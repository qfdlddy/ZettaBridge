#pragma once

#include <GLES/gl.h>

#include <cstdint>
#include <initializer_list>

#include "zb/gl1_backend.h"

namespace zb {

// Phase 8 Task 4: a thin passthrough Gl1Backend that calls the real device driver
// (libGLESv1_CM.so). Every marshaling step (pointer bounds checks, the guest-address table that
// glGetPointerv answers from) already happened in HostGl, so each override below is a direct
// forward with no logic of its own.
//
// This and gl1_driver_backend.cpp are the only two files in the tree that include GLES/gl.h.
// They must never share a translation unit with GLES2/gl2.h or GLES3/gl3.h: the two headers
// declare the 58 entry points GLES 1.1 shares with GLES 2.0 with incompatible fixed-point
// overloads, and the macros that pick a profile collide.
class Gl1DriverBackend final : public Gl1Backend {
public:
    // Generated from core/include/zb/gl1_backend.h by tools/gen_gl1_driver.py, which also
    // proves the seam and the host-call table name the same 87 functions in the same order.
#include "gl1_driver_backend_overrides.inc"

protected:
    // Never called: every entry point is overridden directly, so the seam's recording path is
    // unreachable. It logs and returns zero rather than failing silently, because reaching it
    // means a GLES 1.1 name was added to the seam without regenerating the forwarders.
    std::uint64_t invoke(const char* name,
                         std::initializer_list<std::uint64_t> arguments) override;
};

}  // namespace zb
