// HangWatchdog: the per-thread activity table and the sampler's stuck-thread decision, driven
// with a fake snapshot source and an explicit clock so it needs no real guest thread or timing.
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "zb/hang_watchdog.h"
#include "zb/process.h"
#include "zb/runtime_report.h"

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// A thread that exists but does nothing. The watchdog asks the kernel about a tid, so a thread it
// can describe has to be real: a made-up tid reads as "gone", and a gone tid is not a stuck thread.
struct IdleThread {
    explicit IdleThread(const char* name) {
        thread = std::thread([this, name]() {
            pthread_setname_np(pthread_self(), name);
            tid = static_cast<std::int32_t>(::syscall(SYS_gettid));
            while (!stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        while (tid == 0) std::this_thread::yield();
    }
    ~IdleThread() {
        stop.store(true, std::memory_order_relaxed);
        thread.join();
    }
    IdleThread(const IdleThread&) = delete;
    IdleThread& operator=(const IdleThread&) = delete;

    std::thread thread;
    std::atomic<bool> stop{false};
    std::int32_t tid = 0;
};

}  // namespace

int main() {
    // record_thread_activity / snapshot_thread_activity: a fresh tid claims a slot, repeated
    // calls bump the counter and update the recorded activity in place.
    zb::record_thread_activity(9001, zb::ThreadActivityKind::kSyscall, 3);
    zb::record_thread_activity(9001, zb::ThreadActivityKind::kSyscall, 3);
    zb::record_thread_activity(9001, zb::ThreadActivityKind::kHostCall, 0x2a);
    {
        const auto snapshot = zb::snapshot_thread_activity();
        bool found = false;
        for (const auto& sample : snapshot) {
            if (sample.tid != 9001) continue;
            found = true;
            CHECK(sample.kind == zb::ThreadActivityKind::kHostCall);
            CHECK(sample.id == 0x2a);
            CHECK(sample.counter == 3);
            // Recorded calls start as in progress; the marker says so until they finish. A stub
            // index is named from the generated table, whichever call it is.
            CHECK(zb::describe_thread_activity(sample) == "host:libGLESv2.so glEnable(inside)");
            zb::record_thread_activity_done(9001);
            auto after = zb::snapshot_thread_activity();
            for (const auto& done : after) {
                if (done.tid == 9001) CHECK(zb::describe_thread_activity(done) == "host:libGLESv2.so glEnable");
            }
        }
        CHECK(found);
    }

    // A host call with no table entry prints its raw index, and a flat JNI host call (0xFC00+,
    // served by libzbjni.so) prints its generated JNI name instead of "? ?". A frozen report is
    // read without the tables at hand, so neither case may degrade to an unreadable line.
    {
        zb::ThreadActivitySample unknown{};
        unknown.tid = 9002;
        unknown.kind = zb::ThreadActivityKind::kHostCall;
        unknown.id = 0x7f01;
        unknown.in_progress = true;
        CHECK(zb::describe_thread_activity(unknown) == "host:0x7f01(inside)");

        zb::ThreadActivitySample jni{};
        jni.tid = 9003;
        jni.kind = zb::ThreadActivityKind::kHostCall;
        jni.id = 0xfc2fu;  // ZB_JNI_HC_ExceptionCheck
        jni.in_progress = true;
        CHECK(zb::describe_thread_activity(jni) == "host:libzbjni.so ExceptionCheck(inside)");
    }

    // The library runtime serves its own host calls (0xFE00-0xFEFF) inside the runtime, so they are
    // in neither generated table. Naming them matters: a carrier park lasts exactly as long as the
    // parked carrier's guest identity is borrowed, which is minutes, and reads as a huge stall
    // unless the report says which call it was.
    {
        zb::ThreadActivitySample park{};
        park.tid = 9004;
        park.kind = zb::ThreadActivityKind::kHostCall;
        park.id = 0xfe01;  // ZB_CARRIER_PARK_INDEX
        park.in_progress = true;
        CHECK(zb::describe_thread_activity(park) == "host:guest runtime carrier-park(inside)");

        zb::ThreadActivitySample ready{};
        ready.tid = 9005;
        ready.kind = zb::ThreadActivityKind::kHostCall;
        ready.id = 0xfe00;  // ZB_SERVICE_READY_INDEX
        CHECK(zb::describe_thread_activity(ready) == "host:guest runtime ready");
    }

    // A park is a lease, not work. It returns only when the borrowing Java thread releases the
    // carrier, so its wall time is that thread's lifetime: the device report listed one as a
    // 199-second host call, which reads as a stall that never happened and spends the slow-call
    // budget on a normal event.
    {
        CHECK(zb::host_call_is_lease(0xfe01));   // ZB_CARRIER_PARK_INDEX
        CHECK(!zb::host_call_is_lease(0xfe00));  // ZB_SERVICE_READY_INDEX
        CHECK(!zb::host_call_is_lease(0x2a));    // an ordinary stub call
    }

    // thread_name: the kernel's name for a live thread, and nothing for a tid that does not exist
    // in this process. The slow-host-call report relies on it to identify a tid that the one-time
    // census never listed.
    {
        const auto self = static_cast<std::int32_t>(::syscall(SYS_gettid));
        CHECK(!zb::thread_name(self).empty());
        CHECK(zb::thread_name(0).empty());
        CHECK(zb::thread_name(1).empty());
    }

    // HangWatchdog::sample: a thread whose activity never changes across kStuckThreshold + 1
    // samples is reported as stuck; one that keeps moving never is. Both tids belong to real
    // threads of this process, because the watchdog describes them from /proc.
    zb::runtime_report().clear();
    using Clock = zb::HangWatchdog::Clock;
    Clock::time_point now{};
    bool stuck_activity = true;  // toggled by the fake snapshot to simulate the moving thread
    IdleThread idle_first("idle-first");
    IdleThread idle_second("idle-second");

    zb::HangWatchdog watchdog([&]() {
        std::vector<zb::ThreadActivitySample> out;
        zb::ThreadActivitySample stuck;
        stuck.tid = idle_first.tid;
        stuck.kind = zb::ThreadActivityKind::kSyscall;
        stuck.id = 7;
        stuck.counter = 42;  // never changes
        out.push_back(stuck);

        zb::ThreadActivitySample moving;
        moving.tid = idle_second.tid;
        moving.kind = zb::ThreadActivityKind::kHostCall;
        moving.id = 5;
        static std::uint64_t moving_counter = 0;
        if (stuck_activity) ++moving_counter;
        moving.counter = moving_counter;
        out.push_back(moving);
        return out;
    });

    // Sample 1: establishes the baseline for both threads; nothing is stuck yet.
    CHECK(!watchdog.sample(now));
    now += std::chrono::seconds(3);
    // Sample 2: the first thread unchanged once, the second still moving; below the threshold.
    CHECK(!watchdog.sample(now));
    now += std::chrono::seconds(3);
    // Sample 3: the first thread unchanged twice in a row -> stuck. The second keeps moving.
    CHECK(watchdog.sample(now));

    const std::string text = zb::runtime_report().text();
    CHECK(contains(text, "watch-1: " + std::to_string(idle_first.tid) + "=sys:"));
    CHECK(contains(text, "x42"));
    CHECK(contains(text, "stuck=6"));
    CHECK(!contains(text, std::to_string(idle_second.tid) + "=host:"));
    // The kernel's name for the tid, and the fact that it is sleeping rather than running.
    CHECK(contains(text, "name=idle-first"));

    // Once a thread starts moving again it drops out of the stuck list; if nothing new is stuck,
    // no further note is written.
    zb::runtime_report().clear();
    IdleThread idle_third("idle-third");
    zb::HangWatchdog watchdog2([&]() {
        std::vector<zb::ThreadActivitySample> out;
        zb::ThreadActivitySample sample;
        sample.tid = idle_third.tid;
        sample.kind = zb::ThreadActivityKind::kSyscall;
        sample.id = 1;
        static std::uint64_t counter = 0;
        sample.counter = ++counter;  // always moving
        out.push_back(sample);
        return out;
    });
    Clock::time_point now2{};
    CHECK(!watchdog2.sample(now2));
    now2 += std::chrono::seconds(3);
    CHECK(!watchdog2.sample(now2));
    now2 += std::chrono::seconds(3);
    CHECK(!watchdog2.sample(now2));
    CHECK(zb::runtime_report().text().find("watch-1") == std::string::npos);

    // A guest that spins inside translated code leaves it neither through a syscall nor through a
    // host call, so its recorded activity stands still exactly like an idle futex waiter's. The
    // device freeze showed eight of those waiters in every note while UnityMain - running
    // translated code with no exit, cpu climbing - was invisible. The kernel separates the two:
    // the spinner is running and burning ticks.
    {
        std::atomic<std::int32_t> spinner_tid{0};
        std::atomic<bool> spin{true};
        std::thread spinner([&]() {
            pthread_setname_np(pthread_self(), "UnityMain");
            spinner_tid.store(static_cast<std::int32_t>(::syscall(SYS_gettid)));
            while (spin.load(std::memory_order_relaxed)) {
                volatile std::uint64_t burn = 0;
                for (int i = 0; i < 100000; ++i) burn += static_cast<std::uint64_t>(i);
            }
        });
        while (spinner_tid.load() == 0) std::this_thread::yield();
        const std::int32_t spinning = spinner_tid.load();

        zb::runtime_report().clear();
        zb::HangWatchdog watchdog3([&]() {
            std::vector<zb::ThreadActivitySample> out;
            for (std::int32_t tid = 9100; tid < 9116; ++tid) {
                zb::ThreadActivitySample idle;
                idle.tid = tid;
                idle.kind = zb::ThreadActivityKind::kSyscall;
                idle.id = 240;  // futex
                idle.counter = 9;
                out.push_back(idle);
            }
            zb::ThreadActivitySample spinning_sample;
            spinning_sample.tid = spinning;
            spinning_sample.kind = zb::ThreadActivityKind::kHostCall;
            spinning_sample.id = 5;  // a call it made once and never repeated
            spinning_sample.counter = 3;
            out.push_back(spinning_sample);
            return out;
        });
        Clock::time_point now3{};
        CHECK(!watchdog3.sample(now3));
        now3 += std::chrono::seconds(3);
        // Real time must pass for the kernel's CPU ticks to move; the report's clock is explicit.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(!watchdog3.sample(now3));
        now3 += std::chrono::seconds(3);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(watchdog3.sample(now3));
        const std::string spin_text = zb::runtime_report().text();
        // Listed at all, with the kernel's name for the tid so the line is readable without the
        // census.
        CHECK(contains(spin_text, std::to_string(spinning) + "=host:"));
        CHECK(contains(spin_text, "name=UnityMain"));
        // Not merely running: burning CPU between two samples, which is the spin itself.
        CHECK(contains(spin_text, "cpu-delta="));
        // The sixteen tids above never existed; a slot outlives the thread that claimed it, and a
        // thread that is gone can never make progress again. On the device five such slots had
        // accumulated by the time the freeze began, and they crowded real threads out of the list.
        CHECK(!contains(spin_text, "9100="));
        CHECK(!contains(spin_text, "gone"));

        spin.store(false, std::memory_order_relaxed);
        spinner.join();
    }

    // The note budget is spent on changes of the stuck set. Idle waiters are stuck from the first
    // sample, so a note per sample used up all six long before the guest froze, and the freeze
    // itself was never described.
    {
        zb::runtime_report().clear();
        IdleThread idle_early("idle-early");
        IdleThread idle_late("idle-late");
        bool include_late = false;
        zb::HangWatchdog watchdog4([&]() {
            std::vector<zb::ThreadActivitySample> out;
            zb::ThreadActivitySample early;
            early.tid = idle_early.tid;
            early.kind = zb::ThreadActivityKind::kSyscall;
            early.id = 240;  // futex
            early.counter = 1;
            out.push_back(early);
            if (include_late) {
                zb::ThreadActivitySample late;
                late.tid = idle_late.tid;
                late.kind = zb::ThreadActivityKind::kSyscall;
                late.id = 240;
                late.counter = 1;
                out.push_back(late);
            }
            return out;
        });
        Clock::time_point now4{};
        CHECK(!watchdog4.sample(now4));
        now4 += std::chrono::seconds(3);
        CHECK(!watchdog4.sample(now4));
        now4 += std::chrono::seconds(3);
        CHECK(watchdog4.sample(now4));  // {early} first becomes stuck
        for (int i = 0; i < 4; ++i) {
            now4 += std::chrono::seconds(3);
            CHECK(!watchdog4.sample(now4));  // the same set: no new note
        }
        CHECK(!contains(zb::runtime_report().text(), "watch-2:"));

        include_late = true;
        now4 += std::chrono::seconds(3);
        CHECK(!watchdog4.sample(now4));  // a member joined, still below the threshold
        now4 += std::chrono::seconds(3);
        CHECK(!watchdog4.sample(now4));  // unchanged once
        now4 += std::chrono::seconds(3);
        CHECK(watchdog4.sample(now4));  // the set changed: this is worth a note
        const std::string late_text = zb::runtime_report().text();
        CHECK(contains(late_text, "watch-2: "));
        // Order inside the line follows the kernel, so only membership is asserted here.
        CHECK(contains(late_text, std::to_string(idle_early.tid) + "="));
        CHECK(contains(late_text, std::to_string(idle_late.tid) + "="));
    }

    // The six history notes are a fixed budget, and a real run spends them immediately: on the
    // device every note landed within the first 21 seconds, while the Bluetooth freeze began about
    // a minute later and was described by none of them. A current line that is overwritten in
    // place - not appended, not budgeted - therefore has to say what is stuck at the moment the
    // report is read.
    {
        zb::runtime_report().clear();
        IdleThread idle_always("idle-always");
        IdleThread idle_toggled("idle-toggled");
        bool include_toggled = false;
        zb::HangWatchdog watchdog5([&]() {
            std::vector<zb::ThreadActivitySample> out;
            auto add = [&out](std::int32_t tid) {
                zb::ThreadActivitySample sample;
                sample.tid = tid;
                sample.kind = zb::ThreadActivityKind::kSyscall;
                sample.id = 240;  // futex
                sample.counter = 1;
                out.push_back(sample);
            };
            add(idle_always.tid);
            if (include_toggled) add(idle_toggled.tid);
            return out;
        });
        Clock::time_point now5{};
        for (int i = 0; i < 30; ++i) {
            include_toggled = (i % 6) >= 3;  // three samples in, three out
            now5 += std::chrono::seconds(3);
            watchdog5.sample(now5);
        }
        const std::string spent_text = zb::runtime_report().text();
        CHECK(contains(spent_text, "watch-6: "));
        CHECK(!contains(spent_text, "watch-7:"));

        // The budget is gone; a thread that becomes stuck now must still appear.
        include_toggled = true;
        for (int i = 0; i < 3; ++i) {
            now5 += std::chrono::seconds(3);
            CHECK(!watchdog5.sample(now5));  // no history note left to write
        }
        const std::string current_text = zb::runtime_report().text();
        CHECK(contains(current_text, "watch-current: "));
        CHECK(contains(current_text, std::to_string(idle_always.tid) + "="));
        CHECK(contains(current_text, std::to_string(idle_toggled.tid) + "="));
        // The current line holds the kernel's facts too, so it needs no census.
        CHECK(contains(current_text, "name=idle-toggled"));
    }

    // First-time activity slot registration must set in_progress immediately.
    {
        const std::int32_t synthetic_tid = 919191;
        zb::record_thread_activity(synthetic_tid, zb::ThreadActivityKind::kHostCall, 0x1234);
        bool found_active = false;
        for (const zb::ThreadActivitySample& s : zb::snapshot_thread_activity()) {
            if (s.tid == synthetic_tid) {
                CHECK(s.kind == zb::ThreadActivityKind::kHostCall);
                CHECK(s.id == 0x1234);
                CHECK(s.counter == 1);
                CHECK(s.in_progress);
                found_active = true;
                break;
            }
        }
        CHECK(found_active);
        zb::record_thread_activity_done(synthetic_tid);
        bool found_done = false;
        for (const zb::ThreadActivitySample& s : zb::snapshot_thread_activity()) {
            if (s.tid == synthetic_tid) {
                CHECK(!s.in_progress);
                found_done = true;
                break;
            }
        }
        CHECK(found_done);
    }

    // Guest VM / JIT diagnostic counters accumulate deltas cleanly.
    {
        const zb::GuestVmCounters before = zb::snapshot_guest_vm_counters();
        zb::record_guest_mmap(4096);
        zb::record_guest_munmap(4096);
        zb::record_guest_mremap();
        zb::record_guest_madvise_dontneed(8192);
        zb::record_guest_brk(16384);
        zb::record_guest_clone();
        zb::record_guest_borrower();
        zb::record_jit_invalidate();
        const zb::GuestVmCounters after = zb::snapshot_guest_vm_counters();
        CHECK(after.mmap_calls == before.mmap_calls + 1);
        CHECK(after.mmap_bytes == before.mmap_bytes + 4096);
        CHECK(after.munmap_calls == before.munmap_calls + 1);
        CHECK(after.munmap_bytes == before.munmap_bytes + 4096);
        CHECK(after.mremap_calls == before.mremap_calls + 1);
        CHECK(after.madvise_dontneed_calls == before.madvise_dontneed_calls + 1);
        CHECK(after.madvise_dontneed_bytes == before.madvise_dontneed_bytes + 8192);
        CHECK(after.brk_calls == before.brk_calls + 1);
        CHECK(after.brk_net_bytes == before.brk_net_bytes + 16384);
        CHECK(after.clone_calls == before.clone_calls + 1);
        CHECK(after.borrower_creates == before.borrower_creates + 1);
        CHECK(after.jit_invalidations == before.jit_invalidations + 1);
    }

    // A borrowed guest thread keeps the carrier's guest tid for signal routing while attributing
    // diagnostics and register state to its active host thread (e.g. UnityMain).
    {
        zb::Process proc;
        Dynarmic::ExclusiveMonitor monitor(4);
        zb::GuestThread carrier(proc.memory(), &monitor, 0, false);
        carrier.tid = 20001;
        carrier.regs()[15] = 0x11110000u;
        CHECK(carrier.diagnostic_tid() == 20001);
        carrier.host_tid.store(20099, std::memory_order_relaxed);
        CHECK(carrier.diagnostic_tid() == 20099);
        carrier.host_tid.store(0, std::memory_order_relaxed);

        std::unique_ptr<zb::GuestThread> borrower = proc.create_borrower(carrier);
        CHECK(borrower != nullptr);
        borrower->regs()[15] = 0x22220000u;
        CHECK(borrower->tid == 20001);
        CHECK(borrower->diagnostic_tid() == 20001);

        borrower->host_tid.store(30002, std::memory_order_relaxed);
        CHECK(borrower->diagnostic_tid() == 30002);

        zb::Process::ThreadStateSample sample;
        CHECK(proc.sample_thread_state(30002, sample));
        CHECK(sample.pc == 0x22220000u);
        // Active host_tid keeps carrier tid from matching the borrower's registers.
        CHECK(!proc.sample_thread_state(20001, sample));

        borrower->host_tid.store(0, std::memory_order_relaxed);
        CHECK(!proc.sample_thread_state(30002, sample));
        proc.destroy_borrower(std::move(borrower), carrier);
    }

    std::puts("hang_watchdog_test: ok");
    return 0;
}
