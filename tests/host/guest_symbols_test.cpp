#include <elf.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.h"
#include "zb/elf_symbols.h"
#include "zb/guest_symbols.h"

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kFileSize = 0x1000;
constexpr std::uint32_t kBase = 0x1000;
constexpr std::size_t kDynamicOffset = 0x100;
constexpr std::size_t kStringOffset = 0x300;
constexpr std::size_t kSymbolOffset = 0x500;
constexpr std::size_t kHashOffset = 0x700;

struct SymbolSpec {
    const char* name;
    bool defined;
    bool weak;
};

template <typename T>
void put(std::vector<std::uint8_t>& bytes, std::size_t offset, const T& value) {
    CHECK(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset);
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}

Elf32_Dyn dyn(std::int32_t tag, std::uint32_t value) {
    Elf32_Dyn entry{};
    entry.d_tag = tag;
    entry.d_un.d_val = value;
    return entry;
}

std::uint32_t append_string(std::vector<std::uint8_t>& bytes, std::size_t& cursor, const char* text) {
    const auto index = static_cast<std::uint32_t>(cursor - kStringOffset);
    const std::size_t size = std::strlen(text) + 1;
    CHECK(cursor + size <= kSymbolOffset);
    std::memcpy(bytes.data() + cursor, text, size);
    cursor += size;
    return index;
}

// A minimal ELF32 ARM shared object with a DT_HASH symbol table: the same shape the real
// scanner sees, without needing an NDK build.
std::vector<std::uint8_t> make_elf(const std::vector<SymbolSpec>& specs) {
    std::vector<std::uint8_t> bytes(kFileSize);
    Elf32_Ehdr eh{};
    std::memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS32;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_DYN;
    eh.e_machine = EM_ARM;
    eh.e_version = EV_CURRENT;
    eh.e_ehsize = sizeof(Elf32_Ehdr);
    eh.e_phoff = sizeof(Elf32_Ehdr);
    eh.e_phentsize = sizeof(Elf32_Phdr);
    eh.e_phnum = 2;
    put(bytes, 0, eh);

    Elf32_Phdr load{};
    load.p_type = PT_LOAD;
    load.p_vaddr = kBase;
    load.p_filesz = static_cast<std::uint32_t>(bytes.size());
    load.p_memsz = load.p_filesz;
    put(bytes, eh.e_phoff, load);

    Elf32_Phdr dynamic{};
    dynamic.p_type = PT_DYNAMIC;
    dynamic.p_offset = kDynamicOffset;
    dynamic.p_vaddr = kBase + kDynamicOffset;
    dynamic.p_filesz = 6 * sizeof(Elf32_Dyn);
    dynamic.p_memsz = dynamic.p_filesz;
    put(bytes, eh.e_phoff + sizeof(Elf32_Phdr), dynamic);

    std::size_t string_cursor = kStringOffset + 1;
    std::vector<Elf32_Sym> symbols(specs.size() + 1);
    for (std::size_t i = 0; i < specs.size(); ++i) {
        Elf32_Sym& symbol = symbols[i + 1];
        symbol.st_name = append_string(bytes, string_cursor, specs[i].name);
        symbol.st_value = 0x2001;
        symbol.st_info = ELF32_ST_INFO(specs[i].weak ? STB_WEAK : STB_GLOBAL, STT_FUNC);
        symbol.st_other = STV_DEFAULT;
        symbol.st_shndx = specs[i].defined ? 1 : SHN_UNDEF;
    }
    const auto string_size = static_cast<std::uint32_t>(string_cursor - kStringOffset);
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        put(bytes, kSymbolOffset + i * sizeof(Elf32_Sym), symbols[i]);
    }

    const std::vector<Elf32_Dyn> entries = {
        dyn(DT_STRTAB, kBase + kStringOffset), dyn(DT_STRSZ, string_size),
        dyn(DT_SYMTAB, kBase + kSymbolOffset), dyn(DT_SYMENT, sizeof(Elf32_Sym)),
        dyn(DT_HASH, kBase + kHashOffset), dyn(DT_NULL, 0)};
    for (std::size_t i = 0; i < entries.size(); ++i) {
        put(bytes, kDynamicOffset + i * sizeof(Elf32_Dyn), entries[i]);
    }

    const std::uint32_t header[] = {1, static_cast<std::uint32_t>(symbols.size())};
    put(bytes, kHashOffset, header);
    for (std::uint32_t i = 0; i < symbols.size(); ++i) {
        put(bytes, kHashOffset + (2 + 1 + i) * sizeof(std::uint32_t), std::uint32_t{0});
    }
    return bytes;
}

void write_elf(const fs::path& path, const std::vector<SymbolSpec>& specs) {
    const auto bytes = make_elf(specs);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    CHECK(out.good());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    CHECK(out.good());
}

void check_precheck() {
    const fs::path root =
        fs::temp_directory_path() / ("zb-guest-symbols-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "plugin");
    fs::create_directories(root / "guest");
    fs::create_directories(root / "sysroot");

    write_elf(root / "plugin" / "libgame.so",
              {{"game_export", true, false},
               {"platform_fn", false, false},
               {"missing_fn", false, false},
               {"weak_missing", false, true}});
    // A name provided by a sibling plugin library is a valid reference.
    write_elf(root / "plugin" / "libhelper.so",
              {{"helper_export", true, false}, {"also_missing", false, false}});
    // Bundled runtime libraries are providers too.
    write_elf(root / "guest" / "libandroid.so", {{"platform_fn", true, false}});
    write_elf(root / "sysroot" / "libc.so", {{"memcpy", true, false}});
    // A non-ELF ".so" is diagnostics, not a failed import; a non-".so" file is ignored.
    {
        std::ofstream junk(root / "plugin" / "libnotes.so", std::ios::binary);
        junk << "not an elf file";
    }
    {
        std::ofstream ignored(root / "plugin" / "notes.txt");
        ignored << "ignored";
    }

    const zb::GuestSymbolPrecheck check = zb::precheck_guest_libraries(
        (root / "plugin").string(), {(root / "guest").string(), (root / "sysroot").string()});
    CHECK(check.libraries == 3);
    CHECK(check.unreadable.size() == 1);
    CHECK(check.unreadable[0].rfind("libnotes.so: ", 0) == 0);
    // platform_fn resolves through the bundled runtime, weak_missing is allowed to stay null, and
    // game_export/helper_export are plugin definitions. Only two references are really missing.
    CHECK(check.missing_total == 2);
    CHECK(check.missing.size() == 2);
    CHECK(check.missing[0].library == "libgame.so");
    CHECK(check.missing[0].symbol == "missing_fn");
    CHECK(check.missing[1].library == "libhelper.so");
    CHECK(check.missing[1].symbol == "also_missing");
    CHECK(zb::format_guest_symbol_problem(check.missing[0]) == "libgame.so: missing_fn");

    // Without providers, the platform reference is missing too; the cap truncates the list but
    // the total still counts every problem.
    const zb::GuestSymbolPrecheck capped =
        zb::precheck_guest_libraries((root / "plugin").string(), {}, 1);
    CHECK(capped.missing.size() == 1);
    CHECK(capped.missing_total == 3);

    // A directory that does not exist is an empty result, never an error.
    const zb::GuestSymbolPrecheck absent =
        zb::precheck_guest_libraries((root / "absent").string(), {}, 4);
    CHECK(absent.libraries == 0);
    CHECK(absent.missing.empty());
    CHECK(absent.missing_total == 0);

    fs::remove_all(root);
}

}  // namespace

int main() {
    check_precheck();
    std::puts("guest_symbols_test PASS");
    return 0;
}
