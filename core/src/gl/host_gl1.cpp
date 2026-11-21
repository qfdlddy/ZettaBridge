// The GLES 1.1 fixed-function dispatcher: host-call indices 492-578.
//
// Every entry point here belongs to GLES 1.1 only; the 58 names it shares with GLES 2.0 are
// already served by the generated ES2/3 dispatcher at the indices the guest stubs use for them.
//
// Nothing is copied and nothing is emulated: the guest's arrays are aliases of guest memory
// (the runtime maps it at base() + address), and the fixed-function state lives in the real
// driver. Only glGetPointerv is answered here, because the driver would hand the guest a 64-bit
// host pointer it cannot store in a guest word.

#include <atomic>
#include <cstdint>

#include "zb/gles1_hostcalls.h"
#include "zb/gl1_backend.h"
#include "zb/gl_backend.h"
#include "zb/host_gl.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

constexpr GLenum kGlInvalidEnum = 0x0500;

// Client-array state. The guest addresses are remembered only so glGetPointerv can return
// what the guest passed: the driver holds the translated host pointer itself.
enum class ClientArray : unsigned {
    Vertex = 0,
    Normal,
    Color,
    TexCoord,
    PointSize,
    Count,
};

constexpr unsigned kClientArrayCount = static_cast<unsigned>(ClientArray::Count);

bool pointer_pname_of(GLenum pname, ClientArray& out) {
    switch (pname) {
    case 0x808E:  // GL_VERTEX_ARRAY_POINTER
        out = ClientArray::Vertex;
        return true;
    case 0x808F:  // GL_NORMAL_ARRAY_POINTER
        out = ClientArray::Normal;
        return true;
    case 0x8090:  // GL_COLOR_ARRAY_POINTER
        out = ClientArray::Color;
        return true;
    case 0x8092:  // GL_TEXTURE_COORD_ARRAY_POINTER
        out = ClientArray::TexCoord;
        return true;
    case 0x898C:  // GL_POINT_SIZE_ARRAY_POINTER_OES
        out = ClientArray::PointSize;
        return true;
    default:
        return false;
    }
}

struct ArrayShadow {
    std::uint64_t generation = ~std::uint64_t{0};
    std::uint32_t addresses[kClientArrayCount] = {};
};

// Array state is context state, so it follows the EGL generation the report already tracks.
ArrayShadow& array_shadow() {
    thread_local ArrayShadow shadow;
    const std::uint64_t generation = runtime_report().egl_current_generation();
    if (shadow.generation != generation) {
        shadow.generation = generation;
        for (std::uint32_t& address : shadow.addresses) address = 0;
    }
    return shadow;
}

// Result sizes for the pname-sized getters. A value the table does not name is a single element
// in both GLES 1.1 and GLES 2.0 (GL_LINE_WIDTH, GL_POINT_SIZE, the enable flags, ...).
std::uint64_t gl1_pname_count(HostGl& host, GLenum pname) {
    switch (pname) {
    case 0x0BA6:  // GL_MODELVIEW_MATRIX
    case 0x0BA7:  // GL_PROJECTION_MATRIX
    case 0x0BA8:  // GL_TEXTURE_MATRIX
        return 16;
    case 0x0B02:  // GL_CURRENT_NORMAL
        return 3;
    case 0x0B00:  // GL_CURRENT_COLOR
    case 0x0B03:  // GL_CURRENT_TEXTURE_COORDS
    case 0x8005:  // GL_BLEND_COLOR
    case 0x0C22:  // GL_COLOR_CLEAR_VALUE
    case 0x0C23:  // GL_COLOR_WRITEMASK
    case 0x0C10:  // GL_SCISSOR_BOX
    case 0x0BA2:  // GL_VIEWPORT
    case 0x0B53:  // GL_LIGHT_MODEL_AMBIENT
    case 0x0B66:  // GL_FOG_COLOR
        return 4;
    case 0x846D:  // GL_ALIASED_POINT_SIZE_RANGE
    case 0x846E:  // GL_ALIASED_LINE_WIDTH_RANGE
    case 0x0B12:  // GL_POINT_SIZE_RANGE
    case 0x0B22:  // GL_SMOOTH_LINE_WIDTH_RANGE
    case 0x0B70:  // GL_DEPTH_RANGE
    case 0x0D3A:  // GL_MAX_VIEWPORT_DIMS
        return 2;
    case 0x86A3: {  // GL_COMPRESSED_TEXTURE_FORMATS: the list length is driver-defined
        GLint count = 0;
        host.backend().glGetIntegerv(0x86A2, &count);  // GL_NUM_COMPRESSED_TEXTURE_FORMATS
        return count > 0 ? static_cast<std::uint64_t>(count) : 0;
    }
    default:
        return 1;
    }
}

std::uint64_t gl1_light_count(GLenum pname) {
    switch (pname) {
    case 0x1200:  // GL_AMBIENT
    case 0x1201:  // GL_DIFFUSE
    case 0x1202:  // GL_SPECULAR
    case 0x1203:  // GL_POSITION
        return 4;
    case 0x1204:  // GL_SPOT_DIRECTION
        return 3;
    default:
        return 1;
    }
}

std::uint64_t gl1_material_count(GLenum pname) {
    switch (pname) {
    case 0x1200:  // GL_AMBIENT
    case 0x1201:  // GL_DIFFUSE
    case 0x1202:  // GL_SPECULAR
    case 0x1600:  // GL_EMISSION
    case 0x1602:  // GL_AMBIENT_AND_DIFFUSE
        return 4;
    default:  // GL_SHININESS and anything unknown
        return 1;
    }
}

std::uint64_t gl1_texenv_count(GLenum pname) {
    return pname == 0x2201 ? 4 : 1;  // GL_TEXTURE_ENV_COLOR
}

std::uint64_t gl1_texparameter_count(GLenum pname) {
    return pname == 0x1004 ? 4 : 1;  // GL_TEXTURE_BORDER_COLOR
}

std::uint64_t gl1_fog_count(GLenum pname) {
    return pname == 0x0B66 ? 4 : 1;  // GL_FOG_COLOR
}

std::uint64_t gl1_light_model_count(GLenum pname) {
    return pname == 0x0B53 ? 4 : 1;  // GL_LIGHT_MODEL_AMBIENT
}

std::uint64_t gl1_point_parameter_count(GLenum pname) {
    return pname == 0x8129 ? 3 : 1;  // GL_DISTANCE_ATTENUATION
}

// The first GLES 1.1 call records the context version once, so a single device report proves
// whether the guest really runs in an ES 1.x context. A call made with no current context
// answers null and leaves the flag clear, so a later call records the real string.
void note_gl_context_once(HostGl& host) {
    static std::atomic<bool> recorded{false};
    if (recorded.load(std::memory_order_relaxed)) return;
    const char* version =
        reinterpret_cast<const char*>(host.backend().glGetString(0x1F02));  // GL_VERSION
    if (version == nullptr) return;
    if (recorded.exchange(true, std::memory_order_relaxed)) return;
    runtime_report().note_gl_detail("context", version, false);
}

}  // namespace

bool HostGl::dispatch_gl1(Call& call) {
    if (gl1_backend_ == nullptr) {
        // A missing backend is a wiring bug, not guest behaviour. Reject the first call
        // loudly and keep returning the same error to later calls without flooding the log.
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true, std::memory_order_relaxed)) {
            call.fail(kGlInvalidOperation, "no GLES 1.1 backend is installed");
        } else {
            backend_.set_error(kGlInvalidOperation);
        }
        return true;
    }
    note_gl_context_once(*this);

    switch (call.index()) {
    case ZB_GL1_HC_glAlphaFunc: {
        const GLenum func = call.scalar<GLenum>(0);
        const GLfloat ref = call.scalar<GLfloat>(1);
        if (!call.valid()) return true;
        gl1_backend_->glAlphaFunc(func, ref);
        return true;
    }
    case ZB_GL1_HC_glAlphaFuncx: {
        const GLenum func = call.scalar<GLenum>(0);
        const GLfixed ref = call.scalar<GLfixed>(1);
        if (!call.valid()) return true;
        gl1_backend_->glAlphaFuncx(func, ref);
        return true;
    }
    case ZB_GL1_HC_glClearColorx: {
        const GLfixed red = call.scalar<GLfixed>(0);
        const GLfixed green = call.scalar<GLfixed>(1);
        const GLfixed blue = call.scalar<GLfixed>(2);
        const GLfixed alpha = call.scalar<GLfixed>(3);
        if (!call.valid()) return true;
        gl1_backend_->glClearColorx(red, green, blue, alpha);
        return true;
    }
    case ZB_GL1_HC_glClearDepthx: {
        const GLfixed depth = call.scalar<GLfixed>(0);
        if (!call.valid()) return true;
        gl1_backend_->glClearDepthx(depth);
        return true;
    }
    case ZB_GL1_HC_glClientActiveTexture: {
        const GLenum texture = call.scalar<GLenum>(0);
        if (!call.valid()) return true;
        gl1_backend_->glClientActiveTexture(texture);
        return true;
    }
    case ZB_GL1_HC_glClipPlanef: {
        const GLenum p = call.scalar<GLenum>(0);
        const GLfloat* eqn = call.pointer<const GLfloat>(1, 4, kPageRead);
        if (!call.valid()) return true;
        if (eqn == nullptr) {
            call.fail(kGlInvalidValue, "glClipPlanef equation is null");
            return true;
        }
        gl1_backend_->glClipPlanef(p, eqn);
        return true;
    }
    case ZB_GL1_HC_glClipPlanex: {
        const GLenum plane = call.scalar<GLenum>(0);
        const GLfixed* equation = call.pointer<const GLfixed>(1, 4, kPageRead);
        if (!call.valid()) return true;
        if (equation == nullptr) {
            call.fail(kGlInvalidValue, "glClipPlanex equation is null");
            return true;
        }
        gl1_backend_->glClipPlanex(plane, equation);
        return true;
    }
    case ZB_GL1_HC_glColor4f: {
        const GLfloat red = call.scalar<GLfloat>(0);
        const GLfloat green = call.scalar<GLfloat>(1);
        const GLfloat blue = call.scalar<GLfloat>(2);
        const GLfloat alpha = call.scalar<GLfloat>(3);
        if (!call.valid()) return true;
        gl1_backend_->glColor4f(red, green, blue, alpha);
        return true;
    }
    case ZB_GL1_HC_glColor4ub: {
        const GLubyte red = call.scalar<GLubyte>(0);
        const GLubyte green = call.scalar<GLubyte>(1);
        const GLubyte blue = call.scalar<GLubyte>(2);
        const GLubyte alpha = call.scalar<GLubyte>(3);
        if (!call.valid()) return true;
        gl1_backend_->glColor4ub(red, green, blue, alpha);
        return true;
    }
    case ZB_GL1_HC_glColor4x: {
        const GLfixed red = call.scalar<GLfixed>(0);
        const GLfixed green = call.scalar<GLfixed>(1);
        const GLfixed blue = call.scalar<GLfixed>(2);
        const GLfixed alpha = call.scalar<GLfixed>(3);
        if (!call.valid()) return true;
        gl1_backend_->glColor4x(red, green, blue, alpha);
        return true;
    }
    case ZB_GL1_HC_glColorPointer: {
        const GLint size = call.scalar<GLint>(0);
        const GLenum type = call.scalar<GLenum>(1);
        const GLsizei stride = call.scalar<GLsizei>(2);
        const std::uint32_t address = call.arg(3);
        const void* pointer = call.pointer<const void>(3, 1, kPageRead);
        if (!call.valid()) return true;
        if (pointer == nullptr && address != 0) return true;
        array_shadow().addresses[static_cast<unsigned>(ClientArray::Color)] = address;
        gl1_backend_->glColorPointer(size, type, stride, pointer);
        return true;
    }
    case ZB_GL1_HC_glDepthRangex: {
        const GLfixed near_value = call.scalar<GLfixed>(0);
        const GLfixed far_value = call.scalar<GLfixed>(1);
        if (!call.valid()) return true;
        gl1_backend_->glDepthRangex(near_value, far_value);
        return true;
    }
    case ZB_GL1_HC_glDisableClientState: {
        const GLenum array = call.scalar<GLenum>(0);
        if (!call.valid()) return true;
        gl1_backend_->glDisableClientState(array);
        return true;
    }
    case ZB_GL1_HC_glEnableClientState: {
        const GLenum array = call.scalar<GLenum>(0);
        if (!call.valid()) return true;
        gl1_backend_->glEnableClientState(array);
        return true;
    }
    case ZB_GL1_HC_glFogf: {
        const GLenum pname = call.scalar<GLenum>(0);
        const GLfloat param = call.scalar<GLfloat>(1);
        if (!call.valid()) return true;
        gl1_backend_->glFogf(pname, param);
        return true;
    }
    case ZB_GL1_HC_glFogfv:
    case ZB_GL1_HC_glFogxv: {
        const GLenum pname = call.scalar<GLenum>(0);
        if (!call.valid()) return true;
        const std::uint64_t count = gl1_fog_count(pname);
        if (call.index() == ZB_GL1_HC_glFogfv) {
            const GLfloat* params = call.pointer<const GLfloat>(1, count, kPageRead);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glFogfv params is null");
                return true;
            }
            gl1_backend_->glFogfv(pname, params);
        } else {
            const GLfixed* params = call.pointer<const GLfixed>(1, count, kPageRead);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glFogxv params is null");
                return true;
            }
            gl1_backend_->glFogxv(pname, params);
        }
        return true;
    }
    case ZB_GL1_HC_glFogx: {
        const GLenum pname = call.scalar<GLenum>(0);
        const GLfixed param = call.scalar<GLfixed>(1);
        if (!call.valid()) return true;
        gl1_backend_->glFogx(pname, param);
        return true;
    }
    case ZB_GL1_HC_glFrustumf: {
        const GLfloat l = call.scalar<GLfloat>(0);
        const GLfloat r = call.scalar<GLfloat>(1);
        const GLfloat b = call.scalar<GLfloat>(2);
        const GLfloat t = call.scalar<GLfloat>(3);
        const GLfloat n = call.scalar<GLfloat>(4);
        const GLfloat f = call.scalar<GLfloat>(5);
        if (!call.valid()) return true;
        gl1_backend_->glFrustumf(l, r, b, t, n, f);
        return true;
    }
    case ZB_GL1_HC_glFrustumx: {
        const GLfixed l = call.scalar<GLfixed>(0);
        const GLfixed r = call.scalar<GLfixed>(1);
        const GLfixed b = call.scalar<GLfixed>(2);
        const GLfixed t = call.scalar<GLfixed>(3);
        const GLfixed n = call.scalar<GLfixed>(4);
        const GLfixed f = call.scalar<GLfixed>(5);
        if (!call.valid()) return true;
        gl1_backend_->glFrustumx(l, r, b, t, n, f);
        return true;
    }
    case ZB_GL1_HC_glGetClipPlanef: {
        const GLenum plane = call.scalar<GLenum>(0);
        GLfloat* equation = call.pointer<GLfloat>(1, 4, kPageRead | kPageWrite);
        if (!call.valid()) return true;
        if (equation == nullptr) {
            call.fail(kGlInvalidValue, "glGetClipPlanef output is null");
            return true;
        }
        gl1_backend_->glGetClipPlanef(plane, equation);
        return true;
    }
    case ZB_GL1_HC_glGetClipPlanex: {
        const GLenum plane = call.scalar<GLenum>(0);
        GLfixed* equation = call.pointer<GLfixed>(1, 4, kPageRead | kPageWrite);
        if (!call.valid()) return true;
        if (equation == nullptr) {
            call.fail(kGlInvalidValue, "glGetClipPlanex output is null");
            return true;
        }
        gl1_backend_->glGetClipPlanex(plane, equation);
        return true;
    }
    case ZB_GL1_HC_glGetFixedv: {
        const GLenum pname = call.scalar<GLenum>(0);
        if (!call.valid()) return true;
        const std::uint64_t count = gl1_pname_count(*this, pname);
        GLfixed* params = call.pointer<GLfixed>(1, count, kPageRead | kPageWrite);
        if (!call.valid()) return true;
        if (params == nullptr) {
            call.fail(kGlInvalidValue, "glGetFixedv output is null");
            return true;
        }
        gl1_backend_->glGetFixedv(pname, params);
        return true;
    }
    case ZB_GL1_HC_glGetLightfv:
    case ZB_GL1_HC_glGetLightxv: {
        const GLenum light = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        if (!call.valid()) return true;
        const std::uint64_t count = gl1_light_count(pname);
        if (call.index() == ZB_GL1_HC_glGetLightfv) {
            GLfloat* params = call.pointer<GLfloat>(2, count, kPageRead | kPageWrite);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glGetLightfv output is null");
                return true;
            }
            gl1_backend_->glGetLightfv(light, pname, params);
        } else {
            GLfixed* params = call.pointer<GLfixed>(2, count, kPageRead | kPageWrite);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glGetLightxv output is null");
                return true;
            }
            gl1_backend_->glGetLightxv(light, pname, params);
        }
        return true;
    }
    case ZB_GL1_HC_glGetMaterialfv:
    case ZB_GL1_HC_glGetMaterialxv: {
        const GLenum face = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        if (!call.valid()) return true;
        const std::uint64_t count = gl1_material_count(pname);
        if (call.index() == ZB_GL1_HC_glGetMaterialfv) {
            GLfloat* params = call.pointer<GLfloat>(2, count, kPageRead | kPageWrite);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glGetMaterialfv output is null");
                return true;
            }
            gl1_backend_->glGetMaterialfv(face, pname, params);
        } else {
            GLfixed* params = call.pointer<GLfixed>(2, count, kPageRead | kPageWrite);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glGetMaterialxv output is null");
                return true;
            }
            gl1_backend_->glGetMaterialxv(face, pname, params);
        }
        return true;
    }
    case ZB_GL1_HC_glGetPointerv: {
        const GLenum pname = call.scalar<GLenum>(0);
        ClientArray which;
        if (!pointer_pname_of(pname, which)) {
            call.fail(kGlInvalidEnum, "glGetPointerv pname is not a client-array pointer");
            return true;
        }
        GLuint* output = call.pointer<GLuint>(1, 1, kPageRead | kPageWrite);
        if (!call.valid()) return true;
        if (output == nullptr) {
            call.fail(kGlInvalidValue, "glGetPointerv output is null");
            return true;
        }
        // Answered here, never by the driver: the driver would store a 64-bit host pointer.
        *output = array_shadow().addresses[static_cast<unsigned>(which)];
        return true;
    }
    case ZB_GL1_HC_glGetTexEnvfv:
    case ZB_GL1_HC_glGetTexEnviv:
    case ZB_GL1_HC_glGetTexEnvxv: {
        const GLenum target = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        if (!call.valid()) return true;
        const std::uint64_t count = gl1_texenv_count(pname);
        if (call.index() == ZB_GL1_HC_glGetTexEnvfv) {
            GLfloat* params = call.pointer<GLfloat>(2, count, kPageRead | kPageWrite);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glGetTexEnvfv output is null");
                return true;
            }
            gl1_backend_->glGetTexEnvfv(target, pname, params);
        } else if (call.index() == ZB_GL1_HC_glGetTexEnviv) {
            GLint* params = call.pointer<GLint>(2, count, kPageRead | kPageWrite);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glGetTexEnviv output is null");
                return true;
            }
            gl1_backend_->glGetTexEnviv(target, pname, params);
        } else {
            GLfixed* params = call.pointer<GLfixed>(2, count, kPageRead | kPageWrite);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glGetTexEnvxv output is null");
                return true;
            }
            gl1_backend_->glGetTexEnvxv(target, pname, params);
        }
        return true;
    }
    case ZB_GL1_HC_glGetTexParameterxv: {
        const GLenum target = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        if (!call.valid()) return true;
        const std::uint64_t count = gl1_texparameter_count(pname);
        GLfixed* params = call.pointer<GLfixed>(2, count, kPageRead | kPageWrite);
        if (!call.valid()) return true;
        if (params == nullptr) {
            call.fail(kGlInvalidValue, "glGetTexParameterxv output is null");
            return true;
        }
        gl1_backend_->glGetTexParameterxv(target, pname, params);
        return true;
    }
    case ZB_GL1_HC_glLightModelf: {
        const GLenum pname = call.scalar<GLenum>(0);
        const GLfloat param = call.scalar<GLfloat>(1);
        if (!call.valid()) return true;
        gl1_backend_->glLightModelf(pname, param);
        return true;
    }
    case ZB_GL1_HC_glLightModelfv:
    case ZB_GL1_HC_glLightModelxv: {
        const GLenum pname = call.scalar<GLenum>(0);
        if (!call.valid()) return true;
        const std::uint64_t count = gl1_light_model_count(pname);
        if (call.index() == ZB_GL1_HC_glLightModelfv) {
            const GLfloat* params = call.pointer<const GLfloat>(1, count, kPageRead);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glLightModelfv params is null");
                return true;
            }
            gl1_backend_->glLightModelfv(pname, params);
        } else {
            const GLfixed* params = call.pointer<const GLfixed>(1, count, kPageRead);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glLightModelxv params is null");
                return true;
            }
            gl1_backend_->glLightModelxv(pname, params);
        }
        return true;
    }
    case ZB_GL1_HC_glLightModelx: {
        const GLenum pname = call.scalar<GLenum>(0);
        const GLfixed param = call.scalar<GLfixed>(1);
        if (!call.valid()) return true;
        gl1_backend_->glLightModelx(pname, param);
        return true;
    }
    case ZB_GL1_HC_glLightf: {
        const GLenum light = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        const GLfloat param = call.scalar<GLfloat>(2);
        if (!call.valid()) return true;
        gl1_backend_->glLightf(light, pname, param);
        return true;
    }
    case ZB_GL1_HC_glLightfv:
    case ZB_GL1_HC_glLightxv: {
        const GLenum light = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        if (!call.valid()) return true;
        const std::uint64_t count = gl1_light_count(pname);
        if (call.index() == ZB_GL1_HC_glLightfv) {
            const GLfloat* params = call.pointer<const GLfloat>(2, count, kPageRead);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glLightfv params is null");
                return true;
            }
            gl1_backend_->glLightfv(light, pname, params);
        } else {
            const GLfixed* params = call.pointer<const GLfixed>(2, count, kPageRead);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glLightxv params is null");
                return true;
            }
            gl1_backend_->glLightxv(light, pname, params);
        }
        return true;
    }
    case ZB_GL1_HC_glLightx: {
        const GLenum light = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        const GLfixed param = call.scalar<GLfixed>(2);
        if (!call.valid()) return true;
        gl1_backend_->glLightx(light, pname, param);
        return true;
    }
    case ZB_GL1_HC_glLineWidthx: {
        const GLfixed width = call.scalar<GLfixed>(0);
        if (!call.valid()) return true;
        gl1_backend_->glLineWidthx(width);
        return true;
    }
    case ZB_GL1_HC_glLoadIdentity: {
        if (!call.valid()) return true;
        gl1_backend_->glLoadIdentity();
        return true;
    }
    case ZB_GL1_HC_glLoadMatrixf: {
        const GLfloat* m = call.pointer<const GLfloat>(0, 16, kPageRead);
        if (!call.valid()) return true;
        if (m == nullptr) {
            call.fail(kGlInvalidValue, "glLoadMatrixf matrix is null");
            return true;
        }
        gl1_backend_->glLoadMatrixf(m);
        return true;
    }
    case ZB_GL1_HC_glLoadMatrixx: {
        const GLfixed* m = call.pointer<const GLfixed>(0, 16, kPageRead);
        if (!call.valid()) return true;
        if (m == nullptr) {
            call.fail(kGlInvalidValue, "glLoadMatrixx matrix is null");
            return true;
        }
        gl1_backend_->glLoadMatrixx(m);
        return true;
    }
    case ZB_GL1_HC_glLogicOp: {
        const GLenum opcode = call.scalar<GLenum>(0);
        if (!call.valid()) return true;
        gl1_backend_->glLogicOp(opcode);
        return true;
    }
    case ZB_GL1_HC_glMaterialf: {
        const GLenum face = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        const GLfloat param = call.scalar<GLfloat>(2);
        if (!call.valid()) return true;
        gl1_backend_->glMaterialf(face, pname, param);
        return true;
    }
    case ZB_GL1_HC_glMaterialfv:
    case ZB_GL1_HC_glMaterialxv: {
        const GLenum face = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        if (!call.valid()) return true;
        const std::uint64_t count = gl1_material_count(pname);
        if (call.index() == ZB_GL1_HC_glMaterialfv) {
            const GLfloat* params = call.pointer<const GLfloat>(2, count, kPageRead);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glMaterialfv params is null");
                return true;
            }
            gl1_backend_->glMaterialfv(face, pname, params);
        } else {
            const GLfixed* param = call.pointer<const GLfixed>(2, count, kPageRead);
            if (!call.valid()) return true;
            if (param == nullptr) {
                call.fail(kGlInvalidValue, "glMaterialxv params is null");
                return true;
            }
            gl1_backend_->glMaterialxv(face, pname, param);
        }
        return true;
    }
    case ZB_GL1_HC_glMaterialx: {
        const GLenum face = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        const GLfixed param = call.scalar<GLfixed>(2);
        if (!call.valid()) return true;
        gl1_backend_->glMaterialx(face, pname, param);
        return true;
    }
    case ZB_GL1_HC_glMatrixMode: {
        const GLenum mode = call.scalar<GLenum>(0);
        if (!call.valid()) return true;
        gl1_backend_->glMatrixMode(mode);
        return true;
    }
    case ZB_GL1_HC_glMultMatrixf: {
        const GLfloat* m = call.pointer<const GLfloat>(0, 16, kPageRead);
        if (!call.valid()) return true;
        if (m == nullptr) {
            call.fail(kGlInvalidValue, "glMultMatrixf matrix is null");
            return true;
        }
        gl1_backend_->glMultMatrixf(m);
        return true;
    }
    case ZB_GL1_HC_glMultMatrixx: {
        const GLfixed* m = call.pointer<const GLfixed>(0, 16, kPageRead);
        if (!call.valid()) return true;
        if (m == nullptr) {
            call.fail(kGlInvalidValue, "glMultMatrixx matrix is null");
            return true;
        }
        gl1_backend_->glMultMatrixx(m);
        return true;
    }
    case ZB_GL1_HC_glMultiTexCoord4f: {
        const GLenum target = call.scalar<GLenum>(0);
        const GLfloat s = call.scalar<GLfloat>(1);
        const GLfloat t = call.scalar<GLfloat>(2);
        const GLfloat r = call.scalar<GLfloat>(3);
        const GLfloat q = call.scalar<GLfloat>(4);
        if (!call.valid()) return true;
        gl1_backend_->glMultiTexCoord4f(target, s, t, r, q);
        return true;
    }
    case ZB_GL1_HC_glMultiTexCoord4x: {
        const GLenum texture = call.scalar<GLenum>(0);
        const GLfixed s = call.scalar<GLfixed>(1);
        const GLfixed t = call.scalar<GLfixed>(2);
        const GLfixed r = call.scalar<GLfixed>(3);
        const GLfixed q = call.scalar<GLfixed>(4);
        if (!call.valid()) return true;
        gl1_backend_->glMultiTexCoord4x(texture, s, t, r, q);
        return true;
    }
    case ZB_GL1_HC_glNormal3f: {
        const GLfloat nx = call.scalar<GLfloat>(0);
        const GLfloat ny = call.scalar<GLfloat>(1);
        const GLfloat nz = call.scalar<GLfloat>(2);
        if (!call.valid()) return true;
        gl1_backend_->glNormal3f(nx, ny, nz);
        return true;
    }
    case ZB_GL1_HC_glNormal3x: {
        const GLfixed nx = call.scalar<GLfixed>(0);
        const GLfixed ny = call.scalar<GLfixed>(1);
        const GLfixed nz = call.scalar<GLfixed>(2);
        if (!call.valid()) return true;
        gl1_backend_->glNormal3x(nx, ny, nz);
        return true;
    }
    case ZB_GL1_HC_glNormalPointer: {
        const GLenum type = call.scalar<GLenum>(0);
        const GLsizei stride = call.scalar<GLsizei>(1);
        const std::uint32_t address = call.arg(2);
        const void* pointer = call.pointer<const void>(2, 1, kPageRead);
        if (!call.valid()) return true;
        if (pointer == nullptr && address != 0) return true;
        array_shadow().addresses[static_cast<unsigned>(ClientArray::Normal)] = address;
        gl1_backend_->glNormalPointer(type, stride, pointer);
        return true;
    }
    case ZB_GL1_HC_glOrthof: {
        const GLfloat l = call.scalar<GLfloat>(0);
        const GLfloat r = call.scalar<GLfloat>(1);
        const GLfloat b = call.scalar<GLfloat>(2);
        const GLfloat t = call.scalar<GLfloat>(3);
        const GLfloat n = call.scalar<GLfloat>(4);
        const GLfloat f = call.scalar<GLfloat>(5);
        if (!call.valid()) return true;
        gl1_backend_->glOrthof(l, r, b, t, n, f);
        return true;
    }
    case ZB_GL1_HC_glOrthox: {
        const GLfixed l = call.scalar<GLfixed>(0);
        const GLfixed r = call.scalar<GLfixed>(1);
        const GLfixed b = call.scalar<GLfixed>(2);
        const GLfixed t = call.scalar<GLfixed>(3);
        const GLfixed n = call.scalar<GLfixed>(4);
        const GLfixed f = call.scalar<GLfixed>(5);
        if (!call.valid()) return true;
        gl1_backend_->glOrthox(l, r, b, t, n, f);
        return true;
    }
    case ZB_GL1_HC_glPointParameterf: {
        const GLenum pname = call.scalar<GLenum>(0);
        const GLfloat param = call.scalar<GLfloat>(1);
        if (!call.valid()) return true;
        gl1_backend_->glPointParameterf(pname, param);
        return true;
    }
    case ZB_GL1_HC_glPointParameterfv:
    case ZB_GL1_HC_glPointParameterxv: {
        const GLenum pname = call.scalar<GLenum>(0);
        if (!call.valid()) return true;
        const std::uint64_t count = gl1_point_parameter_count(pname);
        if (call.index() == ZB_GL1_HC_glPointParameterfv) {
            const GLfloat* params = call.pointer<const GLfloat>(1, count, kPageRead);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glPointParameterfv params is null");
                return true;
            }
            gl1_backend_->glPointParameterfv(pname, params);
        } else {
            const GLfixed* params = call.pointer<const GLfixed>(1, count, kPageRead);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glPointParameterxv params is null");
                return true;
            }
            gl1_backend_->glPointParameterxv(pname, params);
        }
        return true;
    }
    case ZB_GL1_HC_glPointParameterx: {
        const GLenum pname = call.scalar<GLenum>(0);
        const GLfixed param = call.scalar<GLfixed>(1);
        if (!call.valid()) return true;
        gl1_backend_->glPointParameterx(pname, param);
        return true;
    }
    case ZB_GL1_HC_glPointSize: {
        const GLfloat size = call.scalar<GLfloat>(0);
        if (!call.valid()) return true;
        gl1_backend_->glPointSize(size);
        return true;
    }
    case ZB_GL1_HC_glPointSizePointerOES: {
        const GLenum type = call.scalar<GLenum>(0);
        const GLsizei stride = call.scalar<GLsizei>(1);
        const std::uint32_t address = call.arg(2);
        const void* pointer = call.pointer<const void>(2, 1, kPageRead);
        if (!call.valid()) return true;
        if (pointer == nullptr && address != 0) return true;
        array_shadow().addresses[static_cast<unsigned>(ClientArray::PointSize)] = address;
        gl1_backend_->glPointSizePointerOES(type, stride, pointer);
        return true;
    }
    case ZB_GL1_HC_glPointSizex: {
        const GLfixed size = call.scalar<GLfixed>(0);
        if (!call.valid()) return true;
        gl1_backend_->glPointSizex(size);
        return true;
    }
    case ZB_GL1_HC_glPolygonOffsetx: {
        const GLfixed factor = call.scalar<GLfixed>(0);
        const GLfixed units = call.scalar<GLfixed>(1);
        if (!call.valid()) return true;
        gl1_backend_->glPolygonOffsetx(factor, units);
        return true;
    }
    case ZB_GL1_HC_glPopMatrix: {
        if (!call.valid()) return true;
        gl1_backend_->glPopMatrix();
        return true;
    }
    case ZB_GL1_HC_glPushMatrix: {
        if (!call.valid()) return true;
        gl1_backend_->glPushMatrix();
        return true;
    }
    case ZB_GL1_HC_glRotatef: {
        const GLfloat angle = call.scalar<GLfloat>(0);
        const GLfloat x = call.scalar<GLfloat>(1);
        const GLfloat y = call.scalar<GLfloat>(2);
        const GLfloat z = call.scalar<GLfloat>(3);
        if (!call.valid()) return true;
        gl1_backend_->glRotatef(angle, x, y, z);
        return true;
    }
    case ZB_GL1_HC_glRotatex: {
        const GLfixed angle = call.scalar<GLfixed>(0);
        const GLfixed x = call.scalar<GLfixed>(1);
        const GLfixed y = call.scalar<GLfixed>(2);
        const GLfixed z = call.scalar<GLfixed>(3);
        if (!call.valid()) return true;
        gl1_backend_->glRotatex(angle, x, y, z);
        return true;
    }
    case ZB_GL1_HC_glSampleCoveragex: {
        const GLclampx value = call.scalar<GLclampx>(0);
        const GLboolean invert = call.scalar<GLboolean>(1);
        if (!call.valid()) return true;
        gl1_backend_->glSampleCoveragex(value, invert);
        return true;
    }
    case ZB_GL1_HC_glScalef: {
        const GLfloat x = call.scalar<GLfloat>(0);
        const GLfloat y = call.scalar<GLfloat>(1);
        const GLfloat z = call.scalar<GLfloat>(2);
        if (!call.valid()) return true;
        gl1_backend_->glScalef(x, y, z);
        return true;
    }
    case ZB_GL1_HC_glScalex: {
        const GLfixed x = call.scalar<GLfixed>(0);
        const GLfixed y = call.scalar<GLfixed>(1);
        const GLfixed z = call.scalar<GLfixed>(2);
        if (!call.valid()) return true;
        gl1_backend_->glScalex(x, y, z);
        return true;
    }
    case ZB_GL1_HC_glShadeModel: {
        const GLenum mode = call.scalar<GLenum>(0);
        if (!call.valid()) return true;
        gl1_backend_->glShadeModel(mode);
        return true;
    }
    case ZB_GL1_HC_glTexCoordPointer: {
        const GLint size = call.scalar<GLint>(0);
        const GLenum type = call.scalar<GLenum>(1);
        const GLsizei stride = call.scalar<GLsizei>(2);
        const std::uint32_t address = call.arg(3);
        const void* pointer = call.pointer<const void>(3, 1, kPageRead);
        if (!call.valid()) return true;
        if (pointer == nullptr && address != 0) return true;
        array_shadow().addresses[static_cast<unsigned>(ClientArray::TexCoord)] = address;
        gl1_backend_->glTexCoordPointer(size, type, stride, pointer);
        return true;
    }
    case ZB_GL1_HC_glTexEnvf: {
        const GLenum target = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        const GLfloat param = call.scalar<GLfloat>(2);
        if (!call.valid()) return true;
        gl1_backend_->glTexEnvf(target, pname, param);
        return true;
    }
    case ZB_GL1_HC_glTexEnvfv:
    case ZB_GL1_HC_glTexEnviv:
    case ZB_GL1_HC_glTexEnvxv: {
        const GLenum target = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        if (!call.valid()) return true;
        const std::uint64_t count = gl1_texenv_count(pname);
        if (call.index() == ZB_GL1_HC_glTexEnvfv) {
            const GLfloat* params = call.pointer<const GLfloat>(2, count, kPageRead);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glTexEnvfv params is null");
                return true;
            }
            gl1_backend_->glTexEnvfv(target, pname, params);
        } else if (call.index() == ZB_GL1_HC_glTexEnviv) {
            const GLint* params = call.pointer<const GLint>(2, count, kPageRead);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glTexEnviv params is null");
                return true;
            }
            gl1_backend_->glTexEnviv(target, pname, params);
        } else {
            const GLfixed* params = call.pointer<const GLfixed>(2, count, kPageRead);
            if (!call.valid()) return true;
            if (params == nullptr) {
                call.fail(kGlInvalidValue, "glTexEnvxv params is null");
                return true;
            }
            gl1_backend_->glTexEnvxv(target, pname, params);
        }
        return true;
    }
    case ZB_GL1_HC_glTexEnvi: {
        const GLenum target = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        const GLint param = call.scalar<GLint>(2);
        if (!call.valid()) return true;
        gl1_backend_->glTexEnvi(target, pname, param);
        return true;
    }
    case ZB_GL1_HC_glTexEnvx: {
        const GLenum target = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        const GLfixed param = call.scalar<GLfixed>(2);
        if (!call.valid()) return true;
        gl1_backend_->glTexEnvx(target, pname, param);
        return true;
    }
    case ZB_GL1_HC_glTexParameterx: {
        const GLenum target = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        const GLfixed param = call.scalar<GLfixed>(2);
        if (!call.valid()) return true;
        gl1_backend_->glTexParameterx(target, pname, param);
        return true;
    }
    case ZB_GL1_HC_glTexParameterxv: {
        const GLenum target = call.scalar<GLenum>(0);
        const GLenum pname = call.scalar<GLenum>(1);
        if (!call.valid()) return true;
        const std::uint64_t count = gl1_texparameter_count(pname);
        const GLfixed* params = call.pointer<const GLfixed>(2, count, kPageRead);
        if (!call.valid()) return true;
        if (params == nullptr) {
            call.fail(kGlInvalidValue, "glTexParameterxv params is null");
            return true;
        }
        gl1_backend_->glTexParameterxv(target, pname, params);
        return true;
    }
    case ZB_GL1_HC_glTranslatef: {
        const GLfloat x = call.scalar<GLfloat>(0);
        const GLfloat y = call.scalar<GLfloat>(1);
        const GLfloat z = call.scalar<GLfloat>(2);
        if (!call.valid()) return true;
        gl1_backend_->glTranslatef(x, y, z);
        return true;
    }
    case ZB_GL1_HC_glTranslatex: {
        const GLfixed x = call.scalar<GLfixed>(0);
        const GLfixed y = call.scalar<GLfixed>(1);
        const GLfixed z = call.scalar<GLfixed>(2);
        if (!call.valid()) return true;
        gl1_backend_->glTranslatex(x, y, z);
        return true;
    }
    case ZB_GL1_HC_glVertexPointer: {
        const GLint size = call.scalar<GLint>(0);
        const GLenum type = call.scalar<GLenum>(1);
        const GLsizei stride = call.scalar<GLsizei>(2);
        const std::uint32_t address = call.arg(3);
        const void* pointer = call.pointer<const void>(3, 1, kPageRead);
        if (!call.valid()) return true;
        if (pointer == nullptr && address != 0) return true;
        array_shadow().addresses[static_cast<unsigned>(ClientArray::Vertex)] = address;
        gl1_backend_->glVertexPointer(size, type, stride, pointer);
        return true;
    }
    default:
        // Every GLES 1.1 index is handled above; a new name must be added here rather than
        // failing silently.
        call.fail(kGlInvalidOperation, "no GLES 1.1 handler for this host call");
        return true;
    }
}

}  // namespace zb
