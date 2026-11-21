#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "zb/gl1_backend.h"

// Records every GLES 1.1 call the dispatcher forwards, with the words that arrived from the
// guest. The seam's default bodies are what make this a one-function mock: the Android backend
// overrides each entry point directly, but a test wants the name and the arguments.
class MockGles1 final : public zb::Gl1Backend {
public:
    struct Call {
        std::string name;
        std::vector<std::uint64_t> arguments;
    };

    const std::vector<Call>& calls() const { return calls_; }
    void clear_calls() { calls_.clear(); }

protected:
    std::uint64_t invoke(const char* name,
                         std::initializer_list<std::uint64_t> arguments) override;

private:
    std::vector<Call> calls_;
};
