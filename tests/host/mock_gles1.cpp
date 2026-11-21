#include "mock_gles1.h"

std::uint64_t MockGles1::invoke(const char* name,
                                std::initializer_list<std::uint64_t> arguments) {
    calls_.push_back(Call{name, std::vector<std::uint64_t>(arguments)});
    return 0;
}
