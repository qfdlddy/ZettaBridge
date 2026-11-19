#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "zb/guest_thread.h"
#include "zb/library_runtime.h"
#include "zb/native_call.h"
#include "zb/opensl_backend.h"

namespace zb {

// OpenSL ES host-call dispatcher (Phase 7c): slCreateEngine plus the fifteen slot thunks
// (core/include/zb/opensl_hostcalls.h). Chained into LibraryRuntime::set_host_call_handler
// alongside HostGl/HostAssets/HostJni, the same way GuestJniEngine wires them.
//
// OpenSL has no flat C API. Every method is a slot in an interface vtable, and the guest library
// has exactly one shared vtable (guest/stubs/libOpenSLES.S), so a slot number alone cannot say
// which method was called. The guest therefore holds a host-allocated struct:
//
//     struct { SLObjectItf_ const *const *vtable; uint32_t handle; uint32_t kind; };
//
// The guest reads only offset 0 (the vtable); `kind` at offset 8 is the host's, and the pair
// (kind, slot) names the method. `handle` is a table index into the objects created here, so no
// host pointer ever crosses into the guest address space.
//
// The guest library reports the vtable's guest address in r12 to slCreateEngine (r12 is
// call-clobbered in AAPCS32 and OpenSL's own ABI never uses it), and every later struct reuses
// that address.
class HostOpenSl {
public:
    using GuestAllocator = std::function<std::optional<std::uint32_t>(std::size_t)>;
    // Executes a guest function from a non-guest host thread (a backend buffer callback).
    // The default posts it to the runtime's service thread.
    using GuestInvoker =
        std::function<std::optional<GuestResult>(std::uint32_t function, const GuestCall& args)>;

    HostOpenSl(LibraryRuntime& runtime, OpenSlBackend& backend, GuestAllocator allocator = {},
               GuestInvoker invoker = {});
    ~HostOpenSl();
    HostOpenSl(const HostOpenSl&) = delete;
    HostOpenSl& operator=(const HostOpenSl&) = delete;

    // Serves ZB_OPENSL_HC_* indices (579-594); false for any other index.
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

    // True once slCreateEngine has run, i.e. the vtable address is known.
    bool ready() const;

private:
    // Structured the way the guest struct is laid out: three words at the guest address.
    enum class Kind : std::uint32_t {
        EngineObject = 1,
        EngineInterface = 2,
        MixObject = 3,
        PlayerObject = 4,
        PlayInterface = 5,
        QueueInterface = 6,
        ConfigInterface = 7,
        RecordInterface = 8,
        VolumeInterface = 9,
        ObjectInterface = 10,
    };

    // One OpenSL object (engine, output mix, audio player). Interfaces are entries in
    // `interfaces`, keyed by the Kind of the interface object the guest holds.
    struct Entry {
        void* object = nullptr;
        std::unordered_map<std::uint32_t, void*> interfaces;
        std::vector<std::uint32_t> guest_structs;
    };

    struct CallbackContext {
        HostOpenSl* host = nullptr;
        std::uint32_t queue_struct = 0;
        std::uint32_t guest_callback = 0;
        std::uint32_t guest_context = 0;
        std::atomic<bool> active{true};
    };

    // A stopped dispatch context: the guest words are captured before the result is cleared,
    // and stack arguments are read from the captured sp.
    class Call {
    public:
        Call(HostOpenSl& host, GuestThread& thread, std::uint32_t index);
        std::uint32_t arg(unsigned position);
        std::uint32_t index() const { return index_; }
        GuestThread& thread() { return thread_; }
        void set_result(std::uint32_t value) { thread_.regs()[0] = value; }

    private:
        HostOpenSl& host_;
        GuestThread& thread_;
        std::uint32_t index_;
        std::uint32_t regs_[4];
        std::uint32_t sp_;
        bool valid_ = true;
    };

    bool dispatch_engine_create(Call& call);
    bool dispatch_slot(Call& call);

    bool object_slot(Call& call, std::uint32_t handle, unsigned slot);
    bool engine_slot(Call& call, std::uint32_t handle, unsigned slot);
    bool play_slot(Call& call, std::uint32_t handle, unsigned slot);
    bool queue_slot(Call& call, std::uint32_t handle, unsigned slot);
    bool config_slot(Call& call, std::uint32_t handle, unsigned slot);

    std::optional<std::uint32_t> allocate_guest(std::size_t size);
    void free_guest(std::uint32_t address);
    // Reads the {vtable, handle, kind} struct the guest points at.
    bool read_guest_struct(std::uint32_t address, std::uint32_t& handle, Kind& kind);
    // Allocates and fills a new guest struct for `handle`.
    std::optional<std::uint32_t> make_guest_struct(std::uint32_t handle, Kind kind);

    std::uint32_t read_word(std::uint32_t address, bool& ok);
    bool write_word(std::uint32_t address, std::uint32_t value);
    // Reads a guest struct of `words` 32-bit words into `out`; false if unreadable.
    bool read_words(std::uint32_t address, std::uint32_t* out, std::size_t words);

    bool register_queue_callback(Call& call, std::uint32_t handle, std::uint32_t queue_struct);
    void drop_queue_callback(std::uint32_t queue_struct);
    static void buffer_callback(void* context);

    GuestResult invoke_guest(std::uint32_t function, std::uint32_t a0, std::uint32_t a1);
    void note_failure(const char* operation, std::uint32_t result);
    // Records why a second (or later) slCreateEngine was refused: after a route change FMOD tears
    // the whole graph down and rebuilds it, so a refusal here is permanent silence, not a hiccup.
    void note_create_reject(const char* reason, std::uint32_t vtable);
    // Bounded call census and a short ring of the most recent control calls. A route change
    // (Bluetooth connect, headset unplug) makes the guest tear the output device down and build
    // it again, and the "audio-*" lines above only keep the latest state of each object, so a
    // silent run cannot show which calls happened or which one started failing.
    void note_call(const char* name, std::uint32_t result, std::uint32_t detail = 0);
    void publish_call_census();
    // Peak amplitude of the buffers the guest actually hands to Enqueue. The AudioTrack log on the
    // device says whether the mixer consumes data, but not whether that data is silence, so this
    // separates "the guest feeds zeros" (a guest-side audio stall) from "our track stopped"
    // (a bridge bug). Only reports, never changes what reaches the driver.
    void note_samples(const std::uint8_t* data, std::uint32_t size, std::uint64_t count);
    // Republished periodically while buffers flow: the "audio-stream" report line is the only
    // proof on a device that sound keeps moving after the first buffer, not just that it started.
    void note_stream();

    LibraryRuntime& runtime_;
    OpenSlBackend& backend_;
    GuestAllocator allocator_;
    GuestInvoker invoker_;

    mutable std::mutex mutex_;
    // Every vtable this bridge has accepted. The guest stub library can be loaded again after a
    // full teardown (FMOD does exactly that when the audio route changes), and a second load
    // relocates the table, so the address is not a stable identity across engine lifetimes.
    // `vtable_` is the most recently accepted one, written into new guest structs.
    static constexpr std::size_t kMaxVtables = 4;
    std::uint32_t vtables_[kMaxVtables] = {};
    std::size_t vtable_count_ = 0;
    std::uint32_t vtable_ = 0;
    std::uint32_t next_handle_ = 1;
    std::unordered_map<std::uint32_t, Entry> objects_;
    // Keyed by the guest queue struct address, so a fired callback finds its registration.
    std::unordered_map<std::uint32_t, std::shared_ptr<CallbackContext>> callbacks_;
    // Retired contexts remain alive until HostOpenSl destruction so buffer_callback can read
    // context->active lock-free without acquiring mutex_ on the RT audio thread.
    std::vector<std::shared_ptr<CallbackContext>> retired_callbacks_;
    std::size_t active_queue_objects_ = 0;
    std::atomic<std::uint32_t> fast_queue_handle_{0};
    std::atomic<void*> fast_queue_ptr_{nullptr};

    // Bounded diagnostics: the device log is the only place a missing sound card shows up.
    std::atomic<bool> logged_engine_{false};
    std::atomic<bool> logged_player_{false};
    std::atomic<bool> logged_enqueue_{false};
    std::atomic<bool> logged_callback_{false};
    // Engine/player create, destroy and setPlay are control calls: a device log line each keeps
    // them on the same timeline as the platform's own route-change lines.
    static constexpr unsigned kMaxControlLogs = 12;
    std::atomic<unsigned> control_logs_{0};
    std::atomic<std::uint64_t> enqueue_count_{0};
    std::atomic<std::uint64_t> callback_count_{0};
    std::atomic<std::uint32_t> enqueue_min_bytes_{0};
    std::atomic<std::uint32_t> enqueue_max_bytes_{0};
    std::atomic<std::uint64_t> last_callback_ns_{0};
    std::atomic<std::uint64_t> callback_max_us_{0};
    std::atomic<std::uint64_t> callback_gap_max_us_{0};
    std::atomic<std::uint64_t> callback_gap_slow_{0};
    std::atomic<std::uint64_t> callback_slow_{0};
    // Sample content of what the guest hands to Enqueue (see note_samples).
    std::atomic<std::uint64_t> sample_buffers_{0};
    std::atomic<std::uint64_t> sample_nonzero_{0};
    std::atomic<std::uint64_t> sample_dropouts_{0};
    std::atomic<std::uint64_t> silent_run_{0};
    std::atomic<unsigned> sample_peak_{0};
    std::atomic<unsigned> sample_max_peak_{0};
    static constexpr std::size_t kMaxTransitions = 4;
    std::atomic<unsigned> transitions_{0};
    std::atomic<unsigned> silent_transitions_{0};
    std::mutex sample_mutex_;
    std::atomic<bool> sample_loud_{false};
    std::shared_ptr<std::atomic<bool>> alive_{std::make_shared<std::atomic<bool>>(true)};
};

}  // namespace zb
