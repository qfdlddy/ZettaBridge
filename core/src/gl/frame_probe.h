#pragma once

#include <cstdint>

namespace zb {

// Frame-cost probe. Splits every presented frame into the three parts that decide whether the
// guest is display-bound, bridge-bound or guest-bound:
//   swap     -- time inside the driver's eglSwapBuffers (waiting for the display/compositor)
//   host-gl  -- time inside every guest->host GL call made since the previous swap
//   guest    -- the remainder (Unity/il2cpp work under translation, syscalls, waits)
// and publishes one "perf-<key>" line per window through RuntimeReport. Measurement only: it
// changes no GL behaviour. Called from the render thread on every swap, and from whichever
// thread issues GL calls, so the accumulators are atomics.

#if defined(ZB_NO_TELEMETRY)
inline void frame_probe_host_call(const char*, std::uint64_t) {}
inline std::uint64_t frame_probe_call_now_ticks() { return 0; }
inline void frame_probe_host_call_ticks(const char*, std::uint64_t) {}
inline void frame_probe_swap(std::uint64_t, std::uint64_t) {}
inline void frame_probe_add_map_copy(std::uint64_t) {}
inline void frame_probe_add_driver_query() {}
inline std::uint64_t frame_probe_now_ns() { return 0; }
inline std::uint64_t frame_probe_last_swap_ns() { return 0; }
inline std::uint64_t frame_probe_total_swaps() { return 0; }
#else
// One guest->host GL call: its duration and the name of the GLES entry point (a static string
// owned by the generated dispatch table, so it stays valid for the next swap to read). Adds to
// the frame's total GL-call time as well.
void frame_probe_host_call(const char* name, std::uint64_t ns);

// Fast per-call timing on ARM64 via cntvct_el0 (converted to nanoseconds once per swap).
std::uint64_t frame_probe_call_now_ticks();
void frame_probe_host_call_ticks(const char* name, std::uint64_t ticks);

// One eglSwapBuffers. `before` is the monotonic clock immediately before the driver call,
// `after` immediately after it returned.
void frame_probe_swap(std::uint64_t before_ns, std::uint64_t after_ns);

// Bytes the mapped-buffer mirror copied on the glFlushMappedBufferRange/glUnmapBuffer path,
// and driver state queries the mirror path issued to name the mapped buffer.
void frame_probe_add_map_copy(std::uint64_t bytes);
void frame_probe_add_driver_query();

// Monotonic nanoseconds, the same clock frame_probe_swap() expects.
std::uint64_t frame_probe_now_ns();

// Last completed eglSwapBuffers monotonic timestamp (ns) and total swap count. Lock-free relaxed
// reads for the watchdog's mid-frame stall detector.
std::uint64_t frame_probe_last_swap_ns();
std::uint64_t frame_probe_total_swaps();
#endif

}  // namespace zb
