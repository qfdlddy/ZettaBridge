#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_gles.h"
#include "mock_jvm.h"
#include "zb/gl_hostcalls.h"
#include "zb/guest_io.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/host_gl.h"
#include "zb/host_jni.h"
#include "zb/jni_hostcalls.h"
#include "zb/jni_protocol.h"
#include "zb/library_runtime.h"
#include "zb/native_call.h"
#include "zb/process.h"
#include "zb/runtime_report.h"
#include "zb/syscalls.h"

namespace {

constexpr std::uint32_t kCpsrUser = 0x10;

void put32(zb::GuestMemory& mem, std::uint32_t addr, std::uint32_t value) {
    std::memcpy(mem.base() + addr, &value, sizeof(value));
}

struct FastHookState {
    zb::Process* proc = nullptr;
    int calls = 0;
    int handled = 0;
    bool observed_in_fast_svc = false;
};

bool test_svc_hook(void* ctx, zb::GuestThread& thread, std::uint32_t swi) {
    if (swi == 0x5AFFFFu) return false;
    auto* state = static_cast<FastHookState*>(ctx);
    ++state->calls;
    if (thread.in_fast_svc()) state->observed_in_fast_svc = true;
    if (swi == 0x5A0000u) {
        thread.regs()[0] += 10u;
        ++state->handled;
        return true;
    }
    if (swi == 0u && state->proc != nullptr) {
        if (zb::try_fast_syscall(*state->proc, thread)) {
            ++state->handled;
            return true;
        }
    }
    return false;
}

void test_guest_thread_fast_svc_and_fallback() {
    zb::GuestMemory mem;
    CHECK(mem.ok());
    CHECK(mem.map_anon(0x10000, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(mem.map_anon(0x20000, 0x2000, PROT_READ | PROT_WRITE));
    CHECK(mem.map_anon(0xFFFF0000, 0x1000, PROT_READ | PROT_WRITE));

    put32(mem, 0x10000, 0xEF5A0000);  // svc #0x5a0000 (fast handled)
    put32(mem, 0x10004, 0xEF5A0000);  // svc #0x5a0000 (fast handled)
    put32(mem, 0x10008, 0xEF5A0001);  // svc #0x5a0001 (falls back to stop handler)
    put32(mem, 0x1000C, 0xE12FFF1E);  // bx lr
    put32(mem, zb::kHostReturnAddress, 0xEF5AFFFF);
    CHECK(mem.protect(0x10000, 0x1000, PROT_READ | PROT_EXEC));
    CHECK(mem.protect(0xFFFF0000, 0x1000, PROT_READ | PROT_EXEC));

    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(mem, &monitor, 0, false, zb::kCarrierCodeCacheSize);
    thread.regs()[13] = 0x22000;
    thread.set_cpsr(kCpsrUser);

    FastHookState hook;
    thread.set_fast_svc_handler(test_svc_hook, &hook);
    CHECK(!thread.in_fast_svc());

    int stop_svcs = 0;
    zb::GuestCall call;
    call.regs = {5, 0, 0, 0};
    const auto result = thread.call(0x10000, call, [&](const zb::Stop& stop) {
        CHECK(!thread.in_fast_svc());
        CHECK(stop.kind == zb::StopKind::Svc && stop.swi == 0x5A0001u);
        ++stop_svcs;
        thread.regs()[0] += 100u;
        return true;
    });

    CHECK(result.has_value());
    CHECK(result->r0 == 125u);  // 5 + 10 + 10 + 100
    CHECK(hook.calls == 3);
    CHECK(hook.handled == 2);
    CHECK(hook.observed_in_fast_svc);
    CHECK(stop_svcs == 1);
    CHECK(!thread.in_fast_svc());

    // Pending signal must suppress fast SVC so signal delivery runs on the standard stop path.
    hook = FastHookState{};
    zb::g::siginfo32 sig{};
    sig.si_signo = 10;
    thread.post_signal(sig);
    int signal_stops = 0;
    call.regs = {1, 0, 0, 0};
    const auto sig_res = thread.call(0x10000, call, [&](const zb::Stop& stop) {
        ++signal_stops;
        if (stop.swi == 0x5A0000u) thread.regs()[0] += 1u;
        if (stop.swi == 0x5A0001u) thread.regs()[0] += 2u;
        return true;
    });
    CHECK(sig_res.has_value());
    CHECK(signal_stops >= 2);
    // First 0x5a0000 fell back because pending signal was non-zero.
    CHECK(hook.calls < 3);
    CHECK(thread.has_pending_signals(0));
}

void test_fast_syscall_in_jit() {
    zb::Process proc;
    auto& mem = proc.memory();
    CHECK(mem.map_anon(0x10000, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(mem.map_anon(0x20000, 0x2000, PROT_READ | PROT_WRITE));
    CHECK(mem.map_anon(0x30000, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(mem.map_anon(0xFFFF0000, 0x1000, PROT_READ | PROT_WRITE));

    put32(mem, 0x10000, 0xEF000000);  // svc #0
    put32(mem, 0x10004, 0xE12FFF1E);  // bx lr
    put32(mem, zb::kHostReturnAddress, 0xEF5AFFFF);
    CHECK(mem.protect(0x10000, 0x1000, PROT_READ | PROT_EXEC));
    CHECK(mem.protect(0xFFFF0000, 0x1000, PROT_READ | PROT_EXEC));

    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(mem, &monitor, 0, false, zb::kCarrierCodeCacheSize);
    thread.regs()[13] = 0x22000;
    thread.set_cpsr(kCpsrUser);

    FastHookState hook;
    hook.proc = &proc;
    thread.set_fast_svc_handler(test_svc_hook, &hook);

    int slow_syscalls = 0;
    auto run_eabi_syscall = [&](std::uint32_t nr, std::uint32_t r0, std::uint32_t r1 = 0,
                                std::uint32_t r2 = 0, std::uint32_t r3 = 0,
                                std::uint32_t r4 = 0, std::uint32_t r5 = 0) -> std::uint32_t {
        thread.regs()[4] = r4;
        thread.regs()[5] = r5;
        thread.regs()[7] = nr;
        zb::GuestCall c;
        c.regs = {r0, r1, r2, r3};
        const auto res = thread.call(0x10000, c, [&](const zb::Stop& stop) {
            CHECK(stop.kind == zb::StopKind::Svc && stop.swi == 0u);
            ++slow_syscalls;
            CHECK(zb::handle_syscall(proc, thread));
            return true;
        });
        CHECK(res.has_value());
        return res->r0;
    };

    // 1. gettid (224), getpid (20)
    const int before_slow = slow_syscalls;
    CHECK(run_eabi_syscall(224, 0) == static_cast<std::uint32_t>(::syscall(SYS_gettid)));
    thread.tid = 4242;
    CHECK(run_eabi_syscall(224, 0) == 4242u);
    CHECK(run_eabi_syscall(20, 0) == static_cast<std::uint32_t>(::getpid()));
    CHECK(slow_syscalls == before_slow);

    // 2. clock_gettime (263), clock_gettime64 (403), gettimeofday (78)
    CHECK(run_eabi_syscall(263, CLOCK_MONOTONIC, 0x30100) == 0u);
    std::uint32_t ts32[2]{};
    std::memcpy(ts32, mem.base() + 0x30100, sizeof(ts32));
    CHECK(ts32[0] != 0u || ts32[1] != 0u);

    CHECK(run_eabi_syscall(403, CLOCK_MONOTONIC, 0x30120) == 0u);
    std::int64_t ts64[2]{};
    std::memcpy(ts64, mem.base() + 0x30120, sizeof(ts64));
    CHECK(ts64[0] != 0 || ts64[1] != 0);

    CHECK(run_eabi_syscall(78, 0x30140, 0) == 0u);
    std::uint32_t tv32[2]{};
    std::memcpy(tv32, mem.base() + 0x30140, sizeof(tv32));
    CHECK(tv32[0] != 0u);
    CHECK(slow_syscalls == before_slow);

    // 3. Cached APK read/pread64: first miss runs slow syscall (which populates cache),
    // subsequent read & pread64 hit fast path with zero stop handler invocations.
    const char* tmpdir = std::getenv("TMPDIR");
    const std::string path = std::string((tmpdir != nullptr && tmpdir[0] != '\0') ? tmpdir : "/data/local/tmp") +
                             "/zb_fast_svc_" + std::to_string(::getpid()) + ".apk";
    std::vector<std::uint8_t> payload(256);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i ^ 0x5Au);

    const int wfd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    CHECK(wfd >= 0);
    CHECK(::write(wfd, payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
    CHECK(::close(wfd) == 0);

    const int non_apk_fd = ::open(path.c_str(), O_RDONLY);
    CHECK(non_apk_fd >= 0);
    // Unregistered fd falls back to slow syscall.
    CHECK(run_eabi_syscall(3, static_cast<std::uint32_t>(non_apk_fd), 0x30180, 12) == 12u);
    CHECK(slow_syscalls == before_slow + 1);
    CHECK(std::memcmp(mem.base() + 0x30180, payload.data(), 12) == 0);
    ::close(non_apk_fd);

    const int rfd = ::open(path.c_str(), O_RDONLY);
    CHECK(rfd >= 0);
    zb::guest_io_note_apk_fd(rfd, true);

    // Registered APK fd is served synchronously in-JIT by try_fast_syscall with 0 slow stops.
    CHECK(run_eabi_syscall(3, static_cast<std::uint32_t>(rfd), 0x30200, 16) == 16u);
    CHECK(slow_syscalls == before_slow + 1);
    CHECK(std::memcmp(mem.base() + 0x30200, payload.data(), 16) == 0);

    CHECK(run_eabi_syscall(3, static_cast<std::uint32_t>(rfd), 0x30240, 32) == 32u);
    CHECK(slow_syscalls == before_slow + 1);
    CHECK(std::memcmp(mem.base() + 0x30240, payload.data() + 16, 32) == 0);

    // pread64 (NR=180): ARM EABI passes (fd, buf, count, pad, off_low, off_high).
    CHECK(run_eabi_syscall(180, static_cast<std::uint32_t>(rfd), 0x30300, 24, 0, 100, 0) == 24u);
    CHECK(slow_syscalls == before_slow + 1);
    CHECK(std::memcmp(mem.base() + 0x30300, payload.data() + 100, 24) == 0);

    // 4. _llseek (140) and lseek (19) on APK fd run in-JIT with zero slow stops.
    CHECK(run_eabi_syscall(140, static_cast<std::uint32_t>(rfd), 0, 8, 0x30380, SEEK_SET) == 0u);
    std::int64_t sought = -1;
    std::memcpy(&sought, mem.base() + 0x30380, sizeof(sought));
    CHECK(sought == 8);
    CHECK(run_eabi_syscall(19, static_cast<std::uint32_t>(rfd), 12, SEEK_SET) == 12u);
    CHECK(slow_syscalls == before_slow + 1);

    // 5. sched_yield (158), rt_sigprocmask (175), futex(FUTEX_WAKE_PRIVATE) (240) run in-JIT;
    // futex(FUTEX_WAIT_PRIVATE) still falls back to slow path.
    CHECK(run_eabi_syscall(158, 0) == 0u);
    std::uint64_t new_mask = 1ULL << 9;  // block signal 10
    std::uint64_t old_mask = 0xFFFFFFFFFFFFFFFFULL;
    std::memcpy(mem.base() + 0x30400, &new_mask, sizeof(new_mask));
    std::memcpy(mem.base() + 0x30410, &old_mask, sizeof(old_mask));
    CHECK(run_eabi_syscall(175, 0 /* SIG_BLOCK */, 0x30400, 0x30410, 8) == 0u);
    std::memcpy(&old_mask, mem.base() + 0x30410, sizeof(old_mask));
    CHECK(old_mask == 0ULL);
    CHECK(thread.sigmask == new_mask);
    std::uint32_t futex_word = 1u;
    std::memcpy(mem.base() + 0x30420, &futex_word, sizeof(futex_word));
    CHECK(run_eabi_syscall(240, 0x30420, FUTEX_WAKE_PRIVATE, 1) == 0u);
    CHECK(slow_syscalls == before_slow + 1);

    // 5b. getuid32 (199), fstat64 (197), and fcntl64(F_GETFD) (221) run in-JIT with zero slow stops.
    CHECK(run_eabi_syscall(199, 0) == static_cast<std::uint32_t>(::getuid()));
    CHECK(run_eabi_syscall(197, static_cast<std::uint32_t>(rfd), 0x30480) == 0u);
    CHECK(run_eabi_syscall(221, static_cast<std::uint32_t>(rfd), F_GETFD, 0) !=
          static_cast<std::uint32_t>(-1));
    CHECK(slow_syscalls == before_slow + 1);

    // FUTEX_WAIT_PRIVATE with mismatched value returns -EAGAIN via slow path.
    CHECK(run_eabi_syscall(240, 0x30420, FUTEX_WAIT_PRIVATE, 99) == static_cast<std::uint32_t>(-11));
    CHECK(slow_syscalls == before_slow + 2);

    ::close(rfd);
    ::unlink(path.c_str());
}

void test_host_gl_fast_leaf_and_fallback() {
    zb::runtime_report().clear();
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(0x10000, 0x10000, PROT_READ | PROT_WRITE));

    Dynarmic::ExclusiveMonitor monitor(1);
    MockGles backend;
    zb::HostGl host(runtime, backend);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);

    // Before first handle_host_call (egl_context_checked_ is false), fast path declines.
    thread.regs()[0] = 0x84C0u;
    CHECK(!host.handle_fast_host_call(zb::ZB_GL_HC_glActiveTexture, thread));

    // Seed context check + initial thread sample via standard path.
    thread.regs()[0] = 0;
    thread.regs()[1] = 0;
    thread.regs()[2] = 0;
    thread.regs()[3] = 0;
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glClearColor, thread));
    CHECK(backend.calls().size() == 1);

    // Leaf GL call succeeds in fast path and dispatches to backend.
    thread.regs()[0] = 0x84C1u;
    CHECK(host.handle_fast_host_call(zb::ZB_GL_HC_glActiveTexture, thread));
    CHECK(backend.calls().size() == 2);
    CHECK(backend.calls().back().name == "glActiveTexture");

    // Allocating / error-querying GL calls must decline fast path cleanly.
    CHECK(!host.handle_fast_host_call(zb::ZB_GL_HC_glGetString, thread));
    CHECK(!host.handle_fast_host_call(zb::ZB_GL_HC_glGetStringi, thread));
    CHECK(!host.handle_fast_host_call(zb::ZB_GL_HC_glMapBufferRange, thread));
    CHECK(!host.handle_fast_host_call(zb::ZB_GL_HC_glMapBufferOES, thread));
    CHECK(!host.handle_fast_host_call(zb::ZB_GL_HC_glUnmapBuffer, thread));
    CHECK(!host.handle_fast_host_call(zb::ZB_GL_HC_glUnmapBufferOES, thread));
    CHECK(!host.handle_fast_host_call(zb::ZB_GL_HC_glGetError, thread));
    CHECK(backend.calls().size() == 2);

    // EGL context switch invalidates fast-path generation until one slow call re-samples thread.
    zb::runtime_report().note_egl_current(99999);
    thread.regs()[0] = 0x84C2u;
    CHECK(!host.handle_fast_host_call(zb::ZB_GL_HC_glActiveTexture, thread));
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glActiveTexture, thread));
    thread.regs()[0] = 0x84C3u;
    CHECK(host.handle_fast_host_call(zb::ZB_GL_HC_glActiveTexture, thread));
    CHECK(backend.calls().size() == 4);
}

void test_host_gl_pooled_map_unmap_fast() {
    zb::runtime_report().clear();
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(0x10000, 0x10000, PROT_READ | PROT_WRITE));
    CHECK(runtime.memory().map_anon(0x40000, 0x1000, PROT_READ | PROT_WRITE));

    Dynarmic::ExclusiveMonitor monitor(1);
    MockGles backend;
    zb::HostGl host(runtime, backend);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);

    // Seed egl_context_checked_ and egl_generation via one slow call.
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glClearColor, thread));
    backend.clear_calls();

    // Seed thread-local mirror pool with a 256-byte block at guest 0x40000.
    host.release_mirror(0x40000, 256);

    // Bind GL_ARRAY_BUFFER = 77 via fast path so mapping_key/can_fast_unmap_buffer know the binding.
    thread.regs()[0] = 0x8892u;
    thread.regs()[1] = 77u;
    CHECK(host.handle_fast_host_call(zb::ZB_GL_HC_glBindBuffer, thread));

    std::array<std::uint8_t, 64> driver_range{};
    driver_range.fill(0x5Au);
    backend.set_result("glMapBufferRange", reinterpret_cast<std::uint64_t>(driver_range.data()));
    backend.set_result("glUnmapBuffer", 1);

    // Pool hit: glMapBufferRange succeeds in fast path and returns pooled guest mirror 0x40000.
    thread.regs()[0] = 0x8892u;
    thread.regs()[1] = 0u;
    thread.regs()[2] = 64u;
    thread.regs()[3] = 0x0003u;  // GL_MAP_READ_BIT | GL_MAP_WRITE_BIT
    CHECK(host.handle_fast_host_call(zb::ZB_GL_HC_glMapBufferRange, thread));
    CHECK(thread.regs()[0] == 0x40000u);
    CHECK(std::memcmp(runtime.memory().base() + 0x40000, driver_range.data(), 64) == 0);

    // Modify guest mirror and glUnmapBuffer via fast path; recycled back to local_mirror_pool.
    std::memset(runtime.memory().base() + 0x40000, 0x3Cu, 64);
    thread.regs()[0] = 0x8892u;
    CHECK(host.handle_fast_host_call(zb::ZB_GL_HC_glUnmapBuffer, thread));
    CHECK(thread.regs()[0] == 1u);
    CHECK(std::memcmp(driver_range.data(), runtime.memory().base() + 0x40000, 64) == 0);

    // Pool miss (request 512 bytes > 256-byte pooled block): declines BEFORE calling backend.
    const std::size_t calls_before_miss = backend.calls().size();
    thread.regs()[0] = 0x8892u;
    thread.regs()[1] = 0u;
    thread.regs()[2] = 512u;
    thread.regs()[3] = 0x0003u;
    CHECK(!host.handle_fast_host_call(zb::ZB_GL_HC_glMapBufferRange, thread));
    CHECK(backend.calls().size() == calls_before_miss);
}

void test_host_jni_fast_leaf_and_fallback() {
    auto* runtime = new zb::LibraryRuntime();
    CHECK(runtime->memory().map_anon(0x10000, 0x10000, PROT_READ | PROT_WRITE));
    auto* monitor = new Dynarmic::ExclusiveMonitor(1);
    auto* jvm = new zb::mock::MockJvm();
    auto* jni = new zb::HostJni(*runtime, *jvm);
    zb::GuestThread thread(runtime->memory(), monitor, 0, false, zb::kCarrierCodeCacheSize);

    // Before registration, fast path declines.
    CHECK(!jni->handle_fast_host_call(ZB_JNI_HC_GetEnv, thread));

    zb_jni_guest_api api{};
    api.size = sizeof(api);
    api.version = ZB_JNI_PROTOCOL_VERSION;
    api.java_vm = 0x12340u;
    api.new_env_fn = 0x12380u;
    api.free_env_fn = 0x123C0u;
    std::memcpy(runtime->memory().base() + 0x11000, &api, sizeof(api));
    thread.regs()[0] = 0x11000u;
    CHECK(!jni->handle_fast_host_call(ZB_JNI_HC_Register, thread));
    CHECK(jni->handle_host_call(ZB_JNI_HC_Register, thread));
    CHECK(thread.regs()[0] == 1u);
    CHECK(jni->ready());

    // Leaf calls run synchronously via handle_fast_host_call.
    thread.regs()[0] = 999u;
    CHECK(jni->handle_fast_host_call(ZB_JNI_HC_GetEnv, thread));
    CHECK(thread.regs()[0] == 0u);  // calling thread has no attached JNIEnv

    thread.regs()[0] = 0u;
    CHECK(jni->handle_fast_host_call(ZB_JNI_HC_GetObjectRefType, thread));
    CHECK(thread.regs()[0] == 0u);

    // Re-entrant / class-init / blocking JNI calls must decline fast path cleanly.
    CHECK(!jni->handle_fast_host_call(ZB_JNI_HC_CallMethodA, thread));
    CHECK(!jni->handle_fast_host_call(ZB_JNI_HC_FindClass, thread));
    CHECK(!jni->handle_fast_host_call(ZB_JNI_HC_GetMethodID, thread));
    CHECK(!jni->handle_fast_host_call(ZB_JNI_HC_MonitorEnter, thread));
    CHECK(!jni->handle_fast_host_call(ZB_JNI_HC_GetDirectBufferAddress, thread));
    CHECK(!jni->handle_fast_host_call(ZB_JNI_HC_AttachCurrentThread, thread));
    CHECK(!jni->handle_fast_host_call(ZB_JNI_HC_DetachCurrentThread, thread));
}

}  // namespace

int main() {
    test_guest_thread_fast_svc_and_fallback();
    test_fast_syscall_in_jit();
    test_host_gl_fast_leaf_and_fallback();
    test_host_gl_pooled_map_unmap_fast();
    test_host_jni_fast_leaf_and_fallback();
    std::printf("fast_svc_test passed\n");
    return 0;
}
