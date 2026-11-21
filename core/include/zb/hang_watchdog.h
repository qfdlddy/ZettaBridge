#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace zb {

class Process;

// Hang diagnostics: a guest that stops making progress (gray screen, no crash) leaves nothing
// in the crash report, because nothing crashed. This records, per guest thread, the last thing
// it did on its way out of translated code, and a background thread periodically checks whether
// that has changed.

// The two places a guest thread leaves translated code.
enum class ThreadActivityKind : std::uint8_t {
    kNone = 0,
    kSyscall = 1,
    kHostCall = 2,
};

// Records that guest thread `tid` just performed a syscall (`id` = syscall number) or a host
// call (`id` = host-call index). Lock-free, allocation-free, safe from any thread: called on
// every syscall and every host call, so it must stay cheap.
#if defined(ZB_NO_TELEMETRY)
inline void record_thread_activity(std::int32_t, ThreadActivityKind, std::uint32_t) {}
inline void record_thread_activity_done(std::int32_t) {}
inline void release_thread_activity(std::int32_t) {}
inline void record_render_thread(std::int32_t) {}
#else
// Tracks the thread that presents frames. It is the one thread whose activity says whether the
// guest is still drawing, and it is reported even when idle workers fill the general list.
void record_render_thread(std::int32_t host_tid);

void record_thread_activity(std::int32_t tid, ThreadActivityKind kind, std::uint32_t id);

// The host call recorded last for this thread has returned. A thread whose last recorded event is
// still in progress is blocked inside our own code, which reads very differently from a guest
// that simply stopped calling out.
void record_thread_activity_done(std::int32_t tid);

// Reclaims the thread's activity slot on exit so long-running sessions do not exhaust g_slots.
void release_thread_activity(std::int32_t tid);
#endif

// Lock-free guest VM / host JIT churn counters sampled by the stall detector.
struct GuestVmCounters {
    std::uint64_t mmap_calls = 0;
    std::uint64_t mmap_bytes = 0;
    std::uint64_t munmap_calls = 0;
    std::uint64_t munmap_bytes = 0;
    std::uint64_t mremap_calls = 0;
    std::uint64_t madvise_dontneed_calls = 0;
    std::uint64_t madvise_dontneed_bytes = 0;
    std::uint64_t brk_calls = 0;
    std::int64_t brk_net_bytes = 0;
    std::uint64_t clone_calls = 0;
    std::uint64_t borrower_creates = 0;
    std::uint64_t jit_invalidations = 0;
};

#if defined(ZB_NO_TELEMETRY)
inline void record_guest_mmap(std::uint64_t) {}
inline void record_guest_munmap(std::uint64_t) {}
inline void record_guest_mremap() {}
inline void record_guest_madvise_dontneed(std::uint64_t) {}
inline void record_guest_brk(std::int64_t) {}
inline void record_guest_clone() {}
inline void record_guest_borrower() {}
inline void record_jit_invalidate() {}
#else
void record_guest_mmap(std::uint64_t bytes);
void record_guest_munmap(std::uint64_t bytes);
void record_guest_mremap();
void record_guest_madvise_dontneed(std::uint64_t bytes);
void record_guest_brk(std::int64_t delta_bytes);
void record_guest_clone();
void record_guest_borrower();
void record_jit_invalidate();
#endif
GuestVmCounters snapshot_guest_vm_counters();

struct ThreadActivitySample {
    std::int32_t tid = 0;
    ThreadActivityKind kind = ThreadActivityKind::kNone;
    std::uint32_t id = 0;
    std::uint64_t counter = 0;
    bool in_progress = false;  // still inside the recorded host call or syscall
    // Filled only by a snapshot that has the runtime's thread registry: the guest PC at the
    // sample and the short form of the image it falls in ("libunity.so+0x1a2b3"). A thread that
    // blocks inside a syscall would otherwise only ever be described as that syscall.
    std::uint32_t pc = 0;
    std::string where;
};

// A snapshot of every guest thread slot touched so far.
std::vector<ThreadActivitySample> snapshot_thread_activity();

// snapshot_thread_activity() plus the live guest PC of every thread that is still a guest thread.
std::vector<ThreadActivitySample> snapshot_thread_activity_with_pc(Process& process);

// "sys:openat" or "host:libGLESv2.so glClear"; "(none)" for a slot that was never recorded.
std::string describe_thread_activity(const ThreadActivitySample& sample);

// What the kernel says about one thread. A guest that stopped calling out is either blocked (a
// stable wait channel, no CPU burned) or spinning in translated code (running, CPU climbing);
// only the kernel can tell those apart, because both look like "no progress" from inside.
struct ThreadKernelState {
    bool present = false;    // the tid still exists in this process
    char state = '?';        // R running, S sleeping, D uninterruptible, ...
    std::uint64_t cpu = 0;   // utime + stime in clock ticks
    std::string waiting;     // kernel wait channel, empty when unreadable
};

ThreadKernelState read_thread_kernel_state(std::int32_t tid);

// "R cpu=469 in=0", or "gone" when the thread no longer exists.
std::string format_thread_kernel_state(const ThreadKernelState& state);

std::string thread_kernel_state(std::int32_t tid);

// The kernel's name for one thread of this process, from /proc/self/task/<tid>/comm, or "" when
// the thread no longer exists. A tid in the report is unidentifiable without it: the thread may
// have been created after the one-time census was written.
std::string thread_name(std::int32_t tid);

// Periodically samples snapshot_thread_activity() and records a report note when one or more
// threads have made no progress since the previous two samples. Never aborts or kills the
// process; this is diagnostics only.
class HangWatchdog {
public:
    using Clock = std::chrono::steady_clock;
    using SnapshotFn = std::function<std::vector<ThreadActivitySample>()>;
    // A one-line summary of guest threads that hold undelivered signals, or "" when the runtime
    // cannot answer. A guest that never gets a signal it is waiting for looks exactly like a
    // guest that stopped drawing, so the report needs to say which threads are holding one.
    using DetailFn = std::function<std::string()>;

    static constexpr std::chrono::seconds kInterval{3};
    static constexpr int kStuckThreshold = 2;      // consecutive unchanged samples
    static constexpr std::size_t kMaxNotes = 6;    // report lines this watchdog will ever write
    // Engine threads plus the running threads that matter; a report line is bounded, so this is
    // not the whole process. Ordering is what keeps the interesting entry inside it.
    static constexpr std::size_t kMaxThreadsShown = 12;

    explicit HangWatchdog(SnapshotFn snapshot = snapshot_thread_activity,
                          DetailFn pending_signals = {});

    // One sampling pass at `now`. Returns true if it wrote a new watch-<n> note from the bounded
    // history; the watch-current line is kept up to date on every pass regardless of that budget.
    bool sample(Clock::time_point now);

    // Runs sample() every kInterval, forever, on the calling thread.
    [[noreturn]] void run_forever();

private:
    struct State {
        std::int32_t tid = 0;
        ThreadActivityKind kind = ThreadActivityKind::kNone;
        std::uint32_t id = 0;
        std::uint64_t counter = 0;
        bool in_progress = false;  // the recorded host call or syscall has not returned yet
        std::uint32_t pc = 0;
        std::string where;
        ThreadKernelState kernel;    // at the last sample
        std::uint64_t cpu_delta = 0;  // ticks burned between the two last samples
        std::string name;             // the kernel's name for the tid
        Clock::time_point since{};  // when this (kind, id, counter) was first observed
        int unchanged_samples = 0;
    };

    SnapshotFn snapshot_;
    DetailFn pending_signals_;
    std::vector<State> previous_;
    // The tids of the previous note, in the order they were shown. A note is written only when
    // this set changes: idle waiters are stuck from the first sample, and counting them again every
    // sample spends the whole budget before a real freeze starts.
    std::string last_note_tids_;
    std::size_t notes_written_ = 0;
    // The always-current stuck line, which is not part of the note budget. Its identity excludes
    // the elapsed seconds so only a real change rewrites the report, and it is refreshed anyway
    // every kCurrentRefresh so those seconds stay near the truth.
    static constexpr std::chrono::seconds kCurrentRefresh{30};
    std::string last_current_identity_;
    Clock::time_point current_written_at_{};
    bool current_written_ = false;
};

// Starts the watchdog on a detached background thread, once per process. GuestJniEngine calls
// this when the runtime starts; the runtime is process-lifetime, so there is no shutdown path.
// With a process, each sample also records every guest thread's PC and a short stack chain
// ("stack-<tid>" notes), which is what names the caller of a thread blocked inside a syscall.
#if defined(ZB_NO_TELEMETRY)
inline void start_hang_watchdog(Process* = nullptr) {}
#else
void start_hang_watchdog(Process* process = nullptr);
#endif

}  // namespace zb
