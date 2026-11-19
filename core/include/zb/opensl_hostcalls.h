#pragma once

#include <cstdint>

namespace zb {

// Host-call indices for the OpenSL ES stub library (Phase 7c). Hand-written from
// tools/gen_stubs.py's opensl_names() output (core/src/gen/hostcalls.inc), which is the generated
// source of truth: appending to it must never shift these, so update both together.
//
// OpenSL has no flat C API: every method is a slot in an interface vtable the implementation
// fills in. The arm32 guest library (guest/stubs/libOpenSLES.S) therefore defines one shared
// fifteen-slot vtable whose slots are the thunks below. The host knows which object it created,
// so the pair (object handle, slot) identifies the method:
//
//   slot 0  Realize                 | SetPlayState          | Enqueue        | SetConfiguration
//   slot 1  Resume                  | GetPlayState          | Clear          | GetConfiguration
//   slot 2  GetState                | GetDuration           | GetState       |
//   slot 3  GetInterface            | (record state)        | RegisterCallback |
//   slot 4  RegisterCallback        | RegisterCallback      |                |
//   slot 6  Destroy                 | ...
//
// The slot number alone is ambiguous; core/src/android/host_opensl.cpp resolves it against the
// kind of the object the handle names.
inline constexpr std::uint32_t ZB_OPENSL_HC_slCreateEngine = 579u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_0 = 580u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_1 = 581u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_2 = 582u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_3 = 583u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_4 = 584u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_5 = 585u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_6 = 586u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_7 = 587u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_8 = 588u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_9 = 589u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_10 = 590u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_11 = 591u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_12 = 592u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_13 = 593u;
inline constexpr std::uint32_t ZB_OPENSL_HC_slot_14 = 594u;
inline constexpr std::uint32_t ZB_OPENSL_HC_last = ZB_OPENSL_HC_slot_14;

inline constexpr bool is_opensl_host_call(std::uint32_t index) {
    return index >= ZB_OPENSL_HC_slCreateEngine && index <= ZB_OPENSL_HC_last;
}

// The first slot thunk's index, so a slot number is index - this.
inline constexpr std::uint32_t ZB_OPENSL_HC_first_slot = ZB_OPENSL_HC_slot_0;

}  // namespace zb
