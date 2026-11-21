#include "zb/guest_symbols.h"

#include <dirent.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "zb/elf_symbols.h"

namespace zb {

namespace {

bool ends_with_so(const std::string& name) {
    return name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0;
}

// Names of the regular files directly inside `dir` that end in ".so", sorted. A missing or
// unreadable directory yields an empty list: the precheck is advisory and must never fail an
// import.
std::vector<std::string> list_shared_libraries(const std::string& dir) {
    std::vector<std::string> names;
    DIR* handle = ::opendir(dir.c_str());
    if (handle == nullptr) return names;
    while (const dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || !ends_with_so(name)) continue;
        names.push_back(name);
    }
    ::closedir(handle);
    std::sort(names.begin(), names.end());
    return names;
}

void collect_defined(const std::string& path, std::vector<std::string>* unreadable,
                     std::unordered_set<std::string>* names) {
    const ElfSymbolTable table = scan_elf32_symbols(path);
    if (table.status != ElfSymbolStatus::Ok) {
        // Not an ARM ELF32 file (a text asset or a different ABI directory): nothing to compare.
        if (table.status == ElfSymbolStatus::Error) {
            const std::size_t slash = path.find_last_of('/');
            unreadable->push_back((slash == std::string::npos ? path : path.substr(slash + 1)) +
                                  ": " + table.message);
        }
        return;
    }
    for (const ElfSymbol& symbol : table.defined) names->insert(symbol.name);
}

}  // namespace

GuestSymbolPrecheck precheck_guest_libraries(const std::string& lib_dir,
                                             const std::vector<std::string>& provider_dirs,
                                             std::size_t max_problems) {
    GuestSymbolPrecheck result;
    const std::vector<std::string> libraries = list_shared_libraries(lib_dir);
    result.libraries = libraries.size();

    std::unordered_set<std::string> provided;
    std::vector<ElfSymbolTable> tables;
    tables.reserve(libraries.size());
    for (const std::string& name : libraries) {
        ElfSymbolTable table = scan_elf32_symbols(lib_dir + "/" + name);
        if (table.status != ElfSymbolStatus::Ok) {
            // Not an ARM ELF32 file (a text asset or a different ABI directory): nothing to
            // compare, and a malformed one is diagnostics, not a failed import.
            if (table.status == ElfSymbolStatus::Error) {
                result.unreadable.push_back(name + ": " + table.message);
            }
        } else {
            for (const ElfSymbol& symbol : table.defined) provided.insert(symbol.name);
        }
        tables.push_back(std::move(table));
    }
    for (const std::string& dir : provider_dirs) {
        for (const std::string& name : list_shared_libraries(dir)) {
            collect_defined(dir + "/" + name, &result.unreadable, &provided);
        }
    }
    result.provided_names = provided.size();

    for (std::size_t i = 0; i < libraries.size(); ++i) {
        for (const ElfSymbol& symbol : tables[i].undefined) {
            if (provided.count(symbol.name) != 0) continue;
            ++result.missing_total;
            if (result.missing.size() < max_problems) {
                result.missing.push_back(GuestSymbolProblem{libraries[i], symbol.name});
            }
        }
    }
    std::sort(result.missing.begin(), result.missing.end(),
              [](const GuestSymbolProblem& a, const GuestSymbolProblem& b) {
                  if (a.library != b.library) return a.library < b.library;
                  return a.symbol < b.symbol;
              });
    return result;
}

std::string format_guest_symbol_problem(const GuestSymbolProblem& problem) {
    return problem.library + ": " + problem.symbol;
}

}  // namespace zb
