#include "zb/elf_loader.h"

#include <elf.h>
#include <sys/mman.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

namespace zb {

bool load_elf(GuestMemory& mem, const std::string& path, std::uint32_t dyn_limit, LoadedElf& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    const std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    Elf32_Ehdr eh;
    if (data.size() < sizeof eh) {
        error = "file too small: " + path;
        return false;
    }
    std::memcpy(&eh, data.data(), sizeof eh);
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS32 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_ARM) {
        error = "not a little-endian ARM ELF32: " + path;
        return false;
    }
    if (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) {
        error = "unsupported ELF type in " + path;
        return false;
    }
    if (eh.e_phnum > 1024 ||
        static_cast<std::uint64_t>(eh.e_phoff) + static_cast<std::uint64_t>(eh.e_phnum) * sizeof(Elf32_Phdr) > data.size()) {
        error = "bad program header table in " + path;
        return false;
    }
    std::vector<Elf32_Phdr> phdrs(eh.e_phnum);
    std::memcpy(phdrs.data(), data.data() + eh.e_phoff, phdrs.size() * sizeof(Elf32_Phdr));

    std::uint64_t lo = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t hi = 0;
    for (const auto& p : phdrs) {
        if (p.p_type != PT_LOAD) continue;
        if (p.p_filesz > p.p_memsz || static_cast<std::uint64_t>(p.p_offset) + p.p_filesz > data.size()) {
            error = "bad PT_LOAD in " + path;
            return false;
        }
        lo = std::min<std::uint64_t>(lo, page_round_down(p.p_vaddr));
        hi = std::max<std::uint64_t>(hi, page_round_up(static_cast<std::uint64_t>(p.p_vaddr) + p.p_memsz));
    }
    if (hi == 0 || hi > kGuestSpaceSize) {
        error = "no loadable segments in " + path;
        return false;
    }
    const std::uint64_t span = hi - lo;

    std::uint32_t bias = 0;
    if (eh.e_type == ET_DYN) {
        const std::uint32_t at = mem.find_free(span, dyn_limit);
        if (at == 0) {
            error = "no guest address space for " + path;
            return false;
        }
        bias = at - static_cast<std::uint32_t>(lo);
    } else if (!mem.range_free(static_cast<std::uint32_t>(lo), span)) {
        error = "address range of " + path + " is already in use";
        return false;
    }

    const std::uint32_t start = static_cast<std::uint32_t>(lo) + bias;
    if (!mem.map_anon(start, span, PROT_READ | PROT_WRITE)) {
        error = "cannot map " + path;
        return false;
    }

    const std::uint64_t span_pages = span >> 12;
    std::vector<int> page_prots(static_cast<std::size_t>(span_pages), 0);
    std::vector<std::uint8_t> page_touched(static_cast<std::size_t>(span_pages), 0);

    out = LoadedElf{};
    for (const auto& p : phdrs) {
        if (p.p_type == PT_LOAD) {
            const std::uint32_t seg_vaddr = static_cast<std::uint32_t>(p.p_vaddr + bias);
            const std::uint64_t seg_end_raw = static_cast<std::uint64_t>(seg_vaddr) + p.p_memsz;
            if (seg_vaddr < start || seg_end_raw > static_cast<std::uint64_t>(start) + span ||
                seg_end_raw > kGuestSpaceSize) {
                mem.unmap(start, span);
                error = "PT_LOAD out of mapped range in " + path;
                return false;
            }
            if (p.p_filesz != 0) {
                std::memcpy(mem.base() + seg_vaddr, data.data() + p.p_offset, p.p_filesz);
            }
            const int prot = ((p.p_flags & PF_R) ? PROT_READ : 0) | ((p.p_flags & PF_W) ? PROT_WRITE : 0) |
                             ((p.p_flags & PF_X) ? PROT_EXEC : 0);
            const std::uint32_t seg_start = page_round_down(seg_vaddr);
            const std::uint64_t seg_end = page_round_up(seg_end_raw);
            for (std::uint64_t addr = seg_start; addr < seg_end; addr += kPageSize) {
                const std::size_t idx = static_cast<std::size_t>((addr - start) >> 12);
                page_prots[idx] |= prot;
                page_touched[idx] = 1;
            }
        } else if (p.p_type == PT_INTERP) {
            if (static_cast<std::uint64_t>(p.p_offset) + p.p_filesz > data.size()) {
                mem.unmap(start, span);
                error = "bad PT_INTERP in " + path;
                return false;
            }
            const char* s = data.data() + p.p_offset;
            out.interp.assign(s, strnlen(s, p.p_filesz));
        }
    }
    for (std::size_t idx = 0; idx < page_prots.size();) {
        if (!page_touched[idx]) {
            ++idx;
            continue;
        }
        const int prot = page_prots[idx];
        std::size_t run = idx + 1;
        while (run < page_prots.size() && page_touched[run] && page_prots[run] == prot) ++run;
        const std::uint32_t seg_start = start + static_cast<std::uint32_t>(idx << 12);
        const std::uint64_t seg_len = static_cast<std::uint64_t>(run - idx) << 12;
        if (!mem.protect(seg_start, seg_len, prot)) {
            mem.unmap(start, span);
            error = "cannot protect segment in " + path;
            return false;
        }
        idx = run;
    }

    for (const auto& p : phdrs) {
        if (p.p_type == PT_PHDR) out.phdr = static_cast<std::uint32_t>(p.p_vaddr + bias);
    }
    if (out.phdr == 0) {
        for (const auto& p : phdrs) {
            if (p.p_type == PT_LOAD && eh.e_phoff >= p.p_offset && eh.e_phoff < p.p_offset + p.p_filesz) {
                out.phdr = static_cast<std::uint32_t>(p.p_vaddr + (eh.e_phoff - p.p_offset) + bias);
                break;
            }
        }
    }

    out.bias = bias;
    out.entry = static_cast<std::uint32_t>(eh.e_entry + bias);
    out.phnum = eh.e_phnum;
    out.load_start = start;
    out.load_end = static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(start) + span,
                                                                      kGuestSpaceSize - 1));
    if (static_cast<std::uint64_t>(start) + span < kGuestSpaceSize) {
        out.load_end = static_cast<std::uint32_t>(static_cast<std::uint64_t>(start) + span);
    }
    out.is_dyn = eh.e_type == ET_DYN;
    return true;
}

}  // namespace zb
