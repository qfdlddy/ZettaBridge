#pragma once

#include <cstdint>

namespace zb {

enum class CpuRole : std::uint8_t {
    MainThread = 0,
    AudioThread = 1,
};

// Registers a host thread tid (0 = caller) under `role` and applies big-core affinity now when
// running in a foreground cpuset on an asymmetric big.LITTLE SoC. Lock-free on repeated calls.
void register_tuned_thread(std::int32_t tid, CpuRole role);

// Checks whether the current host audio callback thread needs its foreground affinity applied or
// refreshed after a foreground resume. Lock-free relaxed atomic check in steady state.
void ensure_audio_thread_tuned();

// Called from eglSwapBuffers after each swap to refresh foreground big-core affinity when resuming
// from background/idle or periodically as new audio/worker threads spawn.
void note_frame_swap_for_tuning(std::uint64_t now_ns);

// True if `tid` was registered as an AudioThread.
bool is_audio_thread_tid(std::int32_t tid);

}  // namespace zb
