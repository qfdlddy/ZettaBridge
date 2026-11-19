#include "zb/guest_memory.h"

#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include "zb/log.h"

namespace zb {

namespace {

constexpr std::uint64_t kGuardSize = 64 * 1024;
constexpr std::uint32_t kLowestAllocPage = 0x10000 >> 12;
constexpr std::size_t kTotalPages = static_cast<std::size_t>(kGuestSpaceSize >> 12);

bool range_valid(std::uint32_t addr, std::uint64_t len) {
    return len != 0 && (addr & kPageMask) == 0 && len <= kGuestSpaceSize - static_cast<std::uint64_t>(addr);
}

std::uint8_t flags_from_prot(int prot) {
    std::uint8_t f = kPageMapped;
    if (prot & PROT_READ) f |= kPageRead;
    if (prot & PROT_WRITE) f |= kPageWrite;
    if (prot & PROT_EXEC) f |= kPageExec;
    return f;
}

}  // namespace

int host_prot(int guest_prot) {
    int p = PROT_NONE;
    if (guest_prot & (PROT_READ | PROT_EXEC)) p |= PROT_READ;
    if (guest_prot & PROT_WRITE) p |= PROT_READ | PROT_WRITE;
    return p;
}

GuestMemory::GuestMemory() : pages_(std::make_unique<std::atomic<std::uint8_t>[]>(kTotalPages)) {
    for (std::size_t i = 0; i < kTotalPages; ++i) {
        pages_[i].store(0, std::memory_order_relaxed);
    }
    void* p = mmap(nullptr, kGuestSpaceSize + kGuardSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        log("cannot reserve guest address space: %s", std::strerror(errno));
        return;
    }
    base_ = static_cast<std::uint8_t*>(p);
}

GuestMemory::~GuestMemory() {
    if (base_) munmap(base_, kGuestSpaceSize + kGuardSize);
}

void GuestMemory::set_flags(std::uint32_t addr, std::uint64_t len, std::uint8_t flags) {
    const std::size_t first = static_cast<std::size_t>(addr >> 12);
    const std::size_t count = static_cast<std::size_t>(page_round_up(len) >> 12);
    for (std::size_t i = 0; i < count; ++i) {
        pages_[first + i].store(flags, std::memory_order_release);
    }
}

bool GuestMemory::map_anon(std::uint32_t addr, std::uint64_t len, int prot) {
    len = page_round_up(len);
    if (!ok() || !range_valid(addr, len)) return false;
    const std::uint8_t new_flags = flags_from_prot(prot);
    const std::size_t first = static_cast<std::size_t>(addr >> 12);
    const std::size_t count = static_cast<std::size_t>(len >> 12);
    std::vector<std::uint8_t> saved(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t old = pages_[first + i].load(std::memory_order_relaxed);
        saved[i] = old;
        if ((old & ~new_flags) != 0) {
            pages_[first + i].store(static_cast<std::uint8_t>(old & new_flags), std::memory_order_release);
        }
    }
    void* p = mmap(base_ + addr, len, host_prot(prot), MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        for (std::size_t i = 0; i < count; ++i) {
            pages_[first + i].store(saved[i], std::memory_order_release);
        }
        return false;
    }
    set_flags(addr, len, new_flags);
    return true;
}

bool GuestMemory::map_file(std::uint32_t addr, std::uint64_t len, int prot, int share_flags, int fd, std::uint64_t offset) {
    len = page_round_up(len);
    if (!ok() || !range_valid(addr, len)) return false;
    const std::uint8_t new_flags = flags_from_prot(prot);
    const std::size_t first = static_cast<std::size_t>(addr >> 12);
    const std::size_t count = static_cast<std::size_t>(len >> 12);
    std::vector<std::uint8_t> saved(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t old = pages_[first + i].load(std::memory_order_relaxed);
        saved[i] = old;
        if ((old & ~new_flags) != 0) {
            pages_[first + i].store(static_cast<std::uint8_t>(old & new_flags), std::memory_order_release);
        }
    }
    int flags = (share_flags & (MAP_SHARED | MAP_PRIVATE)) | MAP_FIXED;
    void* p = mmap(base_ + addr, len, host_prot(prot), flags, fd, static_cast<off_t>(offset));
    if (p == MAP_FAILED) {
        for (std::size_t i = 0; i < count; ++i) {
            pages_[first + i].store(saved[i], std::memory_order_release);
        }
        return false;
    }
    set_flags(addr, len, new_flags);
    return true;
}

bool GuestMemory::protect(std::uint32_t addr, std::uint64_t len, int prot) {
    len = page_round_up(len);
    if (!ok() || !range_valid(addr, len) || !accessible(addr, len, 0)) return false;
    const std::uint8_t new_flags = flags_from_prot(prot);
    const std::size_t first = static_cast<std::size_t>(addr >> 12);
    const std::size_t count = static_cast<std::size_t>(len >> 12);
    std::vector<std::uint8_t> saved(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t old = pages_[first + i].load(std::memory_order_relaxed);
        saved[i] = old;
        // Clear any downgraded permission bits before host mprotect restricts access.
        pages_[first + i].store(static_cast<std::uint8_t>(old & new_flags), std::memory_order_release);
    }
    if (mprotect(base_ + addr, len, host_prot(prot)) != 0) {
        for (std::size_t i = 0; i < count; ++i) {
            pages_[first + i].store(saved[i], std::memory_order_release);
        }
        return false;
    }
    set_flags(addr, len, new_flags);
    return true;
}

bool GuestMemory::unmap(std::uint32_t addr, std::uint64_t len) {
    len = page_round_up(len);
    if (!ok() || !range_valid(addr, len)) return false;
    const std::size_t first = static_cast<std::size_t>(addr >> 12);
    const std::size_t count = static_cast<std::size_t>(len >> 12);
    std::vector<std::uint8_t> saved(count);
    for (std::size_t i = 0; i < count; ++i) {
        saved[i] = pages_[first + i].load(std::memory_order_relaxed);
        pages_[first + i].store(0, std::memory_order_release);
    }
    void* p = mmap(base_ + addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        for (std::size_t i = 0; i < count; ++i) {
            pages_[first + i].store(saved[i], std::memory_order_release);
        }
        return false;
    }
    return true;
}

std::uint32_t GuestMemory::find_free(std::uint64_t len, std::uint32_t limit) const {
    const std::uint64_t rounded = page_round_up(len);
    if (rounded > kGuestSpaceSize) return 0;
    const std::uint64_t pages = rounded >> 12;
    const std::uint64_t top = limit >> 12;
    if (pages == 0 || top < kLowestAllocPage || top - kLowestAllocPage < pages) return 0;
    std::uint64_t run = 0;
    std::uint64_t i = top;
    while (i > kLowestAllocPage) {
        --i;
        if (pages_[i].load(std::memory_order_relaxed) == 0) {
            if (++run == pages) return static_cast<std::uint32_t>(i << 12);
        } else {
            run = 0;
        }
    }
    return 0;
}

bool GuestMemory::range_free(std::uint32_t addr, std::uint64_t len) const {
    len = page_round_up(len);
    if (!range_valid(addr, len)) return false;
    std::uint64_t i = addr >> 12;
    const std::uint64_t end = (static_cast<std::uint64_t>(addr) + len) >> 12;
    for (; i < end; ++i) {
        if (pages_[i].load(std::memory_order_relaxed) != 0) return false;
    }
    return true;
}

bool GuestMemory::accessible_span(std::uint64_t i, std::uint64_t last, std::uint8_t mask) const {
    while (i + 7 <= last) {
        const std::uint8_t combined =
            pages_[i].load(std::memory_order_relaxed) &
            pages_[i + 1].load(std::memory_order_relaxed) &
            pages_[i + 2].load(std::memory_order_relaxed) &
            pages_[i + 3].load(std::memory_order_relaxed) &
            pages_[i + 4].load(std::memory_order_relaxed) &
            pages_[i + 5].load(std::memory_order_relaxed) &
            pages_[i + 6].load(std::memory_order_relaxed) &
            pages_[i + 7].load(std::memory_order_relaxed);
        if ((combined & mask) != mask) return false;
        i += 8;
    }
    for (; i <= last; ++i) {
        if ((pages_[i].load(std::memory_order_relaxed) & mask) != mask) return false;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    return true;
}

bool GuestMemory::any_flag(std::uint32_t addr, std::uint64_t len, std::uint8_t flags) const {
    if (len == 0) return false;
    const std::uint64_t avail = kGuestSpaceSize - static_cast<std::uint64_t>(addr);
    const std::uint64_t clamped = std::min<std::uint64_t>(len, avail);
    if (clamped == 0) return false;
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + clamped;
    std::uint64_t i = addr >> 12;
    const std::uint64_t last = (end - 1) >> 12;
    for (; i <= last; ++i) {
        if ((pages_[i].load(std::memory_order_acquire) & flags) == flags) return true;
    }
    return false;
}

}  // namespace zb
