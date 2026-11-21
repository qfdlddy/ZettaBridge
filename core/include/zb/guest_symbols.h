#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace zb {

struct GuestSymbolProblem {
    std::string library;  // file name inside the plugin's lib directory, e.g. "libgame.so"
    std::string symbol;
};

// What an import-time symbol precheck found.
struct GuestSymbolPrecheck {
    // Shared libraries scanned in the plugin's own lib directory.
    std::size_t libraries = 0;
    // Distinct names the guest can resolve: the plugin's own libraries plus the bundled runtime.
    std::size_t provided_names = 0;
    // Sorted by library name, then symbol name. At most the requested maximum.
    std::vector<GuestSymbolProblem> missing;
    // Missing references beyond the requested maximum, so a truncated list is visible as such.
    std::size_t missing_total = 0;
    // Files that could not be read as ELF, as "<name>: <reason>". Diagnostics only, never fatal.
    std::vector<std::string> unreadable;
};

// Reports the strong undefined symbols of every *.so directly inside `lib_dir` that neither the
// other plugin libraries nor any *.so directly inside `provider_dirs` defines.
//
// Why: a plugin library is linked onto the guest with BIND_NOW, so one strong name the guest
// cannot resolve stops the whole library from loading, and all the launcher sees is
// "JNI_ERR returned from JNI_OnLoad". Checking at import turns that into a list of names before
// the plugin is ever launched.
//
// This is a conservative over-approximation: every plugin library and every provider is treated
// as resolvable even when the guest would not actually load it, so a reported name is a real gap
// while a clean result is not a proof of a successful load.
GuestSymbolPrecheck precheck_guest_libraries(const std::string& lib_dir,
                                             const std::vector<std::string>& provider_dirs,
                                             std::size_t max_problems = 32);

// "libgame.so: glSomeEntryPoint"
std::string format_guest_symbol_problem(const GuestSymbolProblem& problem);

}  // namespace zb
