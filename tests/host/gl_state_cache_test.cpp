#include <sys/mman.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_egl.h"
#include "mock_gles.h"
#include "mock_jvm.h"
#include "zb/gl_hostcalls.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/host_gl.h"
#include "zb/library_protocol.h"
#include "zb/library_runtime.h"
#include "zb/proxy_runtime.h"
#include "zb/runtime_report.h"

namespace {

constexpr std::uint32_t kStack = 0x10000;
constexpr std::uint32_t kData = 0x20000;

void set_words(zb::LibraryRuntime& runtime, zb::GuestThread& thread,
               std::initializer_list<std::uint32_t> words) {
    thread.regs()[13] = kStack;
    std::size_t idx = 0;
    for (std::uint32_t w : words) {
        if (idx < 4) {
            thread.regs()[idx] = w;
        } else {
            std::memcpy(runtime.memory().base() + kStack + (idx - 4) * sizeof(std::uint32_t), &w,
                        sizeof(w));
        }
        ++idx;
    }
}

void test_state_filter_disabled_by_default() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kStack, 0x10000, PROT_READ | PROT_WRITE));
    Dynarmic::ExclusiveMonitor monitor(1);
    MockGles backend;
    zb::HostGl host(runtime, backend);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);

    CHECK(!host.state_filter_enabled());
    set_words(runtime, thread, {0x84C0u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glActiveTexture, thread));
    set_words(runtime, thread, {0x84C0u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glActiveTexture, thread));
    CHECK(backend.calls().size() == 2);
}

void test_host_gl_state_dedup_and_invalidation() {
    zb::runtime_report().clear();
    zb::LibraryRuntime runtime;
    auto& mem = runtime.memory();
    CHECK(mem.map_anon(kStack, 0x20000, PROT_READ | PROT_WRITE));
    Dynarmic::ExclusiveMonitor monitor(1);
    MockGles backend;
    zb::HostGl host(runtime, backend);
    host.set_state_filter_enabled(true);
    zb::GuestThread thread(mem, &monitor, 0, false, zb::kCarrierCodeCacheSize);

    // ActiveTexture + BindTexture per unit.
    set_words(runtime, thread, {0x84C0u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glActiveTexture, thread));
    set_words(runtime, thread, {0x84C0u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glActiveTexture, thread));
    CHECK(backend.calls().size() == 1);

    set_words(runtime, thread, {0x0DE1u, 55u});  // GL_TEXTURE_2D
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindTexture, thread));
    set_words(runtime, thread, {0x0DE1u, 55u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindTexture, thread));
    CHECK(backend.calls().size() == 2);

    // Different unit binds tex 55 cleanly.
    set_words(runtime, thread, {0x84C1u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glActiveTexture, thread));
    set_words(runtime, thread, {0x0DE1u, 55u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindTexture, thread));
    CHECK(backend.calls().size() == 4);

    // glDeleteTextures invalidates tex 55 across cached units.
    const std::uint32_t del_tex = 55u;
    std::memcpy(mem.base() + kData, &del_tex, sizeof(del_tex));
    set_words(runtime, thread, {1u, kData});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glDeleteTextures, thread));
    CHECK(backend.calls().size() == 5);
    set_words(runtime, thread, {0x0DE1u, 55u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindTexture, thread));
    CHECK(backend.calls().size() == 6);

    // UseProgram dedup + glDeleteProgram invalidation.
    set_words(runtime, thread, {42u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glUseProgram, thread));
    set_words(runtime, thread, {42u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glUseProgram, thread));
    CHECK(backend.calls().size() == 7);
    set_words(runtime, thread, {42u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glDeleteProgram, thread));
    set_words(runtime, thread, {42u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glUseProgram, thread));
    CHECK(backend.calls().size() == 9);

    // BindBuffer dedup + VAO invalidation of EBO (0x8893).
    set_words(runtime, thread, {0x8892u, 7u});  // GL_ARRAY_BUFFER
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindBuffer, thread));
    set_words(runtime, thread, {0x8892u, 7u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindBuffer, thread));
    CHECK(backend.calls().size() == 10);

    set_words(runtime, thread, {10u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindVertexArray, thread));
    set_words(runtime, thread, {0x8893u, 21u});  // GL_ELEMENT_ARRAY_BUFFER
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindBuffer, thread));
    set_words(runtime, thread, {11u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindVertexArray, thread));
    // Rebinding EBO=21 on VAO 11 MUST NOT be skipped by VAO 10's cached EBO=21!
    set_words(runtime, thread, {0x8893u, 21u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBindBuffer, thread));
    CHECK(backend.calls().size() == 14);

    // glEnable / glDisable known bit + unknown cap bypass.
    set_words(runtime, thread, {0x0BE2u});  // GL_BLEND
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glEnable, thread));
    set_words(runtime, thread, {0x0BE2u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glEnable, thread));
    CHECK(backend.calls().size() == 15);
    set_words(runtime, thread, {0x0BE2u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glDisable, thread));
    set_words(runtime, thread, {0x0BE2u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glDisable, thread));
    CHECK(backend.calls().size() == 16);

    // Out-of-table cap never deduplicated.
    set_words(runtime, thread, {0x1234u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glEnable, thread));
    set_words(runtime, thread, {0x1234u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glEnable, thread));
    CHECK(backend.calls().size() == 18);

    // DepthMask / ColorMask / Viewport / DepthRangef dedup + EGL context switch reset.
    const std::uint32_t f0 = std::bit_cast<std::uint32_t>(0.0f);
    const std::uint32_t f1 = std::bit_cast<std::uint32_t>(1.0f);
    set_words(runtime, thread, {f0, f1});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glDepthRangef, thread));
    set_words(runtime, thread, {f0, f1});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glDepthRangef, thread));
    CHECK(backend.calls().size() == 19);

    zb::runtime_report().note_egl_current(4444u);
    set_words(runtime, thread, {f0, f1});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glDepthRangef, thread));
    CHECK(backend.calls().size() == 20);

    // glBlendFunc(s, d) shares normalized state with glBlendFuncSeparate(s, d, s, d).
    set_words(runtime, thread, {0x0302u, 0x0303u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBlendFunc, thread));
    set_words(runtime, thread, {0x0302u, 0x0303u, 0x0302u, 0x0303u});
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glBlendFuncSeparate, thread));
    CHECK(backend.calls().size() == 21);
}

int count_calls(const MockGles& backend, const char* name) {
    int n = 0;
    for (const auto& c : backend.calls()) {
        if (c.name == name) ++n;
    }
    return n;
}

void run_guest_cache_mode(int argc, char** argv) {
    CHECK(argc == 5);
    auto* vm = new zb::mock::MockJvm();
    auto* gl_backend = new MockGles();
    auto* egl_backend = new MockEgl();
    egl_backend->set_result("eglGetDisplay", 0x7000);
    egl_backend->set_result("eglInitialize", 1);
    egl_backend->set_configs({reinterpret_cast<void*>(0x1000)});
    egl_backend->set_result("eglCreateContext", 0x3000);
    egl_backend->set_result("eglMakeCurrent", 1);

    auto* engine = new zb::GuestJniEngine(*vm, gl_backend, {}, nullptr, egl_backend, nullptr);
    // Keep Host-side state filtering OFF so 100% of deduplication measured by MockGles proves
    // the arm32 guest libGLESv2.so TLS cache filtered the redundant calls before trapping to Host.
    engine->host_gl()->set_state_filter_enabled(false);

    zb::LibraryRuntimeOptions options;
    options.sysroot = argv[2];
    options.zbhost = argv[3];
    options.target_sdk = 16;
    options.guest_environment = {std::string("LD_LIBRARY_PATH=") + argv[4]};
    options.preload = "libzbjni.so";
    std::string error;
    if (!engine->start(options, error)) {
        std::fprintf(stderr, "engine->start failed: %s\n", error.c_str());
        CHECK(false);
    }

    zb::LibraryRuntime& runtime = engine->runtime();
    const std::uint32_t library =
        runtime.load_library(std::string(argv[4]) + "/libzbglcacheprobe.so", ZB_GUEST_RTLD_NOW, error);
    CHECK(library != 0);
    const std::uint32_t probe = runtime.find_symbol(library, "zb_gl_cache_probe", error);
    CHECK(probe != 0);

    zb::GuestCall call;
    const auto res = runtime.call_on_service(probe, call);
    CHECK(res.has_value() && res->r0 == 0);

    // Expect: glUseProgram x3 -> 1 (initial) + 1 (after glDeleteProgram) + 1 (after eglMakeCurrent) = 3
    CHECK(count_calls(*gl_backend, "glUseProgram") == 3);
    CHECK(count_calls(*gl_backend, "glActiveTexture") == 1);
    CHECK(count_calls(*gl_backend, "glBindTexture") == 1);
    CHECK(count_calls(*gl_backend, "glBindBuffer") == 2);
    // glEnable(GL_BLEND) x3 -> 1 (initial) + 0 (after glDeleteProgram) + 1 (after eglMakeCurrent) = 2
    CHECK(count_calls(*gl_backend, "glEnable") == 2);
    // glBlendFunc + equivalent glBlendFuncSeparate -> 1
    CHECK(count_calls(*gl_backend, "glBlendFunc") == 1);
    CHECK(count_calls(*gl_backend, "glBlendFuncSeparate") == 0);
    CHECK(count_calls(*gl_backend, "glViewport") == 1);
    CHECK(count_calls(*gl_backend, "glBindFramebuffer") == 1);
    CHECK(count_calls(*gl_backend, "glEnableVertexAttribArray") == 2);
    CHECK(count_calls(*gl_backend, "glDisableVertexAttribArray") == 1);
    CHECK(count_calls(*gl_backend, "glStencilMask") == 1);
    CHECK(count_calls(*gl_backend, "glStencilFuncSeparate") == 1);
    CHECK(count_calls(*gl_backend, "glStencilFunc") == 0);
    CHECK(count_calls(*gl_backend, "glStencilOpSeparate") == 1);
    CHECK(count_calls(*gl_backend, "glStencilOp") == 0);
    CHECK(count_calls(*gl_backend, "glClearDepthf") == 1);
    CHECK(count_calls(*gl_backend, "glPixelStorei") == 1);
    CHECK(count_calls(*gl_backend, "glVertexAttribPointer") == 1);
    CHECK(count_calls(*gl_backend, "glClear") == 1);
    CHECK(count_calls(*gl_backend, "glDrawElements") == 1);
    CHECK(count_calls(*gl_backend, "glBufferSubData") == 1);
    CHECK(count_calls(*gl_backend, "glUniform4fv") == 1);
    CHECK(count_calls(*gl_backend, "glDeleteProgram") == 1);

    std::puts("gl_state_cache_test guest-cache passed");
    std::fflush(stdout);
    std::_Exit(0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "guest-cache") == 0) {
        run_guest_cache_mode(argc, argv);
        return 0;
    }
    test_state_filter_disabled_by_default();
    test_host_gl_state_dedup_and_invalidation();
    std::printf("gl_state_cache_test passed\n");
    return 0;
}
