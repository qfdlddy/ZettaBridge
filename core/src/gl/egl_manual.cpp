#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cpu_tuning.h"
#include "gl/frame_probe.h"
#include "gl/gl_diagnostics.h"
#include "zb/egl_hostcalls.h"
#include "zb/hang_watchdog.h"
#include "zb/host_egl.h"
#include "zb/log.h"
#include "zb/process.h"
#include "zb/runtime_report.h"

namespace zb {

#if !defined(ZB_NO_TELEMETRY)
// Frame-cost probe (gl/frame_probe.h). One window is long enough to average out a single slow
// frame and short enough that a phase change appears in the report within a few seconds.
namespace {

constexpr std::uint64_t kFrameProbeWindow = 120;

struct FrameProbe {
    std::atomic<std::uint64_t> host_ns{0};
    std::atomic<std::uint64_t> host_ticks{0};
    std::atomic<std::uint64_t> map_copy_bytes{0};
    std::atomic<std::uint64_t> driver_queries{0};
    // Slowest single host GL call since the last swap: its duration and entry-point name. The
    // name is a string literal in the generated dispatch table, so the pointer never dangles.
    std::atomic<std::uint64_t> worst_call_ns{0};
    std::atomic<std::uint64_t> worst_call_ticks{0};
    std::atomic<const char*> worst_call_name{nullptr};
    std::atomic<const char*> worst_tick_call_name{nullptr};
    std::atomic<std::uint64_t> last_swap_ns{0};
    std::atomic<std::uint64_t> total_swaps{0};

    // Render-thread only.
    std::uint64_t swaps = 0;
    std::uint64_t slow_33 = 0;   // frames over 33 ms (missed a 30 Hz deadline)
    std::uint64_t slow_50 = 0;   // frames over 50 ms (visible stutter)
    std::uint64_t slow_100 = 0;  // frames over 100 ms (a freeze)
    std::uint64_t last_swap_end = 0;
    std::uint64_t interval_sum = 0;
    std::uint64_t interval_max = 0;
    std::uint64_t swap_sum = 0;
    std::uint64_t swap_max = 0;
    std::uint64_t host_sum = 0;
    std::uint64_t host_max = 0;
    std::uint64_t map_copy_sum = 0;
    std::uint64_t query_sum = 0;
    std::uint64_t window_frames = 0;
};

FrameProbe& frame_probe() {
    static FrameProbe probe;
    return probe;
}

void publish_frame_probe(FrameProbe& probe) {
    const std::uint64_t frames = probe.window_frames;
    if (frames == 0) return;
    const auto average_ms = [frames](std::uint64_t sum) {
        return static_cast<double>(sum) / static_cast<double>(frames) / 1e6;
    };
    const auto max_ms = [](std::uint64_t value) { return static_cast<double>(value) / 1e6; };
    const double interval_ms = average_ms(probe.interval_sum);
    const double swap_ms = average_ms(probe.swap_sum);
    const double host_ms = average_ms(probe.host_sum);
    // "guest" is what is left of the frame once the driver swap and the bridge's own GL work are
    // subtracted: Unity's rendering logic under translation, syscalls and waits.
    const double guest_ms =
        interval_ms > swap_ms + host_ms ? interval_ms - swap_ms - host_ms : 0.0;

    char text[256];
    std::snprintf(text, sizeof text, "fps=%.1f interval=%.1f/%.1f swap=%.1f/%.1f host-gl=%.1f/%.1f",
                  interval_ms > 0.0 ? 1000.0 / interval_ms : 0.0, interval_ms,
                  max_ms(probe.interval_max), swap_ms, max_ms(probe.swap_max), host_ms,
                  max_ms(probe.host_max));
    // avg/max milliseconds; the second line is the same split as a share of the frame.
    runtime_report().note_gl_detail("perf-frame", text, true);
    std::snprintf(text, sizeof text, "guest=%.1f (%.0f%% of frame) swap=%.0f%% host-gl=%.0f%%",
                  guest_ms, interval_ms > 0.0 ? guest_ms * 100.0 / interval_ms : 0.0,
                  interval_ms > 0.0 ? swap_ms * 100.0 / interval_ms : 0.0,
                  interval_ms > 0.0 ? host_ms * 100.0 / interval_ms : 0.0);
    runtime_report().note_gl_detail("perf-split", text, true);
    std::snprintf(text, sizeof text, "frames=%llu kib-per-frame=%.1f driver-queries-per-frame=%.1f",
                  static_cast<unsigned long long>(frames),
                  static_cast<double>(probe.map_copy_sum) / static_cast<double>(frames) / 1024.0,
                  static_cast<double>(probe.query_sum) / static_cast<double>(frames));
    runtime_report().note_gl_detail("perf-mirror", text, true);
    const char* worst_name = probe.worst_call_name.load(std::memory_order_relaxed);
    std::snprintf(text, sizeof text, "over33=%llu over50=%llu over100=%llu worst-call=%s %.1fms",
                  static_cast<unsigned long long>(probe.slow_33),
                  static_cast<unsigned long long>(probe.slow_50),
                  static_cast<unsigned long long>(probe.slow_100),
                  worst_name != nullptr ? worst_name : "-",
                  static_cast<double>(probe.worst_call_ns.load(std::memory_order_relaxed)) / 1e6);
    runtime_report().note_gl_detail("perf-jank", text, true);

    probe.interval_sum = probe.interval_max = 0;
    probe.swap_sum = probe.swap_max = 0;
    probe.host_sum = probe.host_max = 0;
    probe.map_copy_sum = probe.query_sum = 0;
    probe.slow_33 = probe.slow_50 = probe.slow_100 = 0;
    probe.worst_call_ns.store(0, std::memory_order_relaxed);
    probe.worst_call_name.store(nullptr, std::memory_order_relaxed);
    probe.window_frames = 0;
}

}  // namespace

std::uint64_t frame_probe_now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

#if defined(__aarch64__)
namespace {

inline std::uint64_t frame_probe_counter_freq_hz() {
    static const std::uint64_t freq = [] {
        std::uint64_t val = 0;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
        return val;
    }();
    return freq;
}

inline std::uint64_t frame_probe_ticks_to_ns(std::uint64_t ticks) {
    const std::uint64_t freq = frame_probe_counter_freq_hz();
    if (freq == 0) return ticks;
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(ticks) * 1'000'000'000ULL) /
                                      freq);
}

}  // namespace
#endif

std::uint64_t frame_probe_call_now_ticks() {
#if defined(__aarch64__)
    if (frame_probe_counter_freq_hz() != 0) {
        std::uint64_t val = 0;
        asm volatile("mrs %0, cntvct_el0" : "=r"(val));
        return val;
    }
#endif
    return frame_probe_now_ns();
}

void frame_probe_host_call(const char* name, std::uint64_t ns) {
    FrameProbe& probe = frame_probe();
    probe.host_ns.fetch_add(ns, std::memory_order_relaxed);
    std::uint64_t worst = probe.worst_call_ns.load(std::memory_order_relaxed);
    while (ns > worst) {
        if (probe.worst_call_ns.compare_exchange_weak(worst, ns, std::memory_order_relaxed)) {
            probe.worst_call_name.store(name, std::memory_order_relaxed);
            return;
        }
    }
}

struct ThreadFrameTicks {
    std::uint64_t host_ticks = 0;
    std::uint64_t worst_ticks = 0;
    const char* worst_name = nullptr;
    std::uint32_t pending_calls = 0;

    void flush() {
        if (pending_calls == 0 && host_ticks == 0 && worst_ticks == 0) return;
        FrameProbe& probe = frame_probe();
        if (host_ticks != 0) {
            probe.host_ticks.fetch_add(host_ticks, std::memory_order_relaxed);
        }
        if (worst_ticks != 0) {
            std::uint64_t worst = probe.worst_call_ticks.load(std::memory_order_relaxed);
            while (worst_ticks > worst) {
                if (probe.worst_call_ticks.compare_exchange_weak(worst, worst_ticks,
                                                                  std::memory_order_relaxed)) {
                    probe.worst_tick_call_name.store(worst_name, std::memory_order_relaxed);
                    break;
                }
            }
        }
        host_ticks = 0;
        worst_ticks = 0;
        worst_name = nullptr;
        pending_calls = 0;
    }

    ~ThreadFrameTicks() { flush(); }
};

ThreadFrameTicks& thread_frame_ticks() {
    thread_local ThreadFrameTicks tls;
    return tls;
}

void frame_probe_host_call_ticks(const char* name, std::uint64_t ticks) {
    ThreadFrameTicks& tls = thread_frame_ticks();
    tls.host_ticks += ticks;
    if (ticks > tls.worst_ticks) {
        tls.worst_ticks = ticks;
        tls.worst_name = name;
    }
    if (++tls.pending_calls >= 256) {
        tls.flush();
    }
}

void frame_probe_add_map_copy(std::uint64_t bytes) {
    frame_probe().map_copy_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void frame_probe_add_driver_query() {
    frame_probe().driver_queries.fetch_add(1, std::memory_order_relaxed);
}

void frame_probe_swap(std::uint64_t before_ns, std::uint64_t after_ns) {
    thread_frame_ticks().flush();
    FrameProbe& probe = frame_probe();
    probe.last_swap_ns.store(after_ns, std::memory_order_relaxed);
    probe.total_swaps.fetch_add(1, std::memory_order_relaxed);

    const std::uint64_t swap_ns = after_ns > before_ns ? after_ns - before_ns : 0;
    const std::uint64_t raw_host_ns = probe.host_ns.exchange(0, std::memory_order_relaxed);
    const std::uint64_t raw_host_ticks = probe.host_ticks.exchange(0, std::memory_order_relaxed);
#if defined(__aarch64__)
    const std::uint64_t tick_host_ns = frame_probe_ticks_to_ns(raw_host_ticks);
#else
    const std::uint64_t tick_host_ns = raw_host_ticks;
#endif
    const std::uint64_t host_ns = raw_host_ns + tick_host_ns;
    const std::uint64_t worst_ticks = probe.worst_call_ticks.exchange(0, std::memory_order_relaxed);
    if (worst_ticks > 0) {
#if defined(__aarch64__)
        const std::uint64_t worst_tick_ns = frame_probe_ticks_to_ns(worst_ticks);
#else
        const std::uint64_t worst_tick_ns = worst_ticks;
#endif
        const char* worst_tick_name = probe.worst_tick_call_name.exchange(nullptr, std::memory_order_relaxed);
        std::uint64_t cur_worst = probe.worst_call_ns.load(std::memory_order_relaxed);
        while (worst_tick_ns > cur_worst) {
            if (probe.worst_call_ns.compare_exchange_weak(cur_worst, worst_tick_ns,
                                                          std::memory_order_relaxed)) {
                if (worst_tick_name != nullptr) {
                    probe.worst_call_name.store(worst_tick_name, std::memory_order_relaxed);
                }
                break;
            }
        }
    }
    const std::uint64_t map_copy = probe.map_copy_bytes.exchange(0, std::memory_order_relaxed);
    const std::uint64_t queries = probe.driver_queries.exchange(0, std::memory_order_relaxed);

    if (probe.last_swap_end == 0) {
        // No interval exists before the first swap; start the window here.
        probe.last_swap_end = after_ns;
        return;
    }
    const std::uint64_t interval_ns =
        before_ns > probe.last_swap_end ? before_ns - probe.last_swap_end : 0;
    probe.interval_sum += interval_ns;
    if (interval_ns > probe.interval_max) probe.interval_max = interval_ns;
    if (interval_ns > 33'000'000) ++probe.slow_33;
    if (interval_ns > 50'000'000) ++probe.slow_50;
    if (interval_ns > 100'000'000) ++probe.slow_100;
    probe.swap_sum += swap_ns;
    if (swap_ns > probe.swap_max) probe.swap_max = swap_ns;
    probe.host_sum += host_ns;
    if (host_ns > probe.host_max) probe.host_max = host_ns;
    probe.map_copy_sum += map_copy;
    probe.query_sum += queries;
    probe.last_swap_end = after_ns;
    ++probe.swaps;
    if (++probe.window_frames >= kFrameProbeWindow) publish_frame_probe(probe);
}

std::uint64_t frame_probe_last_swap_ns() {
    return frame_probe().last_swap_ns.load(std::memory_order_relaxed);
}

std::uint64_t frame_probe_total_swaps() {
    return frame_probe().total_swaps.load(std::memory_order_relaxed);
}
#endif  // !ZB_NO_TELEMETRY

namespace {

constexpr std::uint64_t kMaxAttribs = 512;      // 255 pairs plus EGL_NONE
constexpr std::uint64_t kMaxGuestString = 4096;
constexpr std::size_t kMaxDriverString = 64u << 20;

// Local to the runtime-report annotations below; not part of the generated protocol.
constexpr EGLint kEglConfigIdAttrib = 0x3028;
constexpr EGLint kEglContextClientVersionAttrib = 0x3098;
constexpr EGLint kEglWidthAttrib = 0x3057;
constexpr EGLint kEglHeightAttrib = 0x3056;

void* as_pointer(const void* value) { return const_cast<void*>(value); }

// Reads a guest EGL attribute list: 32-bit words up to and including EGL_NONE. A null list
// stays empty and is passed to the driver as nullptr. T is EGLint for the 1.4 entry points and
// EGLAttrib for the 1.5 ones: the guest's EGLAttrib is 32 bits, the host's is 64, so the list is
// always copied and widened rather than aliased.
template <typename T>
bool read_attribs(HostEgl& host, HostEgl::Call& call, unsigned position, std::vector<T>& out) {
    const std::uint32_t address = call.arg(position);
    if (!call.valid()) return false;
    if (address == 0) return true;
    for (std::uint64_t i = 0; i < kMaxAttribs; ++i) {
        const std::uint64_t at = static_cast<std::uint64_t>(address) + 4u * i;
        if (at + 4 > kGuestSpaceSize) break;
        const std::uint8_t* word = host.runtime().memory().host_ptr(
            static_cast<std::uint32_t>(at), 4, kPageRead);
        if (word == nullptr) {
            call.fail(kEglBadParameter, "attribute list is outside readable guest memory");
            return false;
        }
        std::int32_t value;
        std::memcpy(&value, word, sizeof value);
        out.push_back(static_cast<T>(value));
        if (value == kEglNone) return true;
    }
    call.fail(kEglBadParameter, "attribute list is unterminated");
    return false;
}

bool read_guest_string(HostEgl& host, HostEgl::Call& call, unsigned position, std::string& out) {
    const std::uint32_t address = call.arg(position);
    if (!call.valid()) return false;
    if (address == 0) {
        call.fail(kEglBadParameter, "string pointer is null");
        return false;
    }
    std::uint64_t cursor = address;
    while (cursor < kGuestSpaceSize && out.size() <= kMaxGuestString) {
        const std::uint64_t chunk = kPageSize - (cursor & kPageMask);
        const std::uint8_t* bytes = host.runtime().memory().host_ptr(
            static_cast<std::uint32_t>(cursor), chunk, kPageRead);
        if (bytes == nullptr) break;
        const auto* end = static_cast<const std::uint8_t*>(std::memchr(bytes, 0, chunk));
        if (end != nullptr) {
            out.append(reinterpret_cast<const char*>(bytes),
                       static_cast<std::size_t>(end - bytes));
            return true;
        }
        out.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(chunk));
        cursor += chunk;
    }
    call.fail(kEglBadParameter, "string is unreadable or too long");
    return false;
}

// eglGetConfigAttrib / eglQuerySurface / eglQueryContext all shape the same: one object handle
// and one EGLint out-parameter written straight into guest memory. The object class picks the
// backend entry point.
bool query_attribute(HostEgl& host, HostEgl::Call& call, EglObject kind) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    void* object = as_pointer(call.handle_of(1, kind));
    if (!call.valid()) return true;
    const EGLint attribute = call.scalar<EGLint>(2);
    EGLint* value = call.pointer<EGLint>(3, 1u, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    if (value == nullptr) {
        call.fail(kEglBadParameter, "attribute output pointer is null");
        return true;
    }
    EGLBoolean result = 0;
    switch (kind) {
    case EglObject::Config:
        result = host.backend().eglGetConfigAttrib(dpy, object, attribute, value);
        break;
    case EglObject::Surface:
        result = host.backend().eglQuerySurface(dpy, object, attribute, value);
        // Records the surface size the first time the guest asks for it (Task 7): free
        // visibility into what got created, at the cost of no extra backend calls.
        if (result != 0 && (attribute == kEglWidthAttrib || attribute == kEglHeightAttrib)) {
            char detail[32];
            std::snprintf(detail, sizeof detail, "%s=%d",
                          attribute == kEglWidthAttrib ? "width" : "height", *value);
            runtime_report().note_egl_object(
                attribute == kEglWidthAttrib ? "surface-width" : "surface-height", detail);
        }
        break;
    case EglObject::Context:
        result = host.backend().eglQueryContext(dpy, object, attribute, value);
        break;
    case EglObject::Display:
        break;
    }
    call.set_result(result);
    return true;
}

// Shared by eglChooseConfig and eglGetConfigs: the guest array holds 32-bit handles, never host
// EGLConfig pointers, so the driver fills a host vector that is then mapped handle by handle.
struct ConfigOutcome {
    EGLint size = -1;
    EGLint count = -1;
    EGLBoolean ok = 0;
};

bool serve_configs(HostEgl& host, HostEgl::Call& call, EGLDisplay dpy,
                   const std::vector<EGLint>* attribs, unsigned configs_position,
                   ConfigOutcome* outcome = nullptr) {
    const EGLint config_size = call.scalar<EGLint>(configs_position + 1);
    if (outcome != nullptr) outcome->size = config_size;
    if (!call.valid()) return true;
    if (config_size < 0) {
        call.fail(kEglBadParameter, "config array size is negative");
        return true;
    }
    std::uint32_t* guest_configs = call.pointer<std::uint32_t>(
        configs_position, static_cast<std::uint64_t>(config_size), kPageRead | kPageWrite);
    EGLint* num_config = call.pointer<EGLint>(configs_position + 2, 1u, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    if (num_config == nullptr) {
        call.fail(kEglBadParameter, "num_config is null");
        return true;
    }
    std::vector<EGLConfig> found(guest_configs == nullptr ? 0
                                                          : static_cast<std::size_t>(config_size));
    EGLint count = 0;
    const EGLBoolean ok =
        attribs == nullptr
            ? host.backend().eglGetConfigs(dpy, found.empty() ? nullptr : found.data(),
                                           config_size, &count)
            : host.backend().eglChooseConfig(dpy, attribs->empty() ? nullptr : attribs->data(),
                                             found.empty() ? nullptr : found.data(), config_size,
                                             &count);
    if (outcome != nullptr) {
        outcome->count = count;
        outcome->ok = ok;
    }
    if (ok != 0 && guest_configs != nullptr) {
        const std::size_t written =
            std::min<std::size_t>(found.size(), count < 0 ? 0 : static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < written; ++i) {
            guest_configs[i] = host.handle_for(found[i], EglObject::Config);
        }
    }
    *num_config = count;
    call.set_result(ok);
    return true;
}

// EGL 1.5 sync, image and native-pixmap objects are host pointers that the generated protocol
// passes as plain 32-bit scalars, so they cannot cross safely. Nothing in the guest surface path
// uses them; they are rejected rather than truncated.
bool unsupported(HostEgl::Call& call, const char* what) {
    call.fail(kEglBadParameter, what);
    return true;
}

}  // namespace

bool zbegl_manual_eglChooseConfig(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    std::vector<EGLint> attribs;
    if (!read_attribs(host, call, 1, attribs)) return true;
    ConfigOutcome outcome;
    const bool served = serve_configs(host, call, dpy, &attribs, 2, &outcome);
    // Flutter chooses one configuration per context (onscreen, then offscreen) and does not check
    // the second for failure, so record what each request asked for and how many it got.
    static std::atomic<unsigned> chosen{0};
    const unsigned seen = chosen.fetch_add(1) + 1;
    if (seen <= 4) {
        std::string text;
        char word[24];
        for (std::size_t i = 0; i + 1 < attribs.size() && i < 24; i += 2) {
            std::snprintf(word, sizeof word, "%s0x%x=%d", text.empty() ? "" : " ", attribs[i], attribs[i + 1]);
            text += word;
        }
        std::snprintf(word, sizeof word, " -> ok=%d configs=%d/%d", outcome.ok, outcome.count,
                      outcome.size);
        runtime_report().note_egl_object("choose-" + std::to_string(seen), text + word);
    }
    return served;
}

bool zbegl_manual_eglGetConfigs(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    return serve_configs(host, call, dpy, nullptr, 1);
}

bool zbegl_manual_eglGetConfigAttrib(HostEgl& host, HostEgl::Call& call) {
    return query_attribute(host, call, EglObject::Config);
}

bool zbegl_manual_eglQuerySurface(HostEgl& host, HostEgl::Call& call) {
    return query_attribute(host, call, EglObject::Surface);
}

bool zbegl_manual_eglQueryContext(HostEgl& host, HostEgl::Call& call) {
    return query_attribute(host, call, EglObject::Context);
}

bool zbegl_manual_eglQueryString(HostEgl& host, HostEgl::Call& call) {
    const std::uint32_t display_handle = call.arg(0);
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    const EGLint name = call.scalar<EGLint>(1);
    if (!call.valid()) return true;
    // One guest copy per (display, name): the driver string is constant, and the guest keeps the
    // pointer. HostEgl is process-lifetime, so the cache is too.
    static std::mutex mutex;
    static std::map<std::pair<std::uint32_t, EGLint>, std::uint32_t> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = cache.find({display_handle, name});
        if (found != cache.end()) {
            call.set_result(found->second);
            return true;
        }
    }
    const char* source = host.backend().eglQueryString(dpy, name);
    if (source == nullptr) return true;
    const std::size_t length = strnlen(source, kMaxDriverString);
    if (length == kMaxDriverString) {
        call.fail(kEglBadParameter, "driver string exceeds 64 MiB");
        return true;
    }
    const auto address = host.allocate_guest(length + 1);
    if (!address) {
        call.fail(kEglBadAlloc, "guest allocation for the driver string failed");
        return true;
    }
    std::uint8_t* destination =
        host.runtime().memory().host_ptr(*address, length + 1, kPageRead | kPageWrite);
    if (destination == nullptr) {
        call.fail(kEglBadAlloc, "guest allocator returned an unreadable buffer");
        return true;
    }
    std::memcpy(destination, source, length + 1);
    {
        std::lock_guard<std::mutex> lock(mutex);
        cache[{display_handle, name}] = *address;
    }
    call.set_result(*address);
    return true;
}

bool zbegl_manual_eglGetProcAddress(HostEgl& host, HostEgl::Call& call) {
    std::string name;
    if (!read_guest_string(host, call, 0, name)) return true;
    // Only our own gl*/egl* stubs may be handed out, never an arbitrary guest symbol. The guest
    // gets the address of a trap stub, never a host function pointer.
    const bool ours = name.rfind("egl", 0) == 0 || name.rfind("gl", 0) == 0;
    std::uint32_t address = ours ? host.stub_address(name) : 0;
    // A name this bridge does not implement gets the address of a stub that does nothing and
    // answers zero, not null: an engine that stores every resolved pointer and calls through the
    // table without a null check (Unity 4.5, Impeller) jumps to address zero otherwise, and the
    // illegal instruction surfaces much later, far from the probe. The miss is still named below,
    // and each call of the stub is counted as `unsupported-entry-calls`.
    if (address == 0 && ours) address = host.stub_address("zb_unsupported_entry_point");
    call.set_result(address);
    // A miss is what an engine sees as "this GL function does not exist", so name the first few:
    // they are the exact list a later GLES 3 generator pass has to cover.
    if (address == 0) {
        // The first 8 misses name what the engine probed early; the last 8 are kept in a ring
        // because the miss that kills a guest is usually late (Impeller stores a proc table and
        // calls through it without a null check, long after the first probe).
        constexpr std::size_t kRecentMisses = 8;
        static std::mutex miss_mutex;
        static std::string recent[kRecentMisses];
        static unsigned missed = 0;
        std::lock_guard<std::mutex> lock(miss_mutex);
        const unsigned seen = ++missed;
        if (seen <= 8) runtime_report().note_egl_object("procaddress-miss-" + std::to_string(seen), name);
        runtime_report().note_egl_object("procaddress-misses", std::to_string(seen));
        recent[(seen - 1) % kRecentMisses] = name;
        const unsigned kept = seen < kRecentMisses ? seen : static_cast<unsigned>(kRecentMisses);
        for (unsigned i = 0; i < kept; ++i) {
            runtime_report().note_egl_object(
                "procaddress-last-" + std::to_string(i + 1),
                recent[(seen - kept + i) % kRecentMisses]);
        }
    }
    return true;
}

bool zbegl_manual_eglCreateContext(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    EGLConfig config = as_pointer(call.handle_of(1, EglObject::Config));
    if (!call.valid()) return true;
    EGLContext share = as_pointer(call.handle_of(2, EglObject::Context));
    if (!call.valid()) return true;
    std::vector<EGLint> attribs;
    if (!read_attribs(host, call, 3, attribs)) return true;
    EGLContext context = host.backend().eglCreateContext(
        dpy, config, share, attribs.empty() ? nullptr : attribs.data());
    if (context != nullptr) {
        EGLint config_id = -1;
        host.backend().eglGetConfigAttrib(dpy, config, kEglConfigIdAttrib, &config_id);
        EGLint client_version = 1;
        for (std::size_t i = 0; i + 1 < attribs.size(); i += 2) {
            if (attribs[i] == kEglContextClientVersionAttrib) client_version = attribs[i + 1];
        }
        // Flutter creates a resource context that must share objects with the render context;
        // textures uploaded on one are invisible to the other unless share is carried across.
        char detail[128];
        std::snprintf(detail, sizeof detail, "config=%d client-version=%d share-handle=0x%x share=%p tid=%ld",
                      config_id, client_version, call.arg(2), static_cast<const void*>(share),
                      static_cast<long>(::syscall(SYS_gettid)));
        static std::atomic<unsigned> contexts{0};
        runtime_report().note_egl_object("context-" + std::to_string(contexts.fetch_add(1) + 1), detail);
    }
    call.set_handle(context, EglObject::Context);
    return true;
}

bool zbegl_manual_eglCreateWindowSurface(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    EGLConfig config = as_pointer(call.handle_of(1, EglObject::Config));
    if (!call.valid()) return true;
    // The window is an ANativeWindow handle from HostNativeWindow (Task 5), resolved through the
    // WindowResolver seam HostEgl was built with.
    const std::uint32_t window_handle = call.arg(2);
    if (!call.valid()) return true;
    void* window = as_pointer(host.window_for(window_handle));
    if (window == nullptr) {
        call.fail(kEglBadNativeWindow, "unknown native window handle");
        return true;
    }
    std::vector<EGLint> attribs;
    if (!read_attribs(host, call, 3, attribs)) return true;
    EGLSurface surface = host.backend().eglCreateWindowSurface(
        dpy, config, window, attribs.empty() ? nullptr : attribs.data());
    // Size is not queried here: eglQuerySurface is a separate driver call the guest may not make
    // for a while, and this note must not add a call the guest did not ask for.
    if (surface != nullptr) runtime_report().note_egl_object("surface", "created");
    call.set_handle(surface, EglObject::Surface);
    return true;
}

bool zbegl_manual_eglCreatePlatformWindowSurface(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    EGLConfig config = as_pointer(call.handle_of(1, EglObject::Config));
    if (!call.valid()) return true;
    const std::uint32_t window_handle = call.arg(2);
    if (!call.valid()) return true;
    void* window = as_pointer(host.window_for(window_handle));
    if (window == nullptr) {
        call.fail(kEglBadNativeWindow, "unknown native window handle");
        return true;
    }
    std::vector<EGLAttrib> attribs;
    if (!read_attribs(host, call, 3, attribs)) return true;
    call.set_handle(host.backend().eglCreatePlatformWindowSurface(
                        dpy, config, window, attribs.empty() ? nullptr : attribs.data()),
                    EglObject::Surface);
    return true;
}

bool zbegl_manual_eglCreatePbufferSurface(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    EGLConfig config = as_pointer(call.handle_of(1, EglObject::Config));
    if (!call.valid()) return true;
    std::vector<EGLint> attribs;
    if (!read_attribs(host, call, 2, attribs)) return true;
    call.set_handle(host.backend().eglCreatePbufferSurface(dpy, config,
                                                           attribs.empty() ? nullptr
                                                                           : attribs.data()),
                    EglObject::Surface);
    return true;
}

bool zbegl_manual_eglGetPlatformDisplay(HostEgl& host, HostEgl::Call& call) {
    const EGLenum platform = call.scalar<EGLenum>(0);
    const std::uint32_t native_display = call.arg(1);
    if (!call.valid()) return true;
    if (native_display != 0) {
        // EGL_DEFAULT_DISPLAY is the only native display a guest can name: anything else would
        // be a host pointer truncated to 32 bits.
        return unsupported(call, "only EGL_DEFAULT_DISPLAY is supported");
    }
    std::vector<EGLAttrib> attribs;
    if (!read_attribs(host, call, 2, attribs)) return true;
    call.set_handle(host.backend().eglGetPlatformDisplay(platform, nullptr,
                                                         attribs.empty() ? nullptr
                                                                         : attribs.data()),
                    EglObject::Display);
    return true;
}

bool zbegl_manual_eglGetCurrentDisplay(HostEgl& host, HostEgl::Call& call) {
    call.set_handle(host.backend().eglGetCurrentDisplay(), EglObject::Display);
    return true;
}

bool zbegl_manual_eglGetCurrentContext(HostEgl& host, HostEgl::Call& call) {
    call.set_handle(host.backend().eglGetCurrentContext(), EglObject::Context);
    return true;
}

bool zbegl_manual_eglGetCurrentSurface(HostEgl& host, HostEgl::Call& call) {
    const EGLint readdraw = call.scalar<EGLint>(0);
    if (!call.valid()) return true;
    call.set_handle(host.backend().eglGetCurrentSurface(readdraw), EglObject::Surface);
    return true;
}

bool zbegl_manual_eglSwapBuffers(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    EGLSurface surface = as_pointer(call.handle_of(1, EglObject::Surface));
    if (!call.valid()) return true;
    runtime_report().note_egl_swap();
    // The thread that presents frames is the one whose activity says whether the guest is still
    // drawing at all; the hang report names it explicitly.
    if (const GuestThread* thread = Process::current_thread()) {
        record_render_thread(thread->diagnostic_tid());
    }
    if (gl_diagnostics_enabled()) gl_diagnose_swap();
    const std::uint64_t before_ns = frame_probe_now_ns();
    const EGLBoolean swapped = host.backend().eglSwapBuffers(dpy, surface);
    const std::uint64_t after_ns = frame_probe_now_ns();
    frame_probe_swap(before_ns, after_ns);
    call.set_result(swapped);
    return true;
}

bool zbegl_manual_eglCreateImage(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "EGLImage objects cannot cross to a 32-bit guest");
}

bool zbegl_manual_eglCreateSync(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "EGLSync objects cannot cross to a 32-bit guest");
}

bool zbegl_manual_eglCreatePbufferFromClientBuffer(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "EGLClientBuffer objects cannot cross to a 32-bit guest");
}

bool zbegl_manual_eglCreatePixmapSurface(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "native pixmaps are not supported");
}

bool zbegl_manual_eglCreatePlatformPixmapSurface(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "native pixmaps are not supported");
}

bool zbegl_manual_eglDestroyContext(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    EGLContext ctx = as_pointer(call.handle_of(1, EglObject::Context));
    if (!call.valid()) return true;
    const EGLBoolean ok = host.backend().eglDestroyContext(dpy, ctx);
    if (ok) host.forget_handle(call.arg(1));
    call.set_result(ok);
    return true;
}

bool zbegl_manual_eglDestroySurface(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    EGLSurface surface = as_pointer(call.handle_of(1, EglObject::Surface));
    if (!call.valid()) return true;
    const EGLBoolean ok = host.backend().eglDestroySurface(dpy, surface);
    if (ok) host.forget_handle(call.arg(1));
    call.set_result(ok);
    return true;
}

bool zbegl_manual_eglTerminate(HostEgl& host, HostEgl::Call& call) {
    EGLDisplay dpy = as_pointer(call.handle_of(0, EglObject::Display));
    if (!call.valid()) return true;
    const EGLBoolean ok = host.backend().eglTerminate(dpy);
    if (ok) host.forget_handle(call.arg(0));
    call.set_result(ok);
    return true;
}

bool zbegl_manual_eglDestroyImage(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "EGLImage objects cannot cross to a 32-bit guest");
}

bool zbegl_manual_eglDestroySync(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "EGLSync objects cannot cross to a 32-bit guest");
}

bool zbegl_manual_eglClientWaitSync(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "EGLSync objects cannot cross to a 32-bit guest");
}

bool zbegl_manual_eglWaitSync(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "EGLSync objects cannot cross to a 32-bit guest");
}

bool zbegl_manual_eglGetSyncAttrib(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "EGLSync objects cannot cross to a 32-bit guest");
}

bool zbegl_manual_eglCopyBuffers(HostEgl&, HostEgl::Call& call) {
    return unsupported(call, "native pixmaps are not supported");
}

}  // namespace zb
