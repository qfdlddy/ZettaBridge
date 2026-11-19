// Hang watchdog: per-thread last-activity slots (lock-free, allocation-free) plus a background
// sampler that turns "same activity, same counter, two samples running" into a report note. No
// guest semantics change here; a stuck thread is only ever described, never touched.
#include "zb/hang_watchdog.h"

#include <dirent.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <thread>
#include <unordered_map>
#include <utility>

#include "zb/host_jni.h"
#include "zb/log.h"
#include "zb/process.h"
#include "zb/runtime_report.h"
#include "zb/syscalls.h"
#include "gl/frame_probe.h"

namespace zb {

namespace {

// Threads whose stack is dumped each sample. Enough for the engine's named threads without
// letting a guest with a thread pool crowd the report.
constexpr unsigned kMaxStacks = 14;

// Last distinct PCs per thread, so "same place every sample" is visible in one line.
std::map<std::int32_t, std::deque<std::string>> g_recent_pcs;

// Fixed-size, never-grows table of per-thread activity slots. A guest thread claims a slot the
// first time it is seen and keeps it (tids are not reused across a run in a way that matters for
// diagnostics); a slot is identified by `tid` with 0 meaning free.
constexpr std::size_t kMaxSlots = 256;

struct Slot {
    std::atomic<bool> in_progress{false};
    std::atomic<std::int32_t> tid{0};
    std::atomic<std::uint8_t> kind{static_cast<std::uint8_t>(ThreadActivityKind::kNone)};
    std::atomic<std::uint32_t> id{0};
    std::atomic<std::uint64_t> counter{0};
};

Slot g_slots[kMaxSlots];

struct AtomicGuestVmCounters {
    std::atomic<std::uint64_t> mmap_calls{0};
    std::atomic<std::uint64_t> mmap_bytes{0};
    std::atomic<std::uint64_t> munmap_calls{0};
    std::atomic<std::uint64_t> munmap_bytes{0};
    std::atomic<std::uint64_t> mremap_calls{0};
    std::atomic<std::uint64_t> madvise_dontneed_calls{0};
    std::atomic<std::uint64_t> madvise_dontneed_bytes{0};
    std::atomic<std::uint64_t> brk_calls{0};
    std::atomic<std::int64_t> brk_delta_bytes{0};
    std::atomic<std::uint64_t> clone_calls{0};
    std::atomic<std::uint64_t> borrower_calls{0};
    std::atomic<std::uint64_t> jit_invalidations{0};
};

AtomicGuestVmCounters g_vm_counters;

}  // namespace

#if !defined(ZB_NO_TELEMETRY)
void record_guest_mmap(std::uint64_t bytes) {
    g_vm_counters.mmap_calls.fetch_add(1, std::memory_order_relaxed);
    g_vm_counters.mmap_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void record_guest_munmap(std::uint64_t bytes) {
    g_vm_counters.munmap_calls.fetch_add(1, std::memory_order_relaxed);
    g_vm_counters.munmap_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void record_guest_mremap() {
    g_vm_counters.mremap_calls.fetch_add(1, std::memory_order_relaxed);
}

void record_guest_madvise_dontneed(std::uint64_t bytes) {
    g_vm_counters.madvise_dontneed_calls.fetch_add(1, std::memory_order_relaxed);
    g_vm_counters.madvise_dontneed_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void record_guest_brk(std::int64_t delta_bytes) {
    g_vm_counters.brk_calls.fetch_add(1, std::memory_order_relaxed);
    g_vm_counters.brk_delta_bytes.fetch_add(delta_bytes, std::memory_order_relaxed);
}

void record_guest_clone() {
    g_vm_counters.clone_calls.fetch_add(1, std::memory_order_relaxed);
}

void record_guest_borrower() {
    g_vm_counters.borrower_calls.fetch_add(1, std::memory_order_relaxed);
}

void record_jit_invalidate() {
    g_vm_counters.jit_invalidations.fetch_add(1, std::memory_order_relaxed);
}
#endif

GuestVmCounters snapshot_guest_vm_counters() {
    GuestVmCounters out;
    out.mmap_calls = g_vm_counters.mmap_calls.load(std::memory_order_relaxed);
    out.mmap_bytes = g_vm_counters.mmap_bytes.load(std::memory_order_relaxed);
    out.munmap_calls = g_vm_counters.munmap_calls.load(std::memory_order_relaxed);
    out.munmap_bytes = g_vm_counters.munmap_bytes.load(std::memory_order_relaxed);
    out.mremap_calls = g_vm_counters.mremap_calls.load(std::memory_order_relaxed);
    out.madvise_dontneed_calls = g_vm_counters.madvise_dontneed_calls.load(std::memory_order_relaxed);
    out.madvise_dontneed_bytes = g_vm_counters.madvise_dontneed_bytes.load(std::memory_order_relaxed);
    out.brk_calls = g_vm_counters.brk_calls.load(std::memory_order_relaxed);
    out.brk_net_bytes = g_vm_counters.brk_delta_bytes.load(std::memory_order_relaxed);
    out.clone_calls = g_vm_counters.clone_calls.load(std::memory_order_relaxed);
    out.borrower_creates = g_vm_counters.borrower_calls.load(std::memory_order_relaxed);
    out.jit_invalidations = g_vm_counters.jit_invalidations.load(std::memory_order_relaxed);
    return out;
}

namespace {

thread_local Slot* t_activity_slot = nullptr;
thread_local std::int32_t t_activity_tid = 0;

Slot* find_or_claim_slot(std::int32_t tid) {
    if (t_activity_slot != nullptr && t_activity_tid == tid) return t_activity_slot;
    Slot* free_slot = nullptr;
    for (Slot& slot : g_slots) {
        const std::int32_t current = slot.tid.load(std::memory_order_relaxed);
        if (current == tid) {
            t_activity_slot = &slot;
            t_activity_tid = tid;
            return &slot;
        }
        if (current == 0 && free_slot == nullptr) free_slot = &slot;
    }
    if (free_slot == nullptr) return nullptr;  // table full; diagnostics only, never blocks the guest
    std::int32_t expected = 0;
    if (!free_slot->tid.compare_exchange_strong(expected, tid, std::memory_order_relaxed)) {
        return nullptr;
    }
    free_slot->counter.store(0, std::memory_order_relaxed);
    t_activity_slot = free_slot;
    t_activity_tid = tid;
    return free_slot;
}

}  // namespace

#if !defined(ZB_NO_TELEMETRY)
void record_thread_activity(std::int32_t tid, ThreadActivityKind kind, std::uint32_t id) {
    if (tid == 0) return;
    Slot* slot = find_or_claim_slot(tid);
    if (slot == nullptr) return;
    slot->kind.store(static_cast<std::uint8_t>(kind), std::memory_order_relaxed);
    slot->id.store(id, std::memory_order_relaxed);
    // Single producer per slot tid, so a plain load+store avoids an atomic RMW on every syscall/host call.
    const std::uint64_t next = slot->counter.load(std::memory_order_relaxed) + 1;
    slot->counter.store(next, std::memory_order_relaxed);
    slot->in_progress.store(true, std::memory_order_relaxed);
}

void record_thread_activity_done(std::int32_t tid) {
    if (tid == 0) return;
    if (t_activity_slot != nullptr && t_activity_tid == tid) {
        t_activity_slot->in_progress.store(false, std::memory_order_relaxed);
        return;
    }
    for (Slot& slot : g_slots) {
        if (slot.tid.load(std::memory_order_relaxed) == tid) {
            slot.in_progress.store(false, std::memory_order_relaxed);
            return;
        }
    }
}

void release_thread_activity(std::int32_t tid) {
    if (tid == 0) return;
    if (t_activity_slot != nullptr && t_activity_tid == tid) {
        t_activity_slot->in_progress.store(false, std::memory_order_relaxed);
        t_activity_slot->tid.store(0, std::memory_order_release);
        t_activity_slot = nullptr;
        t_activity_tid = 0;
        return;
    }
    for (Slot& slot : g_slots) {
        std::int32_t expected = tid;
        if (slot.tid.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
            slot.in_progress.store(false, std::memory_order_relaxed);
            return;
        }
    }
}
#endif

std::vector<ThreadActivitySample> snapshot_thread_activity() {
    std::vector<ThreadActivitySample> out;
    for (Slot& slot : g_slots) {
        const std::int32_t tid = slot.tid.load(std::memory_order_relaxed);
        if (tid == 0) continue;
        ThreadActivitySample sample;
        sample.tid = tid;
        sample.kind = static_cast<ThreadActivityKind>(slot.kind.load(std::memory_order_relaxed));
        sample.id = slot.id.load(std::memory_order_relaxed);
        sample.counter = slot.counter.load(std::memory_order_relaxed);
        sample.in_progress = slot.in_progress.load(std::memory_order_relaxed);
        out.push_back(sample);
    }
    return out;
}

std::vector<ThreadActivitySample> snapshot_thread_activity_with_pc(Process& process) {
    std::vector<ThreadActivitySample> out = snapshot_thread_activity();
    for (ThreadActivitySample& sample : out) {
        Process::ThreadStateSample state;
        if (!process.sample_thread_state(sample.tid, state)) continue;
        sample.pc = state.pc;
        if (state.pc != 0) {
            sample.where = process.describe_code_address(state.pc);
            if (sample.where.empty()) sample.where = process.describe_address(state.pc);
            // The full-path form is long and would push the rest of the line out of the report;
            // the compact image label is what identifies the code.
            const std::size_t slash = sample.where.find_last_of('/');
            if (slash != std::string::npos) sample.where = sample.where.substr(slash + 1);
        }
    }
    return out;
}

std::string thread_name(std::int32_t tid) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
    std::FILE* comm = std::fopen(path, "re");
    if (comm == nullptr) return std::string();
    char name[64] = {};
    const char* read = std::fgets(name, sizeof name, comm);
    std::fclose(comm);
    if (read == nullptr) return std::string();
    if (char* newline = std::strchr(name, '\n')) *newline = '\0';
    return std::string(name);
}

// Ticks of CPU this thread has burned, from /proc/self/task/<tid>/stat, or 0 when unreadable.
// A thread whose activity never changes but whose CPU keeps climbing is spinning in translated
// code; one whose CPU stands still is blocked.
// What the kernel says about a thread: "gone" when it no longer exists, otherwise its scheduler
// state (R running, S sleeping, D uninterruptible), the CPU ticks it burned, and the kernel
// function it is waiting in. Zero CPU alone cannot tell an idle thread from a dead one.
ThreadKernelState read_thread_kernel_state_impl(std::int32_t tid, bool read_wchan) {
    ThreadKernelState out;
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
    std::FILE* file = std::fopen(path, "re");
    if (file == nullptr) return out;
    char line[1024];
    const char* read = std::fgets(line, sizeof line, file);
    std::fclose(file);
    if (read == nullptr) return out;
    const char* cursor = std::strrchr(line, ')');
    if (cursor == nullptr || cursor[1] == '\0') return out;
    unsigned long long utime = 0, stime = 0;
    // Fields after "comm": state, ppid, pgrp, session, tty, tpgid, flags, min_flt, cmin_flt,
    // maj_flt, cmaj_flt, utime, stime.
    if (std::sscanf(cursor + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &out.state,
                    &utime, &stime) != 3) {
        return out;
    }
    out.present = true;
    out.cpu = utime + stime;
    if (read_wchan) {
        char waiting[64] = {};
        std::snprintf(path, sizeof path, "/proc/self/task/%d/wchan", tid);
        if (std::FILE* wchan = std::fopen(path, "re")) {
            if (std::fgets(waiting, sizeof waiting, wchan) == nullptr) waiting[0] = '\0';
            std::fclose(wchan);
        }
        out.waiting = waiting;
    }
    return out;
}

ThreadKernelState read_thread_kernel_state(std::int32_t tid) {
    return read_thread_kernel_state_impl(tid, true);
}

std::string format_thread_kernel_state(const ThreadKernelState& state) {
    if (!state.present) return "gone";
    char text[128];
    std::snprintf(text, sizeof text, "%c cpu=%llu%s%s", state.state,
                  static_cast<unsigned long long>(state.cpu),
                  state.waiting.empty() ? "" : " in=", state.waiting.c_str());
    return text;
}

std::string thread_kernel_state(std::int32_t tid) {
    return format_thread_kernel_state(read_thread_kernel_state(tid));
}

// Engine threads carry telling names, and a name is worth more than slot-claim order when the note
// has to choose which stuck threads to list. Lower is more interesting.
int engine_thread_priority(const std::string& name) {
    if (std::strncmp(name.c_str(), "UnityMain", 9) == 0 ||
        std::strncmp(name.c_str(), "Loading.", 8) == 0) {
        return 0;
    }
    if (std::strncmp(name.c_str(), "UnityGfx", 8) == 0 ||
        std::strncmp(name.c_str(), "UnityChoreo", 11) == 0 ||
        std::strncmp(name.c_str(), "Job.Worker", 10) == 0 || std::strncmp(name.c_str(), "GC ", 3) == 0) {
        return 1;
    }
    return 2;
}

// Every thread of this process with its name, scheduler state and wait channel, whether or not it
// ever ran guest code. Engine threads carry telling names (Flutter uses "1.ui", "1.raster",
// "1.io"), so this says which part of the guest is missing or waiting, and shows ART's own threads
// next to ours.
// Threads of the runtime itself (binder, ART daemons) crowd out the engine's own, and one report
// line holds only so much, so they are counted rather than listed and the rest goes out in chunks.
bool is_platform_thread(const char* name) {
    static const char* const kNoise[] = {"binder:", "Jit thread pool", "Profile Saver", "ADB-JDWP",
                                         "Signal Catcher", "perfetto", "HeapTaskDaemon",
                                         "ReferenceQueueD", "FinalizerDaemon", "FinalizerWatchd",
                                         "hwuiTask", "RenderThread", "queued-work-loo", "Timer-",
                                         "GPU completion", "SurfaceSyncGrou"};
    for (const char* prefix : kNoise) {
        if (std::strncmp(name, prefix, std::strlen(prefix)) == 0) return true;
    }
    return false;
}

void write_threads_census() {
    DIR* dir = ::opendir("/proc/self/task");
    if (dir == nullptr) {
        runtime_report().note_watch_detail("threads", "(unavailable)");
        return;
    }
    std::string chunk;
    unsigned chunk_index = 0;
    unsigned platform = 0;
    unsigned listed = 0;
    while (const dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const std::int32_t tid = std::atoi(entry->d_name);
        if (tid == 0) continue;
        const std::string name = thread_name(tid);
        if (is_platform_thread(name.c_str())) {
            ++platform;
            continue;
        }
        if (listed >= 48) break;
        ++listed;
        chunk += (chunk.empty() ? "" : " | ") + std::to_string(tid) + ":" + name + " " +
                 thread_kernel_state(tid);
        if (chunk.size() > 420) {
            runtime_report().note_watch_detail("threads-" + std::to_string(++chunk_index), chunk);
            chunk.clear();
        }
    }
    ::closedir(dir);
    if (!chunk.empty()) runtime_report().note_watch_detail("threads-" + std::to_string(++chunk_index), chunk);
    runtime_report().note_watch_detail("threads-platform", std::to_string(platform) + " runtime threads hidden");
}

std::string describe_thread_activity(const ThreadActivitySample& sample) {
    char text[96];
    switch (sample.kind) {
    case ThreadActivityKind::kSyscall:
        std::snprintf(text, sizeof text, "sys:%s%s", syscall_name(sample.id),
                      sample.in_progress ? "(inside)" : "");
        return text;
    case ThreadActivityKind::kHostCall: {
        // Name the call: the report is read on a device, without the generated index table.
        const auto [library, name] = host_call_name(sample.id);
        if (std::strcmp(library, "?") == 0) {
            std::snprintf(text, sizeof text, "host:0x%x%s", sample.id,
                          sample.in_progress ? "(inside)" : "");
        } else {
            std::snprintf(text, sizeof text, "host:%s %s%s", library, name,
                          sample.in_progress ? "(inside)" : "");
        }
        return text;
    }
    case ThreadActivityKind::kNone:
    default:
        return "(none)";
    }
}

// One "stack-<tid>: <name> pc=.. lr=.. sp=.. fp=.. r0.. chain=.." note per guest thread. A thread
// blocked in futex is described by every other source as "sys:futex"; its registers and the return
// addresses saved on its stack are the only thing that says which library called into the wait.
// The recent PCs also separate a thread spinning inside one function from one running its loop.
void write_thread_stacks(Process& process) {
    DIR* dir = ::opendir("/proc/self/task");
    if (dir == nullptr) return;
    struct Candidate {
        int priority;
        std::int32_t tid;
        std::string name;
    };
    std::vector<Candidate> candidates;
    while (const dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const std::int32_t tid = std::atoi(entry->d_name);
        if (tid == 0) continue;
        char path[64];
        std::snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
        char name[32] = {};
        if (std::FILE* comm = std::fopen(path, "re")) {
            if (std::fgets(name, sizeof name, comm) != nullptr) {
                char* newline = std::strchr(name, '\n');
                if (newline != nullptr) *newline = '\0';
            }
            std::fclose(comm);
        }
        if (is_platform_thread(name)) continue;
        candidates.push_back({engine_thread_priority(name), tid, name});
    }
    ::closedir(dir);
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.priority < b.priority; });
    unsigned written = 0;
    for (const Candidate& candidate : candidates) {
        if (written >= kMaxStacks) break;
        Process::ThreadStateSample state;
        if (!process.sample_thread_state(candidate.tid, state)) continue;
        char text[192];
        std::snprintf(text, sizeof text, "%s pc=0x%08x lr=0x%08x sp=0x%08x fp=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x",
                      candidate.name.c_str(), state.pc, state.lr, state.sp, state.fp, state.arg[0],
                      state.arg[1], state.arg[2]);
        std::string value = text;
        if (state.arg_word_valid) {
            char word[32];
            std::snprintf(word, sizeof word, " *r0=0x%08x", state.arg_word);
            value += word;
        }
        // A thread that keeps returning to the same few PCs is spinning, not running a frame loop.
        if (!state.chain.empty()) value += " lr-in=" + state.chain.front();
        std::deque<std::string>& recent = g_recent_pcs[candidate.tid];
        const std::string here = process.describe_code_address(state.pc);
        if (!here.empty() && (recent.empty() || recent.back() != here)) {
            recent.push_back(here);
            if (recent.size() > 24) recent.pop_front();
        }
        if (!recent.empty()) {
            value += " recent=";
            for (std::size_t i = 0; i < recent.size(); ++i) {
                if (i != 0) value += ',';
                value += recent[i];
            }
        }
        if (!state.chain.empty()) {
            value += " chain=";
            for (std::size_t i = 0; i < state.chain.size(); ++i) {
                if (i != 0) value += " <- ";
                value += state.chain[i];
            }
        }
        runtime_report().note_watch_detail("stack-" + std::to_string(candidate.tid), value);
        ++written;
    }
}

namespace {

struct ProcMemStatus {
    bool valid = false;
    std::uint64_t vm_size_kib = 0;
    std::uint64_t vm_rss_kib = 0;
    std::uint64_t rss_anon_kib = 0;
    std::uint64_t vm_swap_kib = 0;
    std::uint64_t threads = 0;
};

ProcMemStatus read_proc_mem_status() {
    ProcMemStatus out;
    std::FILE* file = std::fopen("/proc/self/status", "re");
    if (file == nullptr) return out;
    char line[256];
    while (std::fgets(line, sizeof line, file) != nullptr) {
        unsigned long long val = 0;
        if (std::sscanf(line, "VmSize: %llu kB", &val) == 1) {
            out.vm_size_kib = val;
            out.valid = true;
        } else if (std::sscanf(line, "VmRSS: %llu kB", &val) == 1) {
            out.vm_rss_kib = val;
            out.valid = true;
        } else if (std::sscanf(line, "RssAnon: %llu kB", &val) == 1) {
            out.rss_anon_kib = val;
        } else if (std::sscanf(line, "VmSwap: %llu kB", &val) == 1) {
            out.vm_swap_kib = val;
        } else if (std::sscanf(line, "Threads: %llu", &val) == 1) {
            out.threads = val;
        }
    }
    std::fclose(file);
    return out;
}

std::string compact_image_label(const std::string& label) {
    const std::size_t slash = label.find_last_of('/');
    return slash == std::string::npos ? label : label.substr(slash + 1);
}

std::string format_compact_chain(const std::vector<std::string>& chain, std::size_t max_items) {
    std::string out;
    const std::size_t count = std::min(chain.size(), max_items);
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) out += "<-";
        out += compact_image_label(chain[i]);
    }
    return out;
}

struct StallThreadSample {
    std::int32_t tid = 0;
    std::string name;
    char state = '?';
    std::uint64_t dcpu = 0;
    std::string act;
    Process::ThreadStateSample guest;
    std::string pc_desc;
    std::string lr_desc;
};

// Samples guest threads, memory footprints, and VM counters every second whenever eglSwapBuffers
// goes silent for >= 3 seconds, emitting live logcat lines ("zbridge") and bounded report notes
// for the longest stall seen in this run.
class FrameStallSampler {
public:
    static constexpr std::uint64_t kStallThresholdNs = 3'000'000'000ULL;
    static constexpr std::size_t kMaxMilestones = 8;

    void sample(Process& process, std::uint64_t now_ns, std::uint64_t swaps,
                std::uint64_t last_swap_ns) {
        if (swaps == 0 || last_swap_ns == 0 || now_ns <= last_swap_ns) return;

        const bool frame_stalled = (now_ns - last_swap_ns) >= kStallThresholdNs;
        if (!frame_stalled && !in_stall_) {
            // While frames are swapping smoothly, refresh the baseline only every 5s and skip
            // opening wchan files so watchdog monitoring adds near-zero syscall noise.
            if (baseline_mem_.valid && ++baseline_ticks_ < 5) return;
            baseline_ticks_ = 0;
            baseline_mem_ = read_proc_mem_status();
            baseline_vm_ = snapshot_guest_vm_counters();
            update_cpu_baseline(snapshot_thread_activity());
            return;
        }

        const ProcMemStatus mem = read_proc_mem_status();
        const GuestVmCounters vm = snapshot_guest_vm_counters();
        const std::vector<ThreadActivitySample> activities = snapshot_thread_activity();
        std::unordered_map<std::int32_t, ThreadActivitySample> act_by_tid;
        for (const ThreadActivitySample& a : activities) act_by_tid[a.tid] = a;

        if (in_stall_ && swaps != stall_swaps_) {
            finish_stall(last_swap_ns, now_ns, swaps, mem, vm);
            in_stall_ = false;
        }
        if (!frame_stalled) {
            baseline_ticks_ = 0;
            baseline_mem_ = mem;
            baseline_vm_ = vm;
            update_cpu_baseline(activities);
            return;
        }
        if (!in_stall_) {
            in_stall_ = true;
            stall_swaps_ = swaps;
            stall_start_ns_ = last_swap_ns;
            start_mem_ = baseline_mem_.valid ? baseline_mem_ : mem;
            start_vm_ = baseline_vm_;
            tick_ = 0;
            next_milestone_sec_ = 3;
            milestones_written_ = 0;
        }

        const std::uint64_t elapsed_ms = (now_ns - stall_start_ns_) / 1'000'000ULL;
        const std::int64_t d_rss_m =
            static_cast<std::int64_t>(mem.vm_rss_kib / 1024) -
            static_cast<std::int64_t>(start_mem_.vm_rss_kib / 1024);
        const std::int64_t d_swap_m =
            static_cast<std::int64_t>(mem.vm_swap_kib / 1024) -
            static_cast<std::int64_t>(start_mem_.vm_swap_kib / 1024);
        const std::uint64_t d_mmap = vm.mmap_calls - start_vm_.mmap_calls;
        const std::uint64_t d_mmap_m = (vm.mmap_bytes - start_vm_.mmap_bytes) / (1024 * 1024);
        const std::uint64_t d_munmap = vm.munmap_calls - start_vm_.munmap_calls;
        const std::uint64_t d_munmap_m = (vm.munmap_bytes - start_vm_.munmap_bytes) / (1024 * 1024);
        const std::uint64_t d_madv = vm.madvise_dontneed_calls - start_vm_.madvise_dontneed_calls;
        const std::uint64_t d_brk = vm.brk_calls - start_vm_.brk_calls;
        const std::int64_t d_brk_m = (vm.brk_net_bytes - start_vm_.brk_net_bytes) / (1024 * 1024);
        const std::uint64_t d_borrow = vm.borrower_creates - start_vm_.borrower_creates;
        const std::uint64_t d_clone = vm.clone_calls - start_vm_.clone_calls;
        const std::uint64_t d_inv = vm.jit_invalidations - start_vm_.jit_invalidations;

        log("STALL %llums swaps=%llu rss=%lluM(%+lldM) swap=%lluM(%+lldM) anon=%lluM vsize=%lluM th=%llu "
            "mmap=%llu(+%lluM) unmap=%llu(%lluM) madv=%llu brk=%llu(%+lldM) brw=%llu clone=%llu inv=%llu",
            static_cast<unsigned long long>(elapsed_ms), static_cast<unsigned long long>(swaps),
            static_cast<unsigned long long>(mem.vm_rss_kib / 1024), static_cast<long long>(d_rss_m),
            static_cast<unsigned long long>(mem.vm_swap_kib / 1024), static_cast<long long>(d_swap_m),
            static_cast<unsigned long long>(mem.rss_anon_kib / 1024),
            static_cast<unsigned long long>(mem.vm_size_kib / 1024),
            static_cast<unsigned long long>(mem.threads), static_cast<unsigned long long>(d_mmap),
            static_cast<unsigned long long>(d_mmap_m), static_cast<unsigned long long>(d_munmap),
            static_cast<unsigned long long>(d_munmap_m), static_cast<unsigned long long>(d_madv),
            static_cast<unsigned long long>(d_brk), static_cast<long long>(d_brk_m),
            static_cast<unsigned long long>(d_borrow), static_cast<unsigned long long>(d_clone),
            static_cast<unsigned long long>(d_inv));

        std::vector<StallThreadSample> active = collect_active_threads(process, act_by_tid);
        const std::size_t to_log = std::min<std::size_t>(active.size(), 4);
        for (std::size_t i = 0; i < to_log; ++i) {
            const StallThreadSample& t = active[i];
            const std::string chain = format_compact_chain(t.guest.chain, 4);
            log("STALL-TH t=%llums tid=%d(%s) %c dcpu=%llu act=%s pc=0x%08x(%s) lr=0x%08x(%s) chain=%s",
                static_cast<unsigned long long>(elapsed_ms), t.tid, t.name.c_str(), t.state,
                static_cast<unsigned long long>(t.dcpu), t.act.c_str(), t.guest.pc,
                t.pc_desc.c_str(), t.guest.lr, t.lr_desc.c_str(), chain.c_str());
        }

        ++tick_;
        if (elapsed_ms >= longest_stall_ms_) {
            longest_stall_ms_ = elapsed_ms;
            char buf[384];
            std::snprintf(buf, sizeof buf,
                          "active max_ms=%llu swaps=%llu rss=%lluM(%+lldM) swap=%lluM(%+lldM) anon=%lluM th=%llu",
                          static_cast<unsigned long long>(elapsed_ms), static_cast<unsigned long long>(swaps),
                          static_cast<unsigned long long>(mem.vm_rss_kib / 1024),
                          static_cast<long long>(d_rss_m),
                          static_cast<unsigned long long>(mem.vm_swap_kib / 1024),
                          static_cast<long long>(d_swap_m),
                          static_cast<unsigned long long>(mem.rss_anon_kib / 1024),
                          static_cast<unsigned long long>(mem.threads));
            runtime_report().note_watch_detail("stall-summary", buf);
            std::snprintf(
                buf, sizeof buf,
                "mmap=%llu(+%lluM) munmap=%llu(%lluM) mremap=%llu madv_dontneed=%llu brk=%llu(%+lldM) "
                "clone=%llu borrow=%llu inv=%llu",
                static_cast<unsigned long long>(d_mmap), static_cast<unsigned long long>(d_mmap_m),
                static_cast<unsigned long long>(d_munmap), static_cast<unsigned long long>(d_munmap_m),
                static_cast<unsigned long long>(vm.mremap_calls - start_vm_.mremap_calls),
                static_cast<unsigned long long>(d_madv), static_cast<unsigned long long>(d_brk),
                static_cast<long long>(d_brk_m), static_cast<unsigned long long>(d_clone),
                static_cast<unsigned long long>(d_borrow), static_cast<unsigned long long>(d_inv));
            runtime_report().note_watch_detail("stall-mem", buf);

            const std::uint64_t elapsed_sec = elapsed_ms / 1000ULL;
            if (milestones_written_ < kMaxMilestones && elapsed_sec >= next_milestone_sec_) {
                ++milestones_written_;
                next_milestone_sec_ = next_milestone_sec_ < 10 ? elapsed_sec + 4 : elapsed_sec * 2;
                std::snprintf(buf, sizeof buf, "t=%llus rss=%lluM swap=%lluM mmap=+%llu brk=%+lldM inv=+%llu",
                              static_cast<unsigned long long>(elapsed_sec),
                              static_cast<unsigned long long>(mem.vm_rss_kib / 1024),
                              static_cast<unsigned long long>(mem.vm_swap_kib / 1024),
                              static_cast<unsigned long long>(d_mmap),
                              static_cast<long long>(d_brk_m),
                              static_cast<unsigned long long>(d_inv));
                std::string note = buf;
                const std::size_t note_threads = std::min<std::size_t>(active.size(), 2);
                for (std::size_t i = 0; i < note_threads; ++i) {
                    const StallThreadSample& t = active[i];
                    std::snprintf(buf, sizeof buf, " | %d:%s %c dcpu=%llu %s pc=0x%08x(%s) chain=%s",
                                  t.tid, t.name.c_str(), t.state,
                                  static_cast<unsigned long long>(t.dcpu), t.act.c_str(),
                                  t.guest.pc, t.pc_desc.c_str(),
                                  format_compact_chain(t.guest.chain, 3).c_str());
                    note += buf;
                }
                runtime_report().note_watch_detail("stall-t" + std::to_string(milestones_written_), note);
            }
        }
    }

private:
    void finish_stall(std::uint64_t last_swap_ns, std::uint64_t now_ns, std::uint64_t swaps,
                      const ProcMemStatus& mem, const GuestVmCounters& vm) {
        const std::uint64_t end_ns = last_swap_ns > stall_start_ns_ ? last_swap_ns : now_ns;
        const std::uint64_t dur_ms = (end_ns - stall_start_ns_) / 1'000'000ULL;
        const std::int64_t d_rss_m =
            static_cast<std::int64_t>(mem.vm_rss_kib / 1024) -
            static_cast<std::int64_t>(start_mem_.vm_rss_kib / 1024);
        const std::int64_t d_swap_m =
            static_cast<std::int64_t>(mem.vm_swap_kib / 1024) -
            static_cast<std::int64_t>(start_mem_.vm_swap_kib / 1024);
        const std::uint64_t d_mmap = vm.mmap_calls - start_vm_.mmap_calls;
        const std::int64_t d_brk_m = (vm.brk_net_bytes - start_vm_.brk_net_bytes) / (1024 * 1024);
        const std::uint64_t d_borrow = vm.borrower_creates - start_vm_.borrower_creates;
        const std::uint64_t d_inv = vm.jit_invalidations - start_vm_.jit_invalidations;
        log("STALL-END %llums swaps=%llu->%llu rss=%lluM(%+lldM) swap=%lluM(%+lldM) "
            "mmap=+%llu brk=%+lldM brw=+%llu inv=+%llu",
            static_cast<unsigned long long>(dur_ms), static_cast<unsigned long long>(stall_swaps_),
            static_cast<unsigned long long>(swaps),
            static_cast<unsigned long long>(mem.vm_rss_kib / 1024), static_cast<long long>(d_rss_m),
            static_cast<unsigned long long>(mem.vm_swap_kib / 1024), static_cast<long long>(d_swap_m),
            static_cast<unsigned long long>(d_mmap), static_cast<long long>(d_brk_m),
            static_cast<unsigned long long>(d_borrow), static_cast<unsigned long long>(d_inv));
        if (dur_ms >= longest_stall_ms_) {
            longest_stall_ms_ = dur_ms;
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "recovered dur_ms=%llu swaps=%llu->%llu rss=%lluM(%+lldM) swap=%lluM(%+lldM) "
                          "mmap=+%llu brk=%+lldM brw=+%llu inv=+%llu",
                          static_cast<unsigned long long>(dur_ms),
                          static_cast<unsigned long long>(stall_swaps_),
                          static_cast<unsigned long long>(swaps),
                          static_cast<unsigned long long>(mem.vm_rss_kib / 1024),
                          static_cast<long long>(d_rss_m),
                          static_cast<unsigned long long>(mem.vm_swap_kib / 1024),
                          static_cast<long long>(d_swap_m), static_cast<unsigned long long>(d_mmap),
                          static_cast<long long>(d_brk_m), static_cast<unsigned long long>(d_borrow),
                          static_cast<unsigned long long>(d_inv));
            runtime_report().note_watch_detail("stall-last", buf);
        }
    }

    void update_cpu_baseline(const std::vector<ThreadActivitySample>& activities) {
        prev_cpu_.clear();
        for (const ThreadActivitySample& a : activities) {
            const ThreadKernelState ks = read_thread_kernel_state_impl(a.tid, false);
            if (ks.present) prev_cpu_[a.tid] = ks.cpu;
        }
    }

    std::vector<StallThreadSample> collect_active_threads(
        Process& process, const std::unordered_map<std::int32_t, ThreadActivitySample>& act_by_tid) {
        std::vector<StallThreadSample> out;
        DIR* dir = ::opendir("/proc/self/task");
        if (dir == nullptr) return out;
        while (const dirent* entry = ::readdir(dir)) {
            if (entry->d_name[0] == '.') continue;
            const std::int32_t tid = std::atoi(entry->d_name);
            if (tid == 0) continue;
            const std::string name = thread_name(tid);
            if (is_platform_thread(name.c_str())) continue;
            Process::ThreadStateSample gst;
            if (!process.sample_thread_state(tid, gst)) continue;
            const ThreadKernelState ks = read_thread_kernel_state(tid);
            if (!ks.present) continue;
            const auto prev_it = prev_cpu_.find(tid);
            const std::uint64_t prev_cpu = prev_it == prev_cpu_.end() ? ks.cpu : prev_it->second;
            const std::uint64_t dcpu = ks.cpu >= prev_cpu ? ks.cpu - prev_cpu : 0;
            prev_cpu_[tid] = ks.cpu;

            const auto act_it = act_by_tid.find(tid);
            const bool is_unity_main = std::strncmp(name.c_str(), "UnityMain", 9) == 0;
            const bool is_active_call =
                act_it != act_by_tid.end() && act_it->second.in_progress &&
                !(act_it->second.kind == ThreadActivityKind::kHostCall && act_it->second.id == 0xfe01);
            if (!is_unity_main && ks.state != 'R' && dcpu == 0 && !is_active_call) continue;

            StallThreadSample s;
            s.tid = tid;
            s.name = name;
            s.state = ks.state;
            s.dcpu = dcpu;
            s.act = act_it == act_by_tid.end() ? "guest" : describe_thread_activity(act_it->second);
            s.guest = std::move(gst);
            s.pc_desc = compact_image_label(process.describe_code_address(s.guest.pc));
            s.lr_desc = compact_image_label(process.describe_code_address(s.guest.lr));
            out.push_back(std::move(s));
        }
        ::closedir(dir);
        std::sort(out.begin(), out.end(), [](const StallThreadSample& a, const StallThreadSample& b) {
            const bool a_main = std::strncmp(a.name.c_str(), "UnityMain", 9) == 0;
            const bool b_main = std::strncmp(b.name.c_str(), "UnityMain", 9) == 0;
            if (a_main != b_main) return a_main;
            if (a.dcpu != b.dcpu) return a.dcpu > b.dcpu;
            return (a.state == 'R') > (b.state == 'R');
        });
        return out;
    }

public:
    bool is_stalled() const { return in_stall_; }

private:
    bool in_stall_ = false;
    unsigned baseline_ticks_ = 0;
    std::uint64_t stall_swaps_ = 0;
    std::uint64_t stall_start_ns_ = 0;
    std::uint64_t longest_stall_ms_ = 0;
    std::uint64_t tick_ = 0;
    std::uint64_t next_milestone_sec_ = 3;
    std::size_t milestones_written_ = 0;
    ProcMemStatus baseline_mem_{};
    ProcMemStatus start_mem_{};
    GuestVmCounters baseline_vm_{};
    GuestVmCounters start_vm_{};
    std::unordered_map<std::int32_t, std::uint64_t> prev_cpu_;
};

}  // namespace

HangWatchdog::HangWatchdog(SnapshotFn snapshot) : snapshot_(std::move(snapshot)) {}

bool HangWatchdog::sample(Clock::time_point now) {
    const std::vector<ThreadActivitySample> current = snapshot_();

    std::vector<State> next;
    next.reserve(current.size());
    for (const ThreadActivitySample& sample : current) {
        State* prior = nullptr;
        for (State& state : previous_) {
            if (state.tid == sample.tid) {
                prior = &state;
                break;
            }
        }
        State state;
        state.tid = sample.tid;
        state.kind = sample.kind;
        state.id = sample.id;
        state.counter = sample.counter;
        state.in_progress = sample.in_progress;
        state.pc = sample.pc;
        state.where = sample.where;
        if (prior != nullptr && prior->kind == sample.kind && prior->id == sample.id &&
            prior->counter == sample.counter) {
            state.since = prior->since;
            state.unchanged_samples = prior->unchanged_samples + 1;
            // Only inspect /proc when a thread stops advancing its activity counter:
            // unchanged_samples == 1 seeds the CPU baseline; >= kStuckThreshold reads wchan & name.
            if (state.unchanged_samples >= kStuckThreshold) {
                state.kernel = read_thread_kernel_state_impl(sample.tid, true);
                state.name = !prior->name.empty() ? prior->name : thread_name(sample.tid);
            } else {
                state.kernel = read_thread_kernel_state_impl(sample.tid, false);
            }
            if (state.kernel.cpu > prior->kernel.cpu) {
                state.cpu_delta = state.kernel.cpu - prior->kernel.cpu;
            }
        } else {
            state.since = now;
            state.unchanged_samples = 0;
        }
        next.push_back(std::move(state));
    }
    previous_ = std::move(next);

    std::vector<const State*> stuck;
    for (const State& state : previous_) {
        // A slot outlives the thread that claimed it, and a thread the kernel no longer knows can
        // never make progress again. Counting such a slot as stuck spends the line on something
        // that is gone: on the device five of them had accumulated by the time a freeze began.
        if (!state.kernel.present) continue;
        if (state.unchanged_samples >= kStuckThreshold) stuck.push_back(&state);
    }

    // A spinner never leaves translated code, so its recorded activity stands still exactly like
    // an idle futex waiter's; the kernel tells them apart. Order by "running now", then by CPU
    // burned since the previous sample, then by the engine's own thread names, so the thread that
    // is actually driving the freeze is listed first rather than crowded out by a long-idle pool.
    std::stable_sort(stuck.begin(), stuck.end(), [](const State* a, const State* b) {
        const bool a_running = a->kernel.state == 'R';
        const bool b_running = b->kernel.state == 'R';
        if (a_running != b_running) return a_running;
        if (a->cpu_delta != b->cpu_delta) return a->cpu_delta > b->cpu_delta;
        const int a_rank = engine_thread_priority(a->name);
        const int b_rank = engine_thread_priority(b->name);
        if (a_rank != b_rank) return a_rank < b_rank;
        return a->tid < b->tid;
    });

    // One line per shown thread, built once: the current line and the history notes describe the
    // same list. `identity` is the same information without the elapsed seconds that tick every
    // sample, so only a real change makes the current line rewrite the report.
    std::vector<std::string> entries;
    std::string identity;
    std::string shown_tids;
    for (const State* state : stuck) {
        if (entries.size() >= kMaxThreadsShown) break;
        ThreadActivitySample sample;
        sample.tid = state->tid;
        sample.kind = state->kind;
        sample.id = state->id;
        sample.counter = state->counter;
        sample.in_progress = state->in_progress;
        const std::string activity = describe_thread_activity(sample);
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(now - state->since).count();
        std::string value = std::to_string(state->tid) + "=" + activity +
                            " x" + std::to_string(state->counter) + " stuck=" +
                            std::to_string(seconds) + " " + format_thread_kernel_state(state->kernel);
        if (!state->name.empty()) value += " name=" + state->name;
        if (state->cpu_delta != 0) value += " cpu-delta=" + std::to_string(state->cpu_delta);
        if (state->pc != 0) {
            char pc[96];
            std::snprintf(pc, sizeof pc, " pc=0x%08x%s%s", state->pc,
                          state->where.empty() ? "" : " in ", state->where.c_str());
            value += pc;
        }
        entries.push_back(std::move(value));
        // Deliberately not the raw cpu ticks: they climb every sample for a spinning thread and
        // would rewrite the report each time, while the fact that it is busy is what changed.
        identity += std::to_string(state->tid) + ":" + activity + ":" + state->kernel.state + ":" +
                    state->kernel.waiting + ":" + (state->cpu_delta != 0 ? "busy" : "idle") + ";";
        shown_tids += (shown_tids.empty() ? "" : ",") + std::to_string(state->tid);
    }

    std::string listed;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i != 0) listed += " | ";
        listed += entries[i];
    }

    // What is stuck right now, whether or not the history budget is left: a busy engine spends the
    // six notes within its first samples (on the device all six landed by t=21 s), so a freeze
    // that starts a minute later had no line at all. This one is replaced in place, so whenever the
    // report is read it describes the moment of reading.
    if (!current_written_ || identity != last_current_identity_ ||
        now - current_written_at_ >= kCurrentRefresh) {
        current_written_ = true;
        last_current_identity_ = identity;
        current_written_at_ = now;
        runtime_report().note_watch_detail("current", entries.empty() ? "(none)" : listed);
    }

    if (stuck.empty()) return false;
    if (notes_written_ >= kMaxNotes) return false;
    // One note per change of the stuck set. Idle waiters are stuck from the first sample, so
    // writing a note on every sample spends the whole budget long before a real freeze begins.
    if (shown_tids == last_note_tids_) return false;
    last_note_tids_ = shown_tids;
    std::string value = listed;
    value += " | recent-jni: " + jni_recent_calls();
    // Once, with the first report: the whole thread census is long and does not change much.
    static bool census_written = false;
    if (!census_written) {
        census_written = true;
        write_threads_census();
    }

    ++notes_written_;
    runtime_report().note_watch_detail(std::to_string(notes_written_), value);
    return true;
}

void HangWatchdog::run_forever() {
    for (;;) {
        std::this_thread::sleep_for(kInterval);
        sample(Clock::now());
    }
}

#if !defined(ZB_NO_TELEMETRY)
void start_hang_watchdog(Process* process) {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    std::thread([process]() {
        // Lightweight activity snapshots on normal ticks avoid taking Process mutexes and walking
        // 8KB guest stacks every 3s during smooth rendering; full stack inspection only runs
        // when a frame stall or new stuck thread is actually detected.
        HangWatchdog watchdog{HangWatchdog::SnapshotFn(snapshot_thread_activity)};
        FrameStallSampler stall_sampler;
        unsigned tick = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (process != nullptr) {
                stall_sampler.sample(*process, frame_probe_now_ns(), frame_probe_total_swaps(),
                                     frame_probe_last_swap_ns());
            }
            if (++tick % 3 == 0) {
                const bool stuck_note = watchdog.sample(HangWatchdog::Clock::now());
                if (process != nullptr && (stuck_note || stall_sampler.is_stalled())) {
                    write_thread_stacks(*process);
                }
            }
        }
    }).detach();
}
#endif

}  // namespace zb
