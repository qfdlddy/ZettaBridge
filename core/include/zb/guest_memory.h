#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace zb {

inline constexpr std::uint64_t kGuestSpaceSize = 1ULL << 32;
inline constexpr std::uint32_t kPageSize = 4096;
inline constexpr std::uint32_t kPageMask = kPageSize - 1;

enum PageFlags : std::uint8_t {
    kPageRead = 1,
    kPageWrite = 2,
    kPageExec = 4,
    kPageMapped = 8,
};

inline std::uint64_t page_round_up(std::uint64_t v) {
    if (v > ~static_cast<std::uint64_t>(kPageMask)) return ~static_cast<std::uint64_t>(0);
    return (v + kPageMask) & ~static_cast<std::uint64_t>(kPageMask);
}

inline std::uint32_t page_round_down(std::uint32_t v) {
    return v & ~kPageMask;
}

// Host protection for a guest protection: the JIT reads guest code through host memory,
// so guest EXEC implies host READ; host memory is never executable.
int host_prot(int guest_prot);

// Owns the 4 GiB guest address space. A guest address g lives at host address base() + g.
// Every page has a flag byte; unmapped pages stay PROT_NONE inside the reservation.
class GuestMemory {
public:
    GuestMemory();
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    bool ok() const { return base_ != nullptr; }
    std::uint8_t* base() const { return base_; }

    // addr must be page-aligned; len is rounded up to pages; prot uses PROT_* values.
    bool map_anon(std::uint32_t addr, std::uint64_t len, int prot);
    bool map_file(std::uint32_t addr, std::uint64_t len, int prot, int share_flags, int fd, std::uint64_t offset);
    // Fails without changes if any page in the range is unmapped.
    bool protect(std::uint32_t addr, std::uint64_t len, int prot);
    bool unmap(std::uint32_t addr, std::uint64_t len);

    // Highest free range of len bytes ending at or below limit, never below 0x10000. 0 if none.
    std::uint32_t find_free(std::uint64_t len, std::uint32_t limit) const;
    bool range_free(std::uint32_t addr, std::uint64_t len) const;

    // True if every page touched by [addr, addr + len) is mapped with all `need` flags.
    inline bool accessible(std::uint32_t addr, std::uint64_t len, std::uint8_t need) const {
        if (__builtin_expect(len == 0, 0)) return true;
        if (__builtin_expect(len > kGuestSpaceSize - static_cast<std::uint64_t>(addr), 0)) return false;
        const std::uint64_t end = static_cast<std::uint64_t>(addr) + len;
        const std::uint8_t mask = need | kPageMapped;
        const std::uint64_t first = addr >> 12;
        const std::uint64_t last = (end - 1) >> 12;
        if (__builtin_expect(first == last, 1)) {
            return (pages_[first].load(std::memory_order_acquire) & mask) == mask;
        }
        return accessible_span(first, last, mask);
    }
    // True if any page touched by [addr, addr + len) has all bits in `flags`.
    bool any_flag(std::uint32_t addr, std::uint64_t len, std::uint8_t flags) const;
    // Host pointer to [addr, addr + len) if accessible, else nullptr.
    inline std::uint8_t* host_ptr(std::uint32_t addr, std::uint64_t len, std::uint8_t need) const {
        return accessible(addr, len, need) ? base_ + addr : nullptr;
    }
    std::uint8_t page_flags(std::uint32_t addr) const {
        return pages_[addr >> 12].load(std::memory_order_acquire);
    }

private:
    bool accessible_span(std::uint64_t first, std::uint64_t last, std::uint8_t mask) const;
    void set_flags(std::uint32_t addr, std::uint64_t len, std::uint8_t flags);

    std::uint8_t* base_ = nullptr;
    std::unique_ptr<std::atomic<std::uint8_t>[]> pages_;
};

}  // namespace zb
