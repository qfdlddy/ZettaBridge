#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_gles.h"
#include "mock_gles1.h"
#include "zb/gles1_hostcalls.h"
#include "zb/gl_hostcalls.h"
#include "zb/host_gl.h"
#include "zb/runtime_report.h"

namespace {

constexpr std::uint32_t kStack = 0x10000;
constexpr std::uint32_t kData = 0x20000;
// A page mapped read-only: legal as an input, an output must be rejected before the driver.
constexpr std::uint32_t kReadOnly = 0x30000;
constexpr std::uint32_t kOutput = kData + 0x300;

void set_words(zb::LibraryRuntime& runtime, zb::GuestThread& thread,
               const std::array<std::uint32_t, 12>& words) {
    for (unsigned i = 0; i < 4; ++i) thread.regs()[i] = words[i];
    thread.regs()[13] = kStack;
    std::memcpy(runtime.memory().base() + kStack, words.data() + 4,
                (words.size() - 4) * sizeof(words[0]));
}

const MockGles1::Call& only_call(const MockGles1& backend) {
    CHECK(backend.calls().size() == 1);
    return backend.calls()[0];
}

}  // namespace

int main() {
    // The table is the whole contract of this range: 87 contiguous indices, the pointerless
    // ones are what Task 2 forwards, and the name helpers agree with it.
    CHECK(zb::kGl1HostCallFirst == 492);
    CHECK(zb::kGl1HostCallLast == 578);
    CHECK(zb::kGl1HostCallCount == 87);
    CHECK(zb::kGl1PointerHostCallCount == 37);
    CHECK(std::strcmp(zb::gl1_host_call_name(492), "glAlphaFunc") == 0);
    CHECK(std::strcmp(zb::gl1_host_call_name(578), "glVertexPointer") == 0);
    CHECK(zb::gl1_host_call(491) == nullptr);
    CHECK(zb::gl1_host_call(579) == nullptr);
    CHECK(!zb::is_gl1_host_call(zb::kGlHostCallLast));
    CHECK(!zb::is_gl1_host_call(zb::kGlHostCallExtLast));

    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kStack, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(runtime.memory().map_anon(kData, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(runtime.memory().map_anon(kReadOnly, 0x1000, PROT_READ));
    MockGles es2;
    es2.set_string(0x1F02, "OpenGL ES-CM 1.1");
    MockGles1 gl1;
    zb::HostGl host(runtime, es2);
    host.set_gl1_backend(&gl1);
    CHECK(host.gl1_backend() == &gl1);
    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);

    // Every pointerless entry point is dispatched with recognizable words, in guest word order.
    // That also proves positions 4 and 5 are read from the guest stack (glOrthof, glFrustumf).
    const std::array<std::uint32_t, 12> words{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const std::array<const char*, 3> no_arguments{"glLoadIdentity", "glPopMatrix", "glPushMatrix"};
    std::size_t seen = 0;
    for (const zb::Gl1HostCallInfo& info : zb::kGl1HostCalls) {
        if (info.has_pointer) continue;
        ++seen;
        set_words(runtime, thread, words);
        gl1.clear_calls();
        es2.set_error(0);
        CHECK(host.handle_host_call(info.index, thread));
        const MockGles1::Call& call = only_call(gl1);
        CHECK(call.name == info.name);
        const bool takes_no_argument =
            std::any_of(no_arguments.begin(), no_arguments.end(),
                        [&](const char* name) { return std::strcmp(name, info.name) == 0; });
        if (takes_no_argument) {
            CHECK(call.arguments.empty());
        } else {
            CHECK(!call.arguments.empty());
            for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                CHECK(call.arguments[i] == words[i]);
            }
        }
        // A GLES 1.1 call must not reach the GLES 2.0 seam. The one exception is the
        // one-time glGetString that records the context version in the runtime report.
        CHECK(es2.error() == 0);
        for (const MockGles::Call& es2_call : es2.calls()) {
            CHECK(es2_call.name == "glGetString");
        }
    }
    CHECK(seen == zb::kGl1HostCallCount - zb::kGl1PointerHostCallCount);

    // GLfloat keeps its 32-bit representation rather than being numerically cast.
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> float_words{
        std::bit_cast<std::uint32_t>(1.5f), std::bit_cast<std::uint32_t>(-2.25f),
        std::bit_cast<std::uint32_t>(0.0f), std::bit_cast<std::uint32_t>(8.5f)};
    set_words(runtime, thread, float_words);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glRotatef, thread));
    const MockGles1::Call& rotate = only_call(gl1);
    CHECK(rotate.arguments.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) CHECK(rotate.arguments[i] == float_words[i]);

    // GLubyte arguments arrive in a full word and are truncated to eight bits.
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> byte_words{0x1FFu, 0x100u, 0xFF80u, 0x1FFu};
    set_words(runtime, thread, byte_words);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glColor4ub, thread));
    const MockGles1::Call& color = only_call(gl1);
    CHECK(color.arguments.size() == 4);
    CHECK(color.arguments[0] == 0xFFu);
    CHECK(color.arguments[1] == 0x00u);
    CHECK(color.arguments[2] == 0x80u);
    CHECK(color.arguments[3] == 0xFFu);

    // GLfixed and GLclampx are signed 32-bit values and pass through unchanged.
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> fixed_words{0xFFFFFE0Cu, 0x00010000u};
    set_words(runtime, thread, fixed_words);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glLineWidthx, thread));
    CHECK(only_call(gl1).arguments.size() == 1);
    CHECK(only_call(gl1).arguments[0] == 0xFFFFFE0Cu);

    // Six arguments prove positions 4 and 5 come from the guest stack in word order.
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> six_words{0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    set_words(runtime, thread, six_words);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glOrthof, thread));
    const MockGles1::Call& ortho = only_call(gl1);
    CHECK(ortho.arguments.size() == 6);
    for (std::size_t i = 0; i < 6; ++i) CHECK(ortho.arguments[i] == 0x11u + i);

    // The 37 pointer-taking calls translate guest addresses in place. A 16-float matrix is the
    // largest fixed input; the guest address becomes base() + address with no copy.
    gl1.clear_calls();
    const std::uint32_t matrix_address = kData + 0x800;
    const std::array<std::uint32_t, 12> matrix_words{matrix_address};
    set_words(runtime, thread, matrix_words);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glLoadMatrixf, thread));
    const MockGles1::Call& matrix = only_call(gl1);
    CHECK(matrix.arguments.size() == 1);
    CHECK(matrix.arguments[0] ==
          reinterpret_cast<std::uintptr_t>(runtime.memory().base() + matrix_address));

    // 64 bytes ending past the mapped page must be rejected before the driver sees it.
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> past_end_words{kData + 0xFF8};
    set_words(runtime, thread, past_end_words);
    es2.set_error(0);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glLoadMatrixf, thread));
    CHECK(gl1.calls().empty());
    CHECK(es2.error() == zb::kGlInvalidValue);

    // A read-only page is fine as an input and rejected as an output.
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> readonly_words{kReadOnly};
    set_words(runtime, thread, readonly_words);
    es2.set_error(0);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glClipPlanef, thread));
    CHECK(only_call(gl1).name == "glClipPlanef");
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> readonly_out_words{0x3000 /*GL_CLIP_PLANE0*/, kReadOnly};
    set_words(runtime, thread, readonly_out_words);
    es2.set_error(0);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glGetClipPlanef, thread));
    CHECK(gl1.calls().empty());
    CHECK(es2.error() == zb::kGlInvalidValue);

    // A pname-sized input: GL_TEXTURE_ENV_COLOR is four elements, GL_TEXTURE_ENV_MODE one.
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> texenv4{0x2300 /*GL_TEXTURE_2D*/, 0x2201, kData + 0x400};
    set_words(runtime, thread, texenv4);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glTexEnvfv, thread));
    CHECK(only_call(gl1).name == "glTexEnvfv");
    CHECK(only_call(gl1).arguments[2] ==
          reinterpret_cast<std::uintptr_t>(runtime.memory().base() + kData + 0x400));
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> texenv1{0x2300, 0x2200 /*GL_TEXTURE_ENV_MODE*/,
                                                kData + 0xFFE};
    set_words(runtime, thread, texenv1);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glTexEnvfv, thread));
    CHECK(only_call(gl1).name == "glTexEnvfv");

    // The legacy fixed-function result sizes are in the shared pname table: an ES2 glGetFloatv
    // for GL_MODELVIEW_MATRIX now validates 64 bytes, so an address with only 8 bytes left is
    // rejected instead of being overwritten by the driver.
    es2.clear_calls();
    es2.set_error(0);
    const std::array<std::uint32_t, 12> matrix_get{0x0BA6, kData + 0xFF8};
    set_words(runtime, thread, matrix_get);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetFloatv, thread));
    CHECK(es2.calls().empty());
    CHECK(es2.error() == zb::kGlInvalidValue);
    es2.set_error(0);
    const std::array<std::uint32_t, 12> matrix_get_ok{0x0BA6, kData + 0x800};
    set_words(runtime, thread, matrix_get_ok);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glGetFloatv, thread));
    CHECK(es2.calls().size() == 1);
    CHECK(es2.calls()[0].name == "glGetFloatv");
    CHECK(es2.calls()[0].arguments[1] ==
          reinterpret_cast<std::uintptr_t>(runtime.memory().base() + kData + 0x800));

    // Client arrays: the guest address becomes a host alias, and glGetPointerv returns the
    // guest address rather than the host pointer the driver would report.
    gl1.clear_calls();
    const std::uint32_t vertex_address = kData + 0x40;
    const std::array<std::uint32_t, 12> array_words{3, 0x1406 /*GL_FLOAT*/, 12, vertex_address};
    set_words(runtime, thread, array_words);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glVertexPointer, thread));
    const MockGles1::Call& vertices = only_call(gl1);
    CHECK(vertices.arguments.size() == 4);
    CHECK(vertices.arguments[0] == 3);
    CHECK(vertices.arguments[1] == 0x1406);
    CHECK(vertices.arguments[2] == 12);
    CHECK(vertices.arguments[3] ==
          reinterpret_cast<std::uintptr_t>(runtime.memory().base() + vertex_address));
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> pointer_query{0x808E /*GL_VERTEX_ARRAY_POINTER*/,
                                                      kOutput};
    set_words(runtime, thread, pointer_query);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glGetPointerv, thread));
    CHECK(gl1.calls().empty());
    std::uint32_t stored = 0;
    std::memcpy(&stored, runtime.memory().base() + kOutput, sizeof stored);
    CHECK(stored == vertex_address);

    // Every one of the five client arrays is remembered independently.
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> texcoord_words{2, 0x1406, 8, kData + 0x100};
    set_words(runtime, thread, texcoord_words);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glTexCoordPointer, thread));
    const std::array<std::uint32_t, 12> point_size_words{0x1406, 0, kData + 0x200};
    set_words(runtime, thread, point_size_words);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glPointSizePointerOES, thread));
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> point_query{0x898C /*POINT_SIZE_ARRAY_POINTER_OES*/,
                                                    kOutput};
    set_words(runtime, thread, point_query);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glGetPointerv, thread));
    std::memcpy(&stored, runtime.memory().base() + kOutput, sizeof stored);
    CHECK(stored == kData + 0x200);

    // A null client array is legal and remembered as null.
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> null_array{4, 0x1401 /*GL_UNSIGNED_BYTE*/, 0, 0};
    set_words(runtime, thread, null_array);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glColorPointer, thread));
    CHECK(only_call(gl1).arguments[3] == 0);

    // Any other glGetPointerv pname is not a client-array pointer.
    gl1.clear_calls();
    es2.set_error(0);
    const std::array<std::uint32_t, 12> bad_pointer_query{0x1234, kOutput};
    set_words(runtime, thread, bad_pointer_query);
    CHECK(host.handle_host_call(zb::ZB_GL1_HC_glGetPointerv, thread));
    CHECK(gl1.calls().empty());
    CHECK(es2.error() == 0x0500 /*GL_INVALID_ENUM*/);

    // The first GLES 1.1 call recorded the context version in the report.
    CHECK(zb::runtime_report().text().find("gl-context: OpenGL ES-CM 1.1") != std::string::npos);

    // A range boundary: 579 is OpenSLES, not GL, and must not be claimed.
    CHECK(!host.handle_host_call(579, thread));

    // A HostGl without a GLES 1.1 backend rejects the call instead of returning zero silently.
    MockGles no_gl1_es2;
    zb::HostGl no_gl1(runtime, no_gl1_es2);
    set_words(runtime, thread, words);
    CHECK(no_gl1.handle_host_call(zb::ZB_GL1_HC_glPushMatrix, thread));
    CHECK(no_gl1_es2.error() == zb::kGlInvalidOperation);

    // The visibility diagnostics are written for OpenGL ES 2.0+ and query ES 2.0 entry points.
    // On device, glCheckFramebufferStatus on an OpenGL ES 1.1 context crashed the driver at
    // address 0 and took Punch Quest down, so a legacy context must skip them entirely: a GLES
    // 1.1 draw reaches no diagnostic query at all and the report says why the lines are absent.
    zb::runtime_report().clear();
    zb::enable_gl_diagnostics();
    es2.clear_calls();
    gl1.clear_calls();
    const std::array<std::uint32_t, 12> legacy_draw{0x0004 /*GL_TRIANGLES*/, 0, 3};
    set_words(runtime, thread, legacy_draw);
    CHECK(host.handle_host_call(zb::ZB_GL_HC_glDrawArrays, thread));
    CHECK(es2.calls().size() == 2);
    CHECK(es2.calls()[0].name == "glGetString");  // the one-time GL_VERSION decision
    CHECK(es2.calls()[1].name == "glDrawArrays");
    CHECK(gl1.calls().empty());
    const std::string legacy_report = zb::runtime_report().text();
    CHECK(legacy_report.find("gl-diagnostics: OpenGL ES-CM 1.1 profile skip") != std::string::npos);
    CHECK(legacy_report.find("gl-sample-draw-") == std::string::npos);
    CHECK(legacy_report.find("gl-draws:") == std::string::npos);
    CHECK(legacy_report.find("gl-first-draw-call:") == std::string::npos);

    return 0;
}
