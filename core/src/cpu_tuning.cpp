#include "cpu_tuning.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
#include <dlfcn.h>
#endif

namespace zb {

namespace {

#ifdef __linux__
constexpr std::size_t kMaxTunedThreads = 32;
struct TunedSlot {
    std::atomic<std::int32_t> tid{0};
    std::atomic<std::uint8_t> role{0};
};
std::array<TunedSlot, kMaxTunedThreads> g_slots{};
std::atomic<std::uint32_t> g_threads_version{0};

std::int32_t resolve_tid(std::int32_t tid) {
    if (tid != 0) return tid;
    thread_local const std::int32_t t_tid = static_cast<std::int32_t>(::syscall(SYS_gettid));
    return t_tid;
}
#endif

#ifdef __ANDROID__
struct APerformanceHintManager;
struct APerformanceHintSession;

using FnGetManager = APerformanceHintManager* (*)();
using FnCreateSession = APerformanceHintSession* (*)(APerformanceHintManager*, const std::int32_t*,
                                                     std::size_t, std::int64_t);
using FnReportActualWorkDuration = int (*)(APerformanceHintSession*, std::int64_t);
using FnSetThreads = int (*)(APerformanceHintSession*, const std::int32_t*, std::size_t);
using FnCloseSession = void (*)(APerformanceHintSession*);

struct AdpfState {
    std::atomic<bool> initialized{false};
    std::atomic<bool> available{false};
    std::mutex mutex;
    FnGetManager get_manager = nullptr;
    FnCreateSession create_session = nullptr;
    FnReportActualWorkDuration report_duration = nullptr;
    FnSetThreads set_threads = nullptr;
    FnCloseSession close_session = nullptr;
    APerformanceHintManager* manager = nullptr;
    APerformanceHintSession* session = nullptr;
    std::uint32_t session_version = 0;
    std::uint64_t prev_swap_ns = 0;
    std::uint64_t last_refresh_ns = 0;
};

AdpfState& adpf() {
    static AdpfState state;
    return state;
}

constexpr std::int64_t kTargetFrameNs = 16666667;         // 60 fps target
constexpr std::uint64_t kMinReportNs = 1000000;           // 1 ms floor
constexpr std::uint64_t kMaxReportNs = 100000000;         // 100 ms clamp
constexpr std::uint64_t kIdleGapNs = 250000000;           // ignore >250 ms pauses
constexpr std::uint64_t kSessionRefreshCooldownNs = 1000000000ULL;  // 1 s

bool init_adpf_locked(AdpfState& st) {
    if (st.initialized.load(std::memory_order_acquire)) {
        return st.available.load(std::memory_order_relaxed);
    }
    void* lib = ::dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
    if (lib == nullptr) lib = ::dlopen("libandroid.so", RTLD_LAZY);
    if (lib != nullptr) {
        st.get_manager = reinterpret_cast<FnGetManager>(::dlsym(lib, "APerformanceHint_getManager"));
        st.create_session = reinterpret_cast<FnCreateSession>(::dlsym(lib, "APerformanceHint_createSession"));
        st.report_duration = reinterpret_cast<FnReportActualWorkDuration>(
            ::dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
        st.set_threads = reinterpret_cast<FnSetThreads>(::dlsym(lib, "APerformanceHint_setThreads"));
        st.close_session = reinterpret_cast<FnCloseSession>(::dlsym(lib, "APerformanceHint_closeSession"));
        if (st.get_manager != nullptr && st.create_session != nullptr && st.report_duration != nullptr) {
            st.manager = st.get_manager();
        }
    }
    const bool ok = st.manager != nullptr;
    st.available.store(ok, std::memory_order_relaxed);
    st.initialized.store(true, std::memory_order_release);
    return ok;
}

std::size_t collect_tids(std::array<std::int32_t, kMaxTunedThreads>& out) {
    std::size_t n = 0;
    for (const TunedSlot& slot : g_slots) {
        const std::int32_t tid = slot.tid.load(std::memory_order_relaxed);
        if (tid > 0 && n < out.size()) out[n++] = tid;
    }
    return n;
}
#endif

}  // namespace

void register_tuned_thread(std::int32_t tid, CpuRole role) {
#ifdef __linux__
    // Track main/render/audio thread TIDs for ADPF hint sessions and NR_setpriority nice
    // protection without overriding kernel EAS/WALT core placement via sched_setaffinity.
    const std::int32_t target = resolve_tid(tid);
    if (target <= 0) return;

    for (TunedSlot& slot : g_slots) {
        if (slot.tid.load(std::memory_order_relaxed) == target) {
            slot.role.store(static_cast<std::uint8_t>(role), std::memory_order_relaxed);
            return;
        }
    }
    for (TunedSlot& slot : g_slots) {
        std::int32_t expected = 0;
        if (slot.tid.compare_exchange_strong(expected, target, std::memory_order_relaxed)) {
            slot.role.store(static_cast<std::uint8_t>(role), std::memory_order_relaxed);
            g_threads_version.fetch_add(1, std::memory_order_release);
            return;
        }
    }
#else
    (void)tid;
    (void)role;
#endif
}

void ensure_audio_thread_tuned() {
#ifdef __linux__
    thread_local bool t_registered = false;
    if (__builtin_expect(!t_registered, 0)) {
        t_registered = true;
        register_tuned_thread(0, CpuRole::AudioThread);
    }
#endif
}

void note_frame_swap_for_tuning(std::uint64_t now_ns) {
#ifdef __ANDROID__
    thread_local bool t_render_registered = false;
    if (__builtin_expect(!t_render_registered, 0)) {
        t_render_registered = true;
        register_tuned_thread(0, CpuRole::MainThread);
    }

    AdpfState& st = adpf();
    if (st.initialized.load(std::memory_order_acquire) &&
        !st.available.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(st.mutex);
    if (!init_adpf_locked(st)) return;

    const std::uint32_t ver = g_threads_version.load(std::memory_order_acquire);
    if (st.session == nullptr) {
        std::array<std::int32_t, kMaxTunedThreads> tids{};
        const std::size_t n = collect_tids(tids);
        if (n > 0) {
            st.session = st.create_session(st.manager, tids.data(), n, kTargetFrameNs);
            st.session_version = ver;
            st.last_refresh_ns = now_ns;
        }
    } else if (ver != st.session_version &&
               now_ns >= st.last_refresh_ns + kSessionRefreshCooldownNs) {
        std::array<std::int32_t, kMaxTunedThreads> tids{};
        const std::size_t n = collect_tids(tids);
        if (n > 0) {
            if (st.set_threads != nullptr) {
                st.set_threads(st.session, tids.data(), n);
            } else if (st.close_session != nullptr) {
                st.close_session(st.session);
                st.session = st.create_session(st.manager, tids.data(), n, kTargetFrameNs);
            }
            st.session_version = ver;
            st.last_refresh_ns = now_ns;
        }
    }

    if (st.session != nullptr && st.prev_swap_ns != 0 && now_ns > st.prev_swap_ns) {
        const std::uint64_t delta = now_ns - st.prev_swap_ns;
        if (delta < kIdleGapNs) {
            std::uint64_t clamped = delta;
            if (clamped < kMinReportNs) clamped = kMinReportNs;
            if (clamped > kMaxReportNs) clamped = kMaxReportNs;
            st.report_duration(st.session, static_cast<std::int64_t>(clamped));
        }
    }
    st.prev_swap_ns = now_ns;
#else
    (void)now_ns;
#endif
}

bool is_audio_thread_tid(std::int32_t tid) {
#ifdef __linux__
    const std::int32_t target = resolve_tid(tid);
    if (target <= 0) return false;
    for (const TunedSlot& slot : g_slots) {
        if (slot.tid.load(std::memory_order_relaxed) == target) {
            return static_cast<CpuRole>(slot.role.load(std::memory_order_relaxed)) ==
                   CpuRole::AudioThread;
        }
    }
#else
    (void)tid;
#endif
    return false;
}

}  // namespace zb
