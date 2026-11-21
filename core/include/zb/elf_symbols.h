#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zb {

enum class ElfSymbolStatus {
    Ok,
    Skipped,
    Error,
};

struct ElfSymbolReport {
    ElfSymbolStatus status = ElfSymbolStatus::Error;
    std::vector<std::string> exports;
    std::string message;
};

struct ElfSymbol {
    std::string name;
    // ELF32_ST_TYPE of the symbol (STT_FUNC, STT_OBJECT, STT_NOTYPE, ...).
    std::uint8_t type = 0;
};

// The dynamic symbol table of an ELF32 ARM file, read without using section headers.
struct ElfSymbolTable {
    ElfSymbolStatus status = ElfSymbolStatus::Error;
    // Defined, externally visible symbols (global or weak, default or protected visibility).
    std::vector<ElfSymbol> defined;
    // Strong external references the dynamic loader must resolve: global, default or protected
    // visibility, SHN_UNDEF. Weak undefined symbols are excluded on purpose - bionic allows them
    // to resolve to null, and the translator already loads libraries that rely on that.
    std::vector<ElfSymbol> undefined;
    std::string message;
};

// Reads the file-backed ELF32 dynamic symbol table. Both vectors keep symbol-table order and
// contain no duplicate names.
ElfSymbolTable scan_elf32_symbols(const std::string& path);

// Only defined, externally visible JNI exports are returned, in symbol-table order.
ElfSymbolReport scan_elf32_jni_exports(const std::string& path);

}  // namespace zb
