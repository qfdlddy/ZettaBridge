#include "gl1_driver_backend.h"

#include "zb/log.h"

namespace zb {

// Every entry point is overridden by the generated forwarders, so the portable recording seam
// (the one MockGles1 implements) is never reached in production. One line names the function if
// a newly added GLES 1.1 name ever lands in host_gl1.cpp without regenerating this backend.
std::uint64_t Gl1DriverBackend::invoke(const char* name,
                                       std::initializer_list<std::uint64_t>) {
    log("GLES 1.1 driver forwarder is missing for %s", name);
    return 0;
}

#include "gl1_driver_backend_bodies.inc"

}  // namespace zb
