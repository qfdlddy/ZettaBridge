#include "zb/guest_io.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

// Parsing and formatting are bounded so a hot guest write loop cannot cost more than a few
// microseconds per record, and so a record can never grow the report without limit.
constexpr std::size_t kMaxRecordBytes = 4096;
constexpr std::size_t kMaxTextChars = 200;
constexpr std::size_t kMaxTagChars = 64;
constexpr int kFastFdCount = 4096;
constexpr std::uint8_t kFdLog = 1;
constexpr std::uint8_t kFdApk = 2;
constexpr std::uint8_t kFdObb = 4;
constexpr std::uint8_t kFdCache = 8;

std::mutex g_mutex;
std::set<int> g_log_fds;
std::set<int> g_apk_fds;
std::set<int> g_obb_fds;
std::set<int> g_cache_fds;
// Lock-free fd lookup table so normal write/read syscalls never contend on g_mutex.
std::array<std::atomic<std::uint8_t>, kFastFdCount> g_fd_flags{};
std::atomic<bool> g_has_overflow_fds{false};
alignas(64) std::atomic<std::uint64_t> g_apk_bytes{0};
std::atomic<std::uint64_t> g_apk_reads{0};
std::atomic<std::uint64_t> g_apk_positioned{0};
std::atomic<std::int64_t> g_apk_note_ms{0};
alignas(64) std::atomic<std::uint64_t> g_cache_hits{0};
std::atomic<std::uint64_t> g_cache_misses{0};
std::atomic<std::uint64_t> g_cache_mmap_hits{0};
std::atomic<std::uint64_t> g_cache_direct{0};
std::atomic<std::int64_t> g_cache_note_ms{0};
std::atomic<bool> g_mmap_enabled{true};

struct alignas(64) ObbStats {
    std::atomic<std::uint64_t> open_ok{0};
    std::atomic<std::uint64_t> open_fail{0};
    std::atomic<std::uint64_t> read_calls{0};
    std::atomic<std::uint64_t> read_bytes{0};
    std::atomic<std::uint64_t> pread_calls{0};
    std::atomic<std::uint64_t> pread_bytes{0};
    std::atomic<std::uint64_t> readv_calls{0};
    std::atomic<std::uint64_t> readv_bytes{0};
    std::atomic<std::uint64_t> seek_calls{0};
    std::atomic<std::uint64_t> op_seq{0};
    std::atomic<std::uint64_t> mmap_calls{0};
    std::atomic<std::uint64_t> mmap_bytes{0};
    std::atomic<std::uint64_t> fstat_calls{0};
    std::atomic<std::uint64_t> eof_count{0};
    std::atomic<std::uint64_t> short_count{0};
    std::atomic<std::uint64_t> err_count{0};
    std::atomic<std::uint64_t> gt2g_count{0};
    std::atomic<std::uint64_t> neg_count{0};
    std::atomic<std::uint64_t> min_off{UINT64_MAX};
    std::atomic<std::uint64_t> max_off{0};
    std::atomic<std::uint64_t> max_len{0};
    std::atomic<std::int64_t> open_note_ms{0};
    std::atomic<std::int64_t> last_note_ms{0};
    std::atomic<bool> first_pread_noted{false};
    std::atomic<bool> first_err_noted{false};
} g_obb;

struct PreadStats {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> cached{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> eof{0};
    std::atomic<std::uint64_t> err{0};
    std::atomic<std::uint64_t> gt2g{0};
    std::atomic<std::uint64_t> neg{0};
    std::atomic<std::uint64_t> max_off{0};
    std::atomic<std::int64_t> last_note_ms{0};
    std::atomic<bool> first_err_noted{false};
    std::atomic<bool> first_gt2g_noted{false};
} g_pread;

void update_min_u64(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    std::uint64_t cur = target.load(std::memory_order_relaxed);
    while (value < cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

void update_max_u64(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    std::uint64_t cur = target.load(std::memory_order_relaxed);
    while (value > cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

std::int64_t monotonic_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
               .count() +
           1;
}

bool throttle_tick(std::atomic<std::int64_t>& last_ms, std::int64_t interval_ms) {
    const std::int64_t now = monotonic_ms();
    std::int64_t prev = last_ms.load(std::memory_order_relaxed);
    if (prev != 0 && now - prev < interval_ms) return false;
    return last_ms.compare_exchange_strong(prev, now, std::memory_order_relaxed);
}

std::string hex64(std::uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string tail_path(const char* path, std::size_t max_len = 120) {
    if (path == nullptr) return "(null)";
    const std::string_view sv(path);
    if (sv.size() <= max_len) return std::string(sv);
    return "..." + std::string(sv.substr(sv.size() - (max_len - 3)));
}

std::string fd_link_tail(int fd) {
    char link[64];
    char target[PATH_MAX];
    std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    const ssize_t n = ::readlink(link, target, sizeof target - 1);
    if (n <= 0) return "?";
    target[n] = '\0';
    return tail_path(target, 96);
}

const char* obb_op_name(ObbIoOp op) {
    switch (op) {
    case ObbIoOp::kRead: return "read";
    case ObbIoOp::kPread64: return "pread64";
    case ObbIoOp::kReadv: return "readv";
    case ObbIoOp::kLseek: return "lseek";
    case ObbIoOp::kLlseek: return "_llseek";
    case ObbIoOp::kMmap2: return "mmap2";
    case ObbIoOp::kFstat64: return "fstat64";
    }
    return "?";
}

void sync_overflow_flag_locked() {
    const bool log_over = !g_log_fds.empty() && *g_log_fds.rbegin() >= kFastFdCount;
    const bool apk_over = !g_apk_fds.empty() && *g_apk_fds.rbegin() >= kFastFdCount;
    const bool obb_over = !g_obb_fds.empty() && *g_obb_fds.rbegin() >= kFastFdCount;
    const bool cache_over = !g_cache_fds.empty() && *g_cache_fds.rbegin() >= kFastFdCount;
    g_has_overflow_fds.store(log_over || apk_over || obb_over || cache_over,
                             std::memory_order_release);
}

bool has_fd_flag(int fd, std::uint8_t bit) {
    if (fd < 0) return false;
    if (fd < kFastFdCount) {
        return (g_fd_flags[static_cast<std::size_t>(fd)].load(std::memory_order_acquire) & bit) != 0;
    }
    if (!g_has_overflow_fds.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (bit == kFdLog) return g_log_fds.count(fd) != 0;
    if (bit == kFdApk) return g_apk_fds.count(fd) != 0;
    if (bit == kFdObb) return g_obb_fds.count(fd) != 0;
    if (bit == kFdCache) return g_cache_fds.count(fd) != 0;
    return false;
}

constexpr std::size_t kApkChunkSize = 64u * 1024u;
constexpr std::size_t kApkMaxCachedReadBytes = 256u * 1024u;
// 16 shards * 32 slots = 512 * 64 KiB = 32 MiB cap, with open-addressing hash & O(1) LRU.
constexpr std::size_t kCacheShards = 16u;
constexpr std::size_t kCacheShardMask = kCacheShards - 1u;
constexpr std::size_t kShardMaxSlots = 32u;
constexpr std::size_t kShardHashBuckets = 64u;
constexpr std::size_t kShardHashMask = kShardHashBuckets - 1u;
constexpr std::size_t kMaxSpareChunks = 4u;
constexpr std::uint8_t kNoSlotIdx = 0xFFu;

struct ApkChunk {
    std::unique_ptr<std::uint8_t[]> data;
    std::uint32_t valid_len = 0;
};

struct ApkBlockKey {
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    std::uint64_t stamp = 0;
    std::uint64_t block_idx = 0;
    bool operator==(const ApkBlockKey& other) const = default;
};

std::uint64_t compute_file_stamp(const struct stat& st) noexcept {
    const std::uint64_t size = static_cast<std::uint64_t>(st.st_size);
    const std::uint64_t mt =
        static_cast<std::uint64_t>(st.st_mtim.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(st.st_mtim.tv_nsec);
    const std::uint64_t ct =
        static_cast<std::uint64_t>(st.st_ctim.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(st.st_ctim.tv_nsec);
    return size ^ (mt * 0x9e3779b97f4a7c15ULL) ^ (ct * 0xbf58476d1ce4e5b9ULL);
}

std::uint64_t hash_block_key(const ApkBlockKey& k) noexcept {
    std::uint64_t h = k.dev * 0x9e3779b97f4a7c15ULL;
    h ^= k.ino * 0xbf58476d1ce4e5b9ULL;
    h ^= k.stamp * 0xd6e8feb86659fd93ULL;
    h ^= k.block_idx * 0x94d049bb133111ebULL;
    h ^= (h >> 30);
    h *= 0xbf58476d1ce4e5b9ULL;
    return h ^ (h >> 32);
}

std::size_t shard_for_key(const ApkBlockKey& k) noexcept {
    // Pairing (block_idx >> 1) puts sequential even/odd block pairs in the same shard.
    const std::uint64_t mixed = (k.block_idx >> 1) ^ (k.ino * 0x9e3779b97f4a7c15ULL) ^ k.dev;
    return static_cast<std::size_t>(mixed) & kCacheShardMask;
}

struct ApkCacheSlot {
    ApkBlockKey key;
    std::uint64_t hash = 0;
    std::shared_ptr<ApkChunk> chunk;
    std::uint8_t prev = kNoSlotIdx;
    std::uint8_t next = kNoSlotIdx;
};

struct alignas(64) CacheShard {
    std::mutex mutex;
    std::array<ApkCacheSlot, kShardMaxSlots> slots{};
    std::array<std::uint8_t, kShardHashBuckets> buckets{};
    std::array<std::shared_ptr<ApkChunk>, kMaxSpareChunks> spares{};
    std::uint8_t spare_count = 0;
    std::uint8_t slot_count = 0;
    std::uint8_t head = kNoSlotIdx;
    std::uint8_t tail = kNoSlotIdx;

    CacheShard() { buckets.fill(kNoSlotIdx); }

    void unlink_locked(std::uint8_t i) {
        const std::uint8_t p = slots[i].prev;
        const std::uint8_t n = slots[i].next;
        if (p != kNoSlotIdx) {
            slots[p].next = n;
        } else {
            head = n;
        }
        if (n != kNoSlotIdx) {
            slots[n].prev = p;
        } else {
            tail = p;
        }
        slots[i].prev = kNoSlotIdx;
        slots[i].next = kNoSlotIdx;
    }

    void push_front_locked(std::uint8_t i) {
        slots[i].prev = kNoSlotIdx;
        slots[i].next = head;
        if (head != kNoSlotIdx) {
            slots[head].prev = i;
        } else {
            tail = i;
        }
        head = i;
    }

    std::uint8_t hash_find_locked(const ApkBlockKey& key, std::uint64_t h) const {
        std::size_t idx = static_cast<std::size_t>(h) & kShardHashMask;
        while (buckets[idx] != kNoSlotIdx) {
            const std::uint8_t s = buckets[idx];
            if (slots[s].hash == h && slots[s].key == key) return s;
            idx = (idx + 1u) & kShardHashMask;
        }
        return kNoSlotIdx;
    }

    void hash_insert_locked(std::uint8_t slot_idx, std::uint64_t h) {
        std::size_t idx = static_cast<std::size_t>(h) & kShardHashMask;
        while (buckets[idx] != kNoSlotIdx) {
            idx = (idx + 1u) & kShardHashMask;
        }
        buckets[idx] = slot_idx;
    }

    void hash_erase_locked(const ApkBlockKey& key, std::uint64_t h) {
        std::size_t idx = static_cast<std::size_t>(h) & kShardHashMask;
        while (buckets[idx] != kNoSlotIdx) {
            const std::uint8_t s = buckets[idx];
            if (slots[s].hash == h && slots[s].key == key) {
                std::size_t gap = idx;
                std::size_t cur = (idx + 1u) & kShardHashMask;
                while (buckets[cur] != kNoSlotIdx) {
                    const std::size_t home =
                        static_cast<std::size_t>(slots[buckets[cur]].hash) & kShardHashMask;
                    if ((cur >= gap) ? (home <= gap || home > cur)
                                     : (home <= gap && home > cur)) {
                        buckets[gap] = buckets[cur];
                        gap = cur;
                    }
                    cur = (cur + 1u) & kShardHashMask;
                }
                buckets[gap] = kNoSlotIdx;
                return;
            }
            idx = (idx + 1u) & kShardHashMask;
        }
    }

    void recycle_locked(std::shared_ptr<ApkChunk>& buf) {
        if (buf && buf.use_count() == 1 && spare_count < kMaxSpareChunks) {
            buf->valid_len = 0;
            spares[spare_count++] = std::move(buf);
        }
    }

    std::shared_ptr<ApkChunk> acquire_buffer() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (spare_count > 0) {
                return std::move(spares[--spare_count]);
            }
        }
        auto fresh = std::make_shared<ApkChunk>();
        fresh->data = std::make_unique<std::uint8_t[]>(kApkChunkSize);
        return fresh;
    }

    void release_buffer(std::shared_ptr<ApkChunk> buf) {
        if (!buf) return;
        std::lock_guard<std::mutex> lock(mutex);
        recycle_locked(buf);
    }

    bool contains(const ApkBlockKey& key) {
        const std::uint64_t h = hash_block_key(key);
        std::lock_guard<std::mutex> lock(mutex);
        return hash_find_locked(key, h) != kNoSlotIdx;
    }

    std::shared_ptr<const ApkChunk> lookup(const ApkBlockKey& key) {
        const std::uint64_t h = hash_block_key(key);
        std::lock_guard<std::mutex> lock(mutex);
        const std::uint8_t pos = hash_find_locked(key, h);
        if (pos == kNoSlotIdx) return nullptr;
        if (pos != head) {
            unlink_locked(pos);
            push_front_locked(pos);
        }
        return slots[pos].chunk;
    }

    std::shared_ptr<const ApkChunk> insert_locked_common(const ApkBlockKey& key, std::uint64_t h,
                                                         std::shared_ptr<ApkChunk> fresh,
                                                         bool promote_existing) {
        const std::uint8_t existing = hash_find_locked(key, h);
        if (existing != kNoSlotIdx) {
            if (promote_existing && existing != head) {
                unlink_locked(existing);
                push_front_locked(existing);
            }
            recycle_locked(fresh);
            return slots[existing].chunk;
        }
        if (slot_count < kShardMaxSlots) {
            const std::uint8_t pos = slot_count++;
            slots[pos].key = key;
            slots[pos].hash = h;
            slots[pos].chunk = std::move(fresh);
            hash_insert_locked(pos, h);
            push_front_locked(pos);
            return slots[pos].chunk;
        }
        const std::uint8_t victim = tail;
        unlink_locked(victim);
        hash_erase_locked(slots[victim].key, slots[victim].hash);
        std::shared_ptr<ApkChunk> old = std::move(slots[victim].chunk);
        slots[victim].key = key;
        slots[victim].hash = h;
        slots[victim].chunk = std::move(fresh);
        hash_insert_locked(victim, h);
        push_front_locked(victim);
        recycle_locked(old);
        return slots[victim].chunk;
    }

    std::shared_ptr<const ApkChunk> insert_or_get(const ApkBlockKey& key,
                                                  std::shared_ptr<ApkChunk> fresh) {
        const std::uint64_t h = hash_block_key(key);
        std::lock_guard<std::mutex> lock(mutex);
        return insert_locked_common(key, h, std::move(fresh), true);
    }

    void insert_if_absent(const ApkBlockKey& key, std::shared_ptr<ApkChunk> fresh) {
        const std::uint64_t h = hash_block_key(key);
        std::lock_guard<std::mutex> lock(mutex);
        (void)insert_locked_common(key, h, std::move(fresh), false);
    }
};

struct ApkBlockCache {
    std::array<CacheShard, kCacheShards> shards;

    CacheShard& shard(const ApkBlockKey& key) { return shards[shard_for_key(key)]; }
};

ApkBlockCache& apk_block_cache() {
    static ApkBlockCache cache;
    return cache;
}

constexpr std::size_t kMaxMappedArchives = 4u;
constexpr std::uint64_t kMinMmapArchiveBytes = 1u << 20;      // 1 MiB
constexpr std::uint64_t kMaxMmapArchiveBytes = 1536ULL << 20;  // 1.5 GiB

struct MappedArchive {
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    std::uint64_t stamp = 0;
    const std::uint8_t* base = nullptr;
    std::size_t size = 0;

    ~MappedArchive() {
        if (base != nullptr && size > 0) {
            ::munmap(const_cast<std::uint8_t*>(base), size);
        }
    }
};

std::mutex g_mmap_mutex;
std::vector<std::shared_ptr<const MappedArchive>> g_mapped_archives;

std::shared_ptr<const MappedArchive> acquire_mapped_archive(int fd, std::uint64_t dev,
                                                            std::uint64_t ino,
                                                            std::uint64_t stamp,
                                                            std::uint64_t file_size) {
    if (!g_mmap_enabled.load(std::memory_order_relaxed)) return nullptr;
    if (file_size < kMinMmapArchiveBytes || file_size > kMaxMmapArchiveBytes) return nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mmap_mutex);
        for (std::size_t i = 0; i < g_mapped_archives.size(); ++i) {
            const auto& m = g_mapped_archives[i];
            if (m->dev == dev && m->ino == ino && m->stamp == stamp) {
                auto found = m;
                if (i + 1u < g_mapped_archives.size()) {
                    g_mapped_archives.erase(g_mapped_archives.begin() +
                                            static_cast<std::ptrdiff_t>(i));
                    g_mapped_archives.push_back(found);
                }
                return found;
            }
        }
    }
    const auto map_size = static_cast<std::size_t>(file_size);
    void* ptr = ::mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        ptr = ::mmap(nullptr, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    }
    if (ptr == MAP_FAILED) return nullptr;
    auto entry = std::make_shared<MappedArchive>();
    entry->dev = dev;
    entry->ino = ino;
    entry->stamp = stamp;
    entry->base = static_cast<const std::uint8_t*>(ptr);
    entry->size = map_size;

    std::lock_guard<std::mutex> lock(g_mmap_mutex);
    for (const auto& existing : g_mapped_archives) {
        if (existing->dev == dev && existing->ino == ino && existing->stamp == stamp) {
            return existing;
        }
    }
    if (g_mapped_archives.size() >= kMaxMappedArchives) {
        g_mapped_archives.erase(g_mapped_archives.begin());
    }
    g_mapped_archives.push_back(entry);
    return entry;
}

struct alignas(64) ApkFdSlot {
    std::mutex mutex;
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    std::uint64_t stamp = 0;
    std::uint64_t file_size = 0;
    // Stored as (block_idx + 1) so 0 == none and g_fast_fd_slots stays in .bss.
    std::uint64_t last_block_plus1 = 0;
    std::int64_t pos = 0;
    std::shared_ptr<const MappedArchive> mapped;
    bool active = false;
    bool pos_valid = false;
    bool kernel_pos_dirty = false;
};

std::array<ApkFdSlot, kFastFdCount> g_fast_fd_slots{};
std::mutex g_overflow_fd_mutex;
std::unordered_map<int, std::shared_ptr<ApkFdSlot>> g_overflow_fd_slots;

void reset_fd_slot_locked(ApkFdSlot& s, std::uint64_t dev, std::uint64_t ino, std::uint64_t stamp,
                          std::uint64_t file_size, std::shared_ptr<const MappedArchive> mapped) {
    s.dev = dev;
    s.ino = ino;
    s.stamp = stamp;
    s.file_size = file_size;
    s.last_block_plus1 = 0;
    s.pos = 0;
    s.mapped = std::move(mapped);
    s.active = true;
    s.pos_valid = true;
    s.kernel_pos_dirty = false;
}

void deactivate_fd_slot(int fd) {
    if (fd < 0) return;
    if (fd < kFastFdCount) {
        ApkFdSlot& slot = g_fast_fd_slots[static_cast<std::size_t>(fd)];
        std::lock_guard<std::mutex> lock(slot.mutex);
        slot.active = false;
        slot.pos_valid = false;
        slot.mapped.reset();
        return;
    }
    std::lock_guard<std::mutex> lock(g_overflow_fd_mutex);
    g_overflow_fd_slots.erase(fd);
}

void activate_fd_slot(int fd, const struct stat& st) {
    if (fd < 0) return;
    const auto dev = static_cast<std::uint64_t>(st.st_dev);
    const auto ino = static_cast<std::uint64_t>(st.st_ino);
    const std::uint64_t stamp = compute_file_stamp(st);
    const auto file_size = static_cast<std::uint64_t>(st.st_size);
    auto mapped = acquire_mapped_archive(fd, dev, ino, stamp, file_size);
    if (fd < kFastFdCount) {
        ApkFdSlot& slot = g_fast_fd_slots[static_cast<std::size_t>(fd)];
        std::lock_guard<std::mutex> lock(slot.mutex);
        reset_fd_slot_locked(slot, dev, ino, stamp, file_size, std::move(mapped));
        return;
    }
    auto slot = std::make_shared<ApkFdSlot>();
    reset_fd_slot_locked(*slot, dev, ino, stamp, file_size, std::move(mapped));
    std::lock_guard<std::mutex> lock(g_overflow_fd_mutex);
    g_overflow_fd_slots[fd] = std::move(slot);
}

template <typename Fn>
auto with_fd_slot(int fd, Fn&& fn) -> decltype(fn(std::declval<ApkFdSlot&>())) {
    if (fd >= 0 && fd < kFastFdCount) {
        ApkFdSlot& slot = g_fast_fd_slots[static_cast<std::size_t>(fd)];
        std::lock_guard<std::mutex> lock(slot.mutex);
        return fn(slot);
    }
    std::shared_ptr<ApkFdSlot> over;
    if (fd >= kFastFdCount) {
        std::lock_guard<std::mutex> lock(g_overflow_fd_mutex);
        const auto it = g_overflow_fd_slots.find(fd);
        if (it != g_overflow_fd_slots.end()) over = it->second;
    }
    if (!over) {
        ApkFdSlot empty;
        return fn(empty);
    }
    std::lock_guard<std::mutex> lock(over->mutex);
    return fn(*over);
}

void note_cache_stats() {
#if defined(ZB_NO_TELEMETRY)
    return;
#else
    const std::uint64_t hits = g_cache_hits.load(std::memory_order_relaxed);
    const std::uint64_t misses = g_cache_misses.load(std::memory_order_relaxed);
    const std::uint64_t mmap_hits = g_cache_mmap_hits.load(std::memory_order_relaxed);
    const std::uint64_t direct = g_cache_direct.load(std::memory_order_relaxed);
    const std::uint64_t ops = hits + misses + mmap_hits + direct;
    if (ops > 4 && (ops & 63u) != 0) return;
    if (!throttle_tick(g_cache_note_ms, 500)) return;
    const std::uint64_t total_hits = hits + mmap_hits;
    const std::uint64_t lookups = total_hits + misses;
    const std::uint64_t pct = lookups > 0 ? (total_hits * 100u) / lookups : 0u;
    runtime_report().note_io_detail(
        "cache",
        "hits=" + std::to_string(hits) + " misses=" + std::to_string(misses) +
            " mmap_hits=" + std::to_string(mmap_hits) +
            " direct=" + std::to_string(direct) + " hit_pct=" + std::to_string(pct) + "%");
#endif
}

std::int32_t pread_exact_into(int fd, std::uint8_t* dst, std::size_t want, std::uint64_t offset,
                              std::size_t& got_out) {
    got_out = 0;
    while (got_out < want) {
        const ssize_t n = ::pread(fd, dst + got_out, want - got_out,
                                  static_cast<off_t>(offset + got_out));
        if (n > 0) {
            got_out += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        return -errno;
    }
    return 0;
}

std::int32_t preadv_two_chunks(int fd, std::uint8_t* d0, std::uint8_t* d1, std::uint64_t offset,
                               std::size_t& got_out) {
    got_out = 0;
    constexpr std::size_t want = 2u * kApkChunkSize;
    while (got_out < want) {
        struct iovec iov[2];
        int iovcnt = 0;
        if (got_out < kApkChunkSize) {
            iov[0].iov_base = d0 + got_out;
            iov[0].iov_len = kApkChunkSize - got_out;
            iov[1].iov_base = d1;
            iov[1].iov_len = kApkChunkSize;
            iovcnt = 2;
        } else {
            const std::size_t off1 = got_out - kApkChunkSize;
            iov[0].iov_base = d1 + off1;
            iov[0].iov_len = kApkChunkSize - off1;
            iovcnt = 1;
        }
        const ssize_t n = ::preadv(fd, iov, iovcnt, static_cast<off_t>(offset + got_out));
        if (n > 0) {
            got_out += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        return -errno;
    }
    return 0;
}

struct PrefetchJob {
    int dup_fd = -1;
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    std::uint64_t stamp = 0;
    std::uint64_t block_idx = 0;
};

struct AsyncPrefetcher {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<PrefetchJob> queue;
    std::thread worker;
    bool started = false;
    bool stopping = false;

    ~AsyncPrefetcher() {
        std::deque<PrefetchJob> leftover;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            leftover.swap(queue);
        }
        cv.notify_all();
        for (const PrefetchJob& j : leftover) {
            if (j.dup_fd >= 0) ::close(j.dup_fd);
        }
        if (worker.joinable()) worker.join();
    }

    void enqueue(int fd, std::uint64_t dev, std::uint64_t ino, std::uint64_t stamp,
                 std::uint64_t block_idx) {
        const ApkBlockKey key{dev, ino, stamp, block_idx};
        if (apk_block_cache().shard(key).contains(key)) return;
        const int dup_fd = ::dup(fd);
        if (dup_fd < 0) return;
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || queue.size() >= 12) {
            ::close(dup_fd);
            return;
        }
        if (!started) {
            started = true;
            worker = std::thread([this] { run(); });
        }
        queue.push_back(PrefetchJob{dup_fd, dev, ino, stamp, block_idx});
        cv.notify_one();
    }

    void run() {
        ApkBlockCache& cache = apk_block_cache();
        for (;;) {
            PrefetchJob job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping) return;
                job = queue.front();
                queue.pop_front();
            }
            const ApkBlockKey key{job.dev, job.ino, job.stamp, job.block_idx};
            CacheShard& shard = cache.shard(key);
            if (!shard.contains(key)) {
                auto fresh = shard.acquire_buffer();
                std::size_t got = 0;
                const std::int32_t err = pread_exact_into(
                    job.dup_fd, fresh->data.get(), kApkChunkSize, job.block_idx * kApkChunkSize, got);
                if (err == 0 && got > 0) {
                    fresh->valid_len = static_cast<std::uint32_t>(got);
                    shard.insert_if_absent(key, std::move(fresh));
                } else {
                    shard.release_buffer(std::move(fresh));
                }
            }
            ::close(job.dup_fd);
        }
    }
};

AsyncPrefetcher& async_prefetcher() {
    static AsyncPrefetcher prefetcher;
    return prefetcher;
}

constexpr std::size_t kMaxRwCount = 0x7FFFF000u;

std::int32_t copy_from_mapped(const MappedArchive& map, std::uint64_t offset, std::uint8_t* dst,
                              std::size_t len) {
    if (len == 0 || offset >= map.size) return 0;
    const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(std::min<std::size_t>(len, kMaxRwCount), map.size - offset));
    std::memcpy(dst, map.base + offset, take);
    g_cache_mmap_hits.fetch_add(1, std::memory_order_relaxed);
    note_cache_stats();
    return static_cast<std::int32_t>(take);
}

std::int32_t copy_from_apk_cache(int fd, std::uint64_t dev, std::uint64_t ino, std::uint64_t stamp,
                                 std::uint64_t file_size, std::uint64_t offset,
                                 std::uint8_t* dst, std::size_t len, bool seq_hint) {
    if (len == 0 || offset >= file_size) return 0;
    const std::size_t clamped_len = static_cast<std::size_t>(
        std::min<std::uint64_t>(std::min<std::size_t>(len, kMaxRwCount), file_size - offset));
    if (clamped_len > kApkMaxCachedReadBytes) {
        g_cache_direct.fetch_add(1, std::memory_order_relaxed);
        std::size_t got = 0;
        const std::int32_t err = pread_exact_into(fd, dst, clamped_len, offset, got);
        note_cache_stats();
        return got > 0 ? static_cast<std::int32_t>(got) : err;
    }
    const std::uint64_t total_blocks = (file_size + kApkChunkSize - 1u) / kApkChunkSize;
    std::size_t total = 0;
    ApkBlockCache& cache = apk_block_cache();
    while (total < clamped_len) {
        const std::uint64_t cur = offset + total;
        const std::uint64_t block_idx = cur / kApkChunkSize;
        const std::size_t in_block = static_cast<std::size_t>(cur % kApkChunkSize);
        const std::size_t want = std::min(clamped_len - total, kApkChunkSize - in_block);
        const ApkBlockKey key{dev, ino, stamp, block_idx};
        CacheShard& shard0 = cache.shard(key);
        std::shared_ptr<const ApkChunk> chunk = shard0.lookup(key);
        if (chunk) {
            g_cache_hits.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_cache_misses.fetch_add(1, std::memory_order_relaxed);
            const bool multi_block = (clamped_len - total) > want;
            const ApkBlockKey next_key{dev, ino, stamp, block_idx + 1u};
            CacheShard* shard1 = nullptr;
            bool do_ahead = false;
            if ((seq_hint || multi_block) && (block_idx + 1u) < total_blocks) {
                shard1 = &cache.shard(next_key);
                if (!shard1->contains(next_key)) do_ahead = true;
            }
            if (do_ahead) {
                auto fresh0 = shard0.acquire_buffer();
                auto fresh1 = shard1->acquire_buffer();
                std::size_t got = 0;
                const std::int32_t err = preadv_two_chunks(
                    fd, fresh0->data.get(), fresh1->data.get(), block_idx * kApkChunkSize, got);
                if (err < 0) {
                    shard0.release_buffer(std::move(fresh0));
                    shard1->release_buffer(std::move(fresh1));
                    note_cache_stats();
                    return total > 0 ? static_cast<std::int32_t>(total) : err;
                }
                const std::size_t got0 = std::min<std::size_t>(got, kApkChunkSize);
                fresh0->valid_len = static_cast<std::uint32_t>(got0);
                chunk = shard0.insert_or_get(key, std::move(fresh0));
                if (got > kApkChunkSize) {
                    fresh1->valid_len = static_cast<std::uint32_t>(got - kApkChunkSize);
                    shard1->insert_if_absent(next_key, std::move(fresh1));
                } else {
                    shard1->release_buffer(std::move(fresh1));
                }
                if (seq_hint && (block_idx + 2u) < total_blocks) {
                    async_prefetcher().enqueue(fd, dev, ino, stamp, block_idx + 2u);
                }
            } else {
                auto fresh = shard0.acquire_buffer();
                std::size_t got = 0;
                const std::int32_t err = pread_exact_into(
                    fd, fresh->data.get(), kApkChunkSize, block_idx * kApkChunkSize, got);
                if (err < 0) {
                    shard0.release_buffer(std::move(fresh));
                    note_cache_stats();
                    return total > 0 ? static_cast<std::int32_t>(total) : err;
                }
                fresh->valid_len = static_cast<std::uint32_t>(got);
                chunk = shard0.insert_or_get(key, std::move(fresh));
            }
        }
        if (in_block >= chunk->valid_len) break;
        const std::size_t take = std::min(want, static_cast<std::size_t>(chunk->valid_len) - in_block);
        std::memcpy(dst + total, chunk->data.get() + in_block, take);
        total += take;
        if (take < want) break;  // Short block (EOF).
    }
    note_cache_stats();
    return static_cast<std::int32_t>(total);
}

char priority_letter(unsigned priority) {
    switch (priority) {
    case 2: return 'V';
    case 3: return 'D';
    case 4: return 'I';
    case 5: return 'W';
    case 6: return 'E';
    case 7: return 'F';
    default: return '?';
    }
}

// Keeps the text readable in a one-line report: control bytes become '.', a newline becomes the
// separator " / " so a multi-line record still shows as a single report line.
std::string printable(std::string_view text) {
    std::string out;
    out.reserve(std::min(text.size(), kMaxTextChars));
    for (const char c : text) {
        if (out.size() >= kMaxTextChars) break;
        if (c == '\n' || c == '\r') {
            if (out.size() + 3 <= kMaxTextChars && !out.empty() && out.back() != ' ') out += " / ";
            continue;
        }
        const auto byte = static_cast<unsigned char>(c);
        out += (byte >= 0x20 && byte < 0x7F) ? c : '.';
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        if (out.back() == '.' && out.size() > 1 && out[out.size() - 2] != ' ') break;
        out.pop_back();
    }
    return out;
}

void emit(char priority, const std::string& tag, const std::string& text) {
    if (text.empty() && tag.empty()) return;
    std::string line;
    line += '[';
    line += priority;
    line += "] ";
    if (!tag.empty()) {
        line += tag;
        line += ": ";
    }
    line += text;
    runtime_report().note_guest_log_captured();
    runtime_report().note_guest_log_line(line);
}

// One record as written to logd: a `logger_entry` header (len, hdr_size, ...) followed by
// priority, tag and message. liblog may also hand the payload alone to a second iov, so both
// shapes are accepted; when neither matches, the bytes are shown as raw text.
void record(const std::uint8_t* data, std::size_t size, bool from_stderr) {
    if (data == nullptr || size < 3) return;
    if (size > 65536) size = kMaxRecordBytes;
    const auto bytes = std::string_view(reinterpret_cast<const char*>(data), size);

    std::size_t offset = 0;
    if (!from_stderr && size >= 4) {
        const auto length = static_cast<std::uint16_t>(data[0] | (data[1] << 8));
        const auto header = static_cast<std::uint16_t>(data[2] | (data[3] << 8));
        if (header >= 8 && header <= 64) {
            if (length >= header && length == size) {
                offset = header;  // header and payload in one buffer
            } else if (length >= header && length - header == size) {
                offset = 0;  // payload only
            }
        }
    }

    if (offset < size && data[offset] <= 7) {
        const unsigned priority = data[offset++];
        std::size_t tag_end = offset;
        while (tag_end < size && tag_end - offset <= kMaxTagChars && data[tag_end] != 0) ++tag_end;
        if (tag_end < size && tag_end - offset <= kMaxTagChars) {
            const std::string tag(reinterpret_cast<const char*>(data) + offset, tag_end - offset);
            const std::string text = printable(bytes.substr(tag_end + 1));
            if (text.empty() && tag.empty()) return;
            emit(priority_letter(priority), tag, text);
            return;
        }
    }

    const std::string text = printable(from_stderr ? bytes : bytes.substr(offset));
    if (text.find_first_not_of(" .") == std::string::npos) return;  // zero padding, not a record
    emit('?', from_stderr ? "stderr" : "log", text);
}

bool is_log_socket(const void* address, std::uint32_t length) {
    if (address == nullptr || length < 3 || length > 256) return false;
    const auto* sa = static_cast<const struct sockaddr*>(address);
    if (sa->sa_family != AF_UNIX) return false;
    const auto* un = static_cast<const struct sockaddr_un*>(address);
    const char* name = un->sun_path;
    const std::size_t max = length > offsetof(struct sockaddr_un, sun_path)
                                ? length - offsetof(struct sockaddr_un, sun_path)
                                : 0;
    const std::string_view path(name, ::strnlen(name, max));
    // "logdw" is the write side; older guests use the abstract or filesystem form.
    return path.find("logdw") != std::string_view::npos ||
           path.rfind("logd", 0) == 0 || path.find("/logd") != std::string_view::npos;
}

}  // namespace

void guest_log_note_connect(int fd, const void* address, std::uint32_t length) {
    if (fd < 0 || !is_log_socket(address, length)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_log_fds.insert(fd);
    if (fd < kFastFdCount) {
        g_fd_flags[static_cast<std::size_t>(fd)].fetch_or(kFdLog, std::memory_order_release);
    } else {
        sync_overflow_flag_locked();
    }
}

void guest_log_note_close(int fd) {
    if (fd < 0) return;
    if (fd < kFastFdCount) {
        const std::uint8_t cur =
            g_fd_flags[static_cast<std::size_t>(fd)].load(std::memory_order_acquire);
        if (cur == 0) return;
        if ((cur & kFdCache) != 0) deactivate_fd_slot(fd);
    } else {
        if (!g_has_overflow_fds.load(std::memory_order_acquire)) return;
        deactivate_fd_slot(fd);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_log_fds.erase(fd);
    g_apk_fds.erase(fd);
    g_obb_fds.erase(fd);
    g_cache_fds.erase(fd);
    if (fd < kFastFdCount) {
        g_fd_flags[static_cast<std::size_t>(fd)].store(0, std::memory_order_release);
    } else {
        sync_overflow_flag_locked();
    }
}

void guest_log_note_write(int fd, const void* data, std::size_t size) {
    if (fd < 0 || data == nullptr || size == 0) return;
    const bool logs = has_fd_flag(fd, kFdLog);
    const bool stderr_write = fd == 2;
    if (!logs && !stderr_write) return;
    record(static_cast<const std::uint8_t*>(data), size, !logs);
}

void guest_log_note_writev(int fd, const struct iovec* iov, int count) {
    if (fd < 0 || iov == nullptr || count <= 0) return;
    const bool logs = has_fd_flag(fd, kFdLog);
    const bool stderr_write = fd == 2;
    if (!logs && !stderr_write) return;
    std::uint8_t buffer[kMaxRecordBytes];
    std::size_t used = 0;
    for (int i = 0; i < count && used < sizeof buffer; ++i) {
        if (iov[i].iov_base == nullptr || iov[i].iov_len == 0) continue;
        const std::size_t take = std::min(iov[i].iov_len, sizeof buffer - used);
        std::memcpy(buffer + used, iov[i].iov_base, take);
        used += take;
    }
    if (used == 0) return;
    record(buffer, used, !logs);
}

namespace {

void mark_fd_flag_locked(int fd, std::uint8_t bit, std::set<int>& set) {
    set.insert(fd);
    if (fd < kFastFdCount) {
        g_fd_flags[static_cast<std::size_t>(fd)].fetch_or(bit, std::memory_order_release);
    } else {
        sync_overflow_flag_locked();
    }
}

void clear_fd_cache_flag_locked(int fd) {
    g_cache_fds.erase(fd);
    if (fd < kFastFdCount) {
        g_fd_flags[static_cast<std::size_t>(fd)].fetch_and(
            static_cast<std::uint8_t>(~kFdCache), std::memory_order_release);
    } else {
        sync_overflow_flag_locked();
    }
}

bool init_cached_archive_fd(int fd, bool read_only, const struct stat* pre_st = nullptr,
                            std::uint8_t extra_flag_bit = 0,
                            std::set<int>* extra_flag_set = nullptr) {
    struct stat local_st {};
    const struct stat* st = pre_st;
    bool cache_ok = (fd >= 0 && read_only);
    if (cache_ok && st == nullptr) {
        if (::fstat(fd, &local_st) != 0) {
            cache_ok = false;
        } else {
            st = &local_st;
        }
    }
    if (cache_ok && (!S_ISREG(st->st_mode) || st->st_size <= 0)) {
        cache_ok = false;
    }
    if (cache_ok) {
        activate_fd_slot(fd, *st);
    } else {
        deactivate_fd_slot(fd);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (extra_flag_bit != 0 && extra_flag_set != nullptr) {
        mark_fd_flag_locked(fd, extra_flag_bit, *extra_flag_set);
    }
    if (cache_ok) {
        mark_fd_flag_locked(fd, kFdCache, g_cache_fds);
    } else {
        clear_fd_cache_flag_locked(fd);
    }
    return cache_ok;
}

}  // namespace

void guest_io_note_apk_fd(int fd, bool read_only) {
    if (fd < 0) return;
    (void)init_cached_archive_fd(fd, read_only, nullptr, kFdApk, &g_apk_fds);
}

void guest_io_note_dup(int old_fd, int new_fd) {
    if (old_fd < 0 || new_fd < 0 || old_fd == new_fd) return;
    // Flush tracked position on old_fd back to the kernel and disable per-fd cursor tracking on
    // shared open file descriptions so read/lseek through either descriptor remains coherent.
    if (has_fd_flag(old_fd, kFdCache)) {
        with_fd_slot(old_fd, [old_fd](ApkFdSlot& s) {
            if (!s.active) return;
            if (s.pos_valid && s.kernel_pos_dirty) {
                (void)::lseek(old_fd, static_cast<off_t>(s.pos), SEEK_SET);
                s.kernel_pos_dirty = false;
            }
            s.pos_valid = false;
        });
    }
    // dup2 / dup3 silently closes new_fd first; drop any stale APK fd state on new_fd.
    deactivate_fd_slot(new_fd);
    const bool old_zero = old_fd < kFastFdCount &&
                          g_fd_flags[static_cast<std::size_t>(old_fd)].load(std::memory_order_acquire) == 0;
    const bool new_zero = new_fd < kFastFdCount &&
                          g_fd_flags[static_cast<std::size_t>(new_fd)].load(std::memory_order_acquire) == 0;
    if (old_zero && new_zero && !g_has_overflow_fds.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::uint8_t flags = 0;
    if (g_log_fds.count(old_fd) != 0) {
        g_log_fds.insert(new_fd);
        flags |= kFdLog;
    } else {
        g_log_fds.erase(new_fd);
    }
    if (g_apk_fds.count(old_fd) != 0) {
        g_apk_fds.insert(new_fd);
        flags |= kFdApk;
    } else {
        g_apk_fds.erase(new_fd);
    }
    if (g_obb_fds.count(old_fd) != 0) {
        g_obb_fds.insert(new_fd);
        flags |= kFdObb;
    } else {
        g_obb_fds.erase(new_fd);
    }
    if (g_cache_fds.count(old_fd) != 0) {
        g_cache_fds.insert(new_fd);
        flags |= kFdCache;
    } else {
        g_cache_fds.erase(new_fd);
    }
    if (new_fd < kFastFdCount) {
        g_fd_flags[static_cast<std::size_t>(new_fd)].store(flags, std::memory_order_release);
    } else {
        sync_overflow_flag_locked();
    }
}

std::optional<std::int32_t> guest_io_apk_read(int fd, void* buf, std::size_t size) {
    if (fd < 0 || !has_fd_flag(fd, kFdCache)) return std::nullopt;
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    std::uint64_t stamp = 0;
    std::uint64_t file_size = 0;
    std::uint64_t start_pos = 0;
    std::shared_ptr<const MappedArchive> mapped;
    bool seq_hint = false;
    std::size_t planned = 0;
    const std::optional<std::int32_t> early =
        with_fd_slot(fd, [&](ApkFdSlot& s) -> std::optional<std::int32_t> {
            if (!s.active || !s.pos_valid) return -1;  // sentinel mapped to nullopt below
            if (size == 0) return 0;
            if (buf == nullptr) return -EFAULT;
            file_size = s.file_size;
            if (s.pos < 0 || static_cast<std::uint64_t>(s.pos) >= file_size) return 0;
            start_pos = static_cast<std::uint64_t>(s.pos);
            const std::size_t clamped = static_cast<std::size_t>(
                std::min<std::uint64_t>(std::min<std::size_t>(size, kMaxRwCount), file_size - start_pos));
            planned = clamped;
            if (s.mapped) {
                mapped = s.mapped;
            } else {
                dev = s.dev;
                ino = s.ino;
                stamp = s.stamp;
                const std::uint64_t start_block = start_pos / kApkChunkSize;
                const std::uint64_t end_block = (start_pos + clamped - 1u) / kApkChunkSize;
                seq_hint = (s.last_block_plus1 != 0) &&
                           (start_block + 1u == s.last_block_plus1 || start_block == s.last_block_plus1);
                s.last_block_plus1 = end_block + 1u;
            }
            s.pos += static_cast<std::int64_t>(clamped);
            s.kernel_pos_dirty = true;
            return std::nullopt;
        });
    if (early) {
        if (*early == -1) return std::nullopt;
        return early;
    }
    if (mapped) {
        return copy_from_mapped(*mapped, start_pos, static_cast<std::uint8_t*>(buf), planned);
    }
    const std::int32_t got = copy_from_apk_cache(fd, dev, ino, stamp, file_size, start_pos,
                                                 static_cast<std::uint8_t*>(buf), planned, seq_hint);
    const std::size_t actual = got > 0 ? static_cast<std::size_t>(got) : 0u;
    if (actual != planned) {
        with_fd_slot(fd, [&](ApkFdSlot& s) -> int {
            if (s.active && s.pos_valid &&
                s.pos == static_cast<std::int64_t>(start_pos + planned)) {
                s.pos = static_cast<std::int64_t>(start_pos + actual);
                s.kernel_pos_dirty = true;
            }
            return 0;
        });
    }
    return got;
}

std::optional<std::int32_t> guest_io_apk_pread(int fd, void* buf, std::size_t size, off_t offset) {
    if (fd < 0 || !has_fd_flag(fd, kFdCache)) return std::nullopt;
    if (offset < 0) return -EINVAL;
    if (size == 0) return 0;
    if (buf == nullptr) return -EFAULT;
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    std::uint64_t stamp = 0;
    std::uint64_t file_size = 0;
    std::shared_ptr<const MappedArchive> mapped;
    bool seq_hint = false;
    const bool active = with_fd_slot(fd, [&](ApkFdSlot& s) {
        if (!s.active) return false;
        file_size = s.file_size;
        if (s.mapped) {
            mapped = s.mapped;
            return true;
        }
        dev = s.dev;
        ino = s.ino;
        stamp = s.stamp;
        const auto uoff = static_cast<std::uint64_t>(offset);
        if (uoff < file_size) {
            const std::size_t clamped =
                static_cast<std::size_t>(std::min<std::uint64_t>(size, file_size - uoff));
            const std::uint64_t start_block = uoff / kApkChunkSize;
            const std::uint64_t end_block = (uoff + clamped - 1u) / kApkChunkSize;
            seq_hint = (s.last_block_plus1 != 0) &&
                       (start_block + 1u == s.last_block_plus1 || start_block == s.last_block_plus1);
            s.last_block_plus1 = end_block + 1u;
        }
        return true;
    });
    if (!active) return std::nullopt;
    if (mapped) {
        return copy_from_mapped(*mapped, static_cast<std::uint64_t>(offset),
                                static_cast<std::uint8_t*>(buf), size);
    }
    return copy_from_apk_cache(fd, dev, ino, stamp, file_size, static_cast<std::uint64_t>(offset),
                               static_cast<std::uint8_t*>(buf), size, seq_hint);
}

std::optional<std::int32_t> guest_io_apk_readv(int fd, const struct iovec* iov, int count) {
    if (fd < 0 || !has_fd_flag(fd, kFdCache)) return std::nullopt;
    if (count < 0) return -EINVAL;
    return with_fd_slot(fd, [&](ApkFdSlot& s) -> std::optional<std::int32_t> {
        if (!s.active || !s.pos_valid) return std::nullopt;
        if (count == 0) return 0;
        if (iov == nullptr) return -EFAULT;
        std::int64_t total = 0;
        for (int i = 0; i < count; ++i) {
            if (iov[i].iov_len == 0) continue;
            if (iov[i].iov_base == nullptr) {
                return total > 0 ? static_cast<std::int32_t>(total) : -EFAULT;
            }
            const std::size_t remaining_i32 =
                static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) -
                static_cast<std::size_t>(total);
            const std::size_t want = std::min(iov[i].iov_len, remaining_i32);
            if (want == 0) break;
            const std::int32_t res = s.mapped
                ? copy_from_mapped(*s.mapped, static_cast<std::uint64_t>(s.pos),
                                   static_cast<std::uint8_t*>(iov[i].iov_base), want)
                : copy_from_apk_cache(
                      fd, s.dev, s.ino, s.stamp, s.file_size, static_cast<std::uint64_t>(s.pos),
                      static_cast<std::uint8_t*>(iov[i].iov_base), want, true);
            if (res < 0) {
                return total > 0 ? static_cast<std::int32_t>(total) : res;
            }
            if (res == 0) break;
            s.pos += res;
            s.kernel_pos_dirty = true;
            total += res;
            if (static_cast<std::size_t>(res) < want) break;
        }
        return static_cast<std::int32_t>(total);
    });
}

std::optional<std::int64_t> guest_io_apk_lseek(int fd, std::int64_t offset, int whence,
                                               bool limit_32bit) {
    if (fd < 0 || !has_fd_flag(fd, kFdCache)) return std::nullopt;
    return with_fd_slot(fd, [&](ApkFdSlot& s) -> std::optional<std::int64_t> {
        if (!s.active || !s.pos_valid) return std::nullopt;
        std::int64_t base = 0;
        if (whence == SEEK_SET) {
            base = 0;
        } else if (whence == SEEK_CUR) {
            base = s.pos;
        } else if (whence == SEEK_END) {
            if (s.file_size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return -EOVERFLOW;
            }
            base = static_cast<std::int64_t>(s.file_size);
        } else {
            if (s.kernel_pos_dirty) {
                (void)::lseek(fd, static_cast<off_t>(s.pos), SEEK_SET);
                s.kernel_pos_dirty = false;
            }
            s.pos_valid = false;
            return std::nullopt;
        }
        if ((offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset) ||
            (offset < 0 && base < std::numeric_limits<std::int64_t>::min() - offset)) {
            return -EINVAL;
        }
        const std::int64_t next_pos = base + offset;
        if (next_pos < 0) return -EINVAL;
        if (limit_32bit && next_pos > INT32_MAX) return -EOVERFLOW;
        s.pos = next_pos;
        s.kernel_pos_dirty = true;
        return next_pos;
    });
}

bool guest_io_note_read(int fd, std::size_t size, bool positioned) {
#if defined(ZB_NO_TELEMETRY)
    (void)fd;
    (void)size;
    (void)positioned;
    return false;
#else
    if (fd < 0 || size == 0 || !has_fd_flag(fd, kFdApk)) return false;
    if (size > 0x40000000u) size = 0x40000000u;
    const std::uint64_t bytes =
        g_apk_bytes.fetch_add(size, std::memory_order_relaxed) + size;
    const std::uint64_t reads =
        g_apk_reads.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint64_t positioned_count = positioned
        ? (g_apk_positioned.fetch_add(1, std::memory_order_relaxed) + 1)
        : g_apk_positioned.load(std::memory_order_relaxed);
    if (!throttle_tick(g_apk_note_ms, 500)) return true;
    runtime_report().note_io_detail("apk-reads", std::to_string(reads) + " positioned=" +
                                                         std::to_string(positioned_count));
    runtime_report().note_io_detail("apk-mib", std::to_string(bytes / (1024 * 1024)) + " read");
    return true;
#endif
}

std::uint64_t guest_io_apk_bytes() {
    return g_apk_bytes.load(std::memory_order_relaxed);
}

namespace {

void emit_obb_summary() {
    const std::uint64_t r = g_obb.read_calls.load(std::memory_order_relaxed);
    const std::uint64_t p = g_obb.pread_calls.load(std::memory_order_relaxed);
    const std::uint64_t rv = g_obb.readv_calls.load(std::memory_order_relaxed);
    const std::uint64_t rb = g_obb.read_bytes.load(std::memory_order_relaxed);
    const std::uint64_t pb = g_obb.pread_bytes.load(std::memory_order_relaxed);
    const std::uint64_t rvb = g_obb.readv_bytes.load(std::memory_order_relaxed);
    const std::uint64_t total_b = rb + pb + rvb;
    const std::uint64_t seeks = g_obb.seek_calls.load(std::memory_order_relaxed);
    const std::uint64_t mmaps = g_obb.mmap_calls.load(std::memory_order_relaxed);
    const std::uint64_t mmap_b = g_obb.mmap_bytes.load(std::memory_order_relaxed);
    const std::uint64_t fstats = g_obb.fstat_calls.load(std::memory_order_relaxed);
    const std::uint64_t min_off = g_obb.min_off.load(std::memory_order_relaxed);
    const std::uint64_t max_off = g_obb.max_off.load(std::memory_order_relaxed);

    runtime_report().note_io_detail(
        "obb-reads",
        "read=" + std::to_string(r) + " pread=" + std::to_string(p) +
            " readv=" + std::to_string(rv) +
            " eof=" + std::to_string(g_obb.eof_count.load(std::memory_order_relaxed)) +
            " short=" + std::to_string(g_obb.short_count.load(std::memory_order_relaxed)) +
            " err=" + std::to_string(g_obb.err_count.load(std::memory_order_relaxed)));
    runtime_report().note_io_detail(
        "obb-mib",
        "read=" + std::to_string(rb / (1024 * 1024)) +
            " pread=" + std::to_string(pb / (1024 * 1024)) +
            " readv=" + std::to_string(rvb / (1024 * 1024)) +
            " bytes=" + std::to_string(total_b));
    runtime_report().note_io_detail(
        "obb-range",
        "seeks=" + std::to_string(seeks) + " mmaps=" + std::to_string(mmaps) +
            "(" + std::to_string(mmap_b / (1024 * 1024)) + "M) fstats=" + std::to_string(fstats) +
            " min=" + (min_off == UINT64_MAX ? std::string("none") : hex64(min_off)) +
            " max=" + hex64(max_off) +
            " max_len=" + std::to_string(g_obb.max_len.load(std::memory_order_relaxed)) +
            " gt2g=" + std::to_string(g_obb.gt2g_count.load(std::memory_order_relaxed)) +
            " neg=" + std::to_string(g_obb.neg_count.load(std::memory_order_relaxed)));
}

void emit_pread_summary() {
    runtime_report().note_io_detail(
        "pread-all",
        "calls=" + std::to_string(g_pread.calls.load(std::memory_order_relaxed)) +
            " cached=" + std::to_string(g_pread.cached.load(std::memory_order_relaxed)) +
            " bytes=" + std::to_string(g_pread.bytes.load(std::memory_order_relaxed)) +
            " eof=" + std::to_string(g_pread.eof.load(std::memory_order_relaxed)) +
            " err=" + std::to_string(g_pread.err.load(std::memory_order_relaxed)) +
            " gt2g=" + std::to_string(g_pread.gt2g.load(std::memory_order_relaxed)) +
            " neg=" + std::to_string(g_pread.neg.load(std::memory_order_relaxed)) +
            " max_off=" + hex64(g_pread.max_off.load(std::memory_order_relaxed)));
}

}  // namespace

void guest_io_note_obb_open(const char* path, int fd_or_neg_errno, int open_flags) {
    if (fd_or_neg_errno >= 0) {
        const int fd = fd_or_neg_errno;
        struct stat st {};
        const int st_rc = ::fstat(fd, &st);
        const int fstat_err = errno;
        const bool read_only =
            (open_flags & O_ACCMODE) == O_RDONLY && (open_flags & O_PATH) == 0;
        const bool cached = init_cached_archive_fd(
            fd, read_only && st_rc == 0, st_rc == 0 ? &st : nullptr, kFdObb, &g_obb_fds);
#if defined(ZB_NO_TELEMETRY)
        (void)path;
        (void)fstat_err;
        (void)cached;
#else
        const std::uint64_t ok = g_obb.open_ok.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint64_t fail = g_obb.open_fail.load(std::memory_order_relaxed);
        if (ok <= 4 || throttle_tick(g_obb.open_note_ms, 250)) {
            const std::string size_str =
                st_rc == 0 ? std::to_string(static_cast<long long>(st.st_size))
                           : ("fstat_err=" + std::to_string(fstat_err));
            runtime_report().note_io_detail(
                "obb-opens", "ok=" + std::to_string(ok) + " fail=" + std::to_string(fail));
            runtime_report().note_io_detail(
                "obb-open",
                "fd=" + std::to_string(fd) + " flags=" + hex64(static_cast<std::uint32_t>(open_flags)) +
                    " cached=" + std::to_string(cached ? 1 : 0) +
                    " size=" + size_str + " path=" + tail_path(path));
        }
#endif
    } else {
#if defined(ZB_NO_TELEMETRY)
        (void)path;
        (void)open_flags;
#else
        const std::uint64_t fail = g_obb.open_fail.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint64_t ok = g_obb.open_ok.load(std::memory_order_relaxed);
        runtime_report().note_io_detail(
            "obb-opens", "ok=" + std::to_string(ok) + " fail=" + std::to_string(fail));
        runtime_report().note_io_detail(
            "obb-open-fail",
            "errno=" + std::to_string(-fd_or_neg_errno) + " path=" + tail_path(path));
#endif
    }
}

bool guest_io_is_obb_fd(int fd) {
    return has_fd_flag(fd, kFdObb);
}

bool guest_io_is_cached_fd(int fd) {
    return has_fd_flag(fd, kFdCache);
}

void guest_io_set_mmap_enabled(bool enabled) {
    g_mmap_enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        std::lock_guard<std::mutex> lock(g_mmap_mutex);
        g_mapped_archives.clear();
    }
}

bool guest_io_note_obb_io(int fd, ObbIoOp op, std::uint64_t offset, std::size_t req_len,
                          std::int64_t result) {
#if defined(ZB_NO_TELEMETRY)
    (void)fd;
    (void)op;
    (void)offset;
    (void)req_len;
    (void)result;
    return false;
#else
    if (fd < 0 || !has_fd_flag(fd, kFdObb)) return false;

    const bool is_err = result < 0;
    bool is_read_positive = false;
    bool urgent = false;

    if (is_err) {
        g_obb.err_count.fetch_add(1, std::memory_order_relaxed);
        if (!g_obb.first_err_noted.exchange(true, std::memory_order_relaxed)) {
            runtime_report().note_io_detail(
                "obb-first-err",
                std::string("op=") + obb_op_name(op) + " fd=" + std::to_string(fd) +
                    " off=" + hex64(offset) + " len=" + std::to_string(req_len) +
                    " err=" + std::to_string(-result));
            urgent = true;
        }
    }

    switch (op) {
    case ObbIoOp::kRead:
    case ObbIoOp::kPread64:
    case ObbIoOp::kReadv: {
        if (op == ObbIoOp::kRead) {
            g_obb.read_calls.fetch_add(1, std::memory_order_relaxed);
            if (result > 0) g_obb.read_bytes.fetch_add(static_cast<std::uint64_t>(result), std::memory_order_relaxed);
        } else if (op == ObbIoOp::kPread64) {
            g_obb.pread_calls.fetch_add(1, std::memory_order_relaxed);
            if (result > 0) g_obb.pread_bytes.fetch_add(static_cast<std::uint64_t>(result), std::memory_order_relaxed);
            update_min_u64(g_obb.min_off, offset);
            update_max_u64(g_obb.max_off, offset);
            if (static_cast<std::int64_t>(offset) < 0) {
                g_obb.neg_count.fetch_add(1, std::memory_order_relaxed);
            } else if (offset >= 0x80000000ULL) {
                g_obb.gt2g_count.fetch_add(1, std::memory_order_relaxed);
            }
            if (!g_obb.first_pread_noted.exchange(true, std::memory_order_relaxed)) {
                runtime_report().note_io_detail(
                    "obb-first-pread",
                    "fd=" + std::to_string(fd) + " off=" + hex64(offset) +
                        " len=" + std::to_string(req_len) + " res=" + std::to_string(result));
                urgent = true;
            }
        } else {
            g_obb.readv_calls.fetch_add(1, std::memory_order_relaxed);
            if (result > 0) g_obb.readv_bytes.fetch_add(static_cast<std::uint64_t>(result), std::memory_order_relaxed);
        }
        update_max_u64(g_obb.max_len, req_len);
        if (result == 0 && req_len > 0) {
            g_obb.eof_count.fetch_add(1, std::memory_order_relaxed);
        } else if (result > 0 && static_cast<std::size_t>(result) < req_len) {
            g_obb.short_count.fetch_add(1, std::memory_order_relaxed);
        }
        is_read_positive = result > 0;
        break;
    }
    case ObbIoOp::kLseek:
    case ObbIoOp::kLlseek:
        g_obb.seek_calls.fetch_add(1, std::memory_order_relaxed);
        if (result >= 0) {
            update_min_u64(g_obb.min_off, static_cast<std::uint64_t>(result));
            update_max_u64(g_obb.max_off, static_cast<std::uint64_t>(result));
        }
        if (static_cast<std::int64_t>(offset) < 0 && static_cast<int>(req_len) == SEEK_SET) {
            g_obb.neg_count.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    case ObbIoOp::kMmap2:
        g_obb.mmap_calls.fetch_add(1, std::memory_order_relaxed);
        if (!is_err) {
            g_obb.mmap_bytes.fetch_add(req_len, std::memory_order_relaxed);
            update_min_u64(g_obb.min_off, offset);
            update_max_u64(g_obb.max_off, offset);
            if (offset >= 0x80000000ULL) g_obb.gt2g_count.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    case ObbIoOp::kFstat64:
        g_obb.fstat_calls.fetch_add(1, std::memory_order_relaxed);
        break;
    }

    const std::uint64_t seq = g_obb.op_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    if (urgent || ((seq <= 8 || (seq & 31u) == 0) && throttle_tick(g_obb.last_note_ms, 250))) {
        emit_obb_summary();
        if (op == ObbIoOp::kPread64) {
            runtime_report().note_io_detail(
                "obb-last-pread",
                "fd=" + std::to_string(fd) + " off=" + hex64(offset) +
                    " len=" + std::to_string(req_len) + " res=" + std::to_string(result));
        }
    }
    return is_read_positive;
#endif
}

void guest_io_note_pread64(int fd, std::int64_t offset, std::size_t len, std::int32_t res,
                           bool fast_cached) {
#if defined(ZB_NO_TELEMETRY)
    (void)fd;
    (void)offset;
    (void)len;
    (void)res;
    (void)fast_cached;
    return;
#else
    g_pread.calls.fetch_add(1, std::memory_order_relaxed);
    if (fast_cached) g_pread.cached.fetch_add(1, std::memory_order_relaxed);
    if (res > 0) g_pread.bytes.fetch_add(static_cast<std::uint64_t>(res), std::memory_order_relaxed);
    if (res == 0 && len > 0) g_pread.eof.fetch_add(1, std::memory_order_relaxed);

    bool urgent = false;
    if (res < 0) {
        g_pread.err.fetch_add(1, std::memory_order_relaxed);
        if (!g_pread.first_err_noted.exchange(true, std::memory_order_relaxed)) {
            runtime_report().note_io_detail(
                "pread-err-1",
                "fd=" + std::to_string(fd) + " off=" + hex64(static_cast<std::uint64_t>(offset)) +
                    " len=" + std::to_string(len) + " err=" + std::to_string(-res) +
                    " obb=" + std::to_string(has_fd_flag(fd, kFdObb) ? 1 : 0) +
                    " apk=" + std::to_string(has_fd_flag(fd, kFdApk) ? 1 : 0) +
                    " path=" + fd_link_tail(fd));
            urgent = true;
        }
    }
    if (offset < 0) {
        g_pread.neg.fetch_add(1, std::memory_order_relaxed);
    } else {
        const auto uoff = static_cast<std::uint64_t>(offset);
        update_max_u64(g_pread.max_off, uoff);
        if (uoff >= 0x80000000ULL) {
            g_pread.gt2g.fetch_add(1, std::memory_order_relaxed);
            if (!g_pread.first_gt2g_noted.exchange(true, std::memory_order_relaxed)) {
                runtime_report().note_io_detail(
                    "pread-gt2g-1",
                    "fd=" + std::to_string(fd) + " off=" + hex64(uoff) +
                        " len=" + std::to_string(len) + " res=" + std::to_string(res) +
                        " obb=" + std::to_string(has_fd_flag(fd, kFdObb) ? 1 : 0) +
                        " apk=" + std::to_string(has_fd_flag(fd, kFdApk) ? 1 : 0) +
                        " path=" + fd_link_tail(fd));
                urgent = true;
            }
        }
    }
    if (urgent || throttle_tick(g_pread.last_note_ms, 500)) {
        emit_pread_summary();
    }
#endif
}

}  // namespace zb
