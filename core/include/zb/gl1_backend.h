#pragma once

#include <cstdint>
#include <initializer_list>

#include "zb/gl_backend.h"

namespace zb {

// GLES 1.1 fixed-point scalars. Both are plain 32-bit integers on the wire and in the ABI.
using GLfixed = std::int32_t;
using GLclampx = std::int32_t;

// The portable GLES 1.1 seam: one virtual per entry point that GLES 2.0 does not share.
//
// The default body records the call through invoke(), which is what a host mock needs, and the
// Android backend overrides every one of them with a direct call to the real driver. There is
// no set_error: a rejection happens in HostGl before the backend is reached, and it is queued
// through the GLES 2.0 backend so the guest's glGetError() serves it in the normal order.
//
// Pointer parameters are already bounds-checked guest-to-host addresses when they arrive here,
// exactly as they are on GlBackend. All 87 return void.
class Gl1Backend {
public:
    virtual ~Gl1Backend() = default;

    virtual void glAlphaFunc(GLenum func, GLfloat ref) {
        (void)invoke("glAlphaFunc", {gl_backend_word(func), gl_backend_word(ref)});
    }
    virtual void glAlphaFuncx(GLenum func, GLfixed ref) {
        (void)invoke("glAlphaFuncx", {gl_backend_word(func), gl_backend_word(ref)});
    }
    virtual void glClearColorx(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha) {
        (void)invoke("glClearColorx", {gl_backend_word(red), gl_backend_word(green),
                                       gl_backend_word(blue), gl_backend_word(alpha)});
    }
    virtual void glClearDepthx(GLfixed depth) {
        (void)invoke("glClearDepthx", {gl_backend_word(depth)});
    }
    virtual void glClientActiveTexture(GLenum texture) {
        (void)invoke("glClientActiveTexture", {gl_backend_word(texture)});
    }
    virtual void glClipPlanef(GLenum p, const GLfloat *eqn) {
        (void)invoke("glClipPlanef", {gl_backend_word(p), gl_backend_word(eqn)});
    }
    virtual void glClipPlanex(GLenum p, const GLfixed *eqn) {
        (void)invoke("glClipPlanex", {gl_backend_word(p), gl_backend_word(eqn)});
    }
    virtual void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
        (void)invoke("glColor4f", {gl_backend_word(red), gl_backend_word(green),
                                   gl_backend_word(blue), gl_backend_word(alpha)});
    }
    virtual void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha) {
        (void)invoke("glColor4ub", {gl_backend_word(red), gl_backend_word(green),
                                    gl_backend_word(blue), gl_backend_word(alpha)});
    }
    virtual void glColor4x(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha) {
        (void)invoke("glColor4x", {gl_backend_word(red), gl_backend_word(green),
                                   gl_backend_word(blue), gl_backend_word(alpha)});
    }
    virtual void glColorPointer(GLint size, GLenum type, GLsizei stride, const void *pointer) {
        (void)invoke("glColorPointer", {gl_backend_word(size), gl_backend_word(type),
                                        gl_backend_word(stride), gl_backend_word(pointer)});
    }
    virtual void glDepthRangex(GLfixed n, GLfixed f) {
        (void)invoke("glDepthRangex", {gl_backend_word(n), gl_backend_word(f)});
    }
    virtual void glDisableClientState(GLenum array) {
        (void)invoke("glDisableClientState", {gl_backend_word(array)});
    }
    virtual void glEnableClientState(GLenum array) {
        (void)invoke("glEnableClientState", {gl_backend_word(array)});
    }
    virtual void glFogf(GLenum pname, GLfloat param) {
        (void)invoke("glFogf", {gl_backend_word(pname), gl_backend_word(param)});
    }
    virtual void glFogfv(GLenum pname, const GLfloat *params) {
        (void)invoke("glFogfv", {gl_backend_word(pname), gl_backend_word(params)});
    }
    virtual void glFogx(GLenum pname, GLfixed param) {
        (void)invoke("glFogx", {gl_backend_word(pname), gl_backend_word(param)});
    }
    virtual void glFogxv(GLenum pname, const GLfixed *params) {
        (void)invoke("glFogxv", {gl_backend_word(pname), gl_backend_word(params)});
    }
    virtual void glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
        (void)invoke("glFrustumf", {gl_backend_word(l), gl_backend_word(r), gl_backend_word(b),
                                    gl_backend_word(t), gl_backend_word(n), gl_backend_word(f)});
    }
    virtual void glFrustumx(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f) {
        (void)invoke("glFrustumx", {gl_backend_word(l), gl_backend_word(r), gl_backend_word(b),
                                    gl_backend_word(t), gl_backend_word(n), gl_backend_word(f)});
    }
    virtual void glGetClipPlanef(GLenum plane, GLfloat *equation) {
        (void)invoke("glGetClipPlanef", {gl_backend_word(plane), gl_backend_word(equation)});
    }
    virtual void glGetClipPlanex(GLenum plane, GLfixed *equation) {
        (void)invoke("glGetClipPlanex", {gl_backend_word(plane), gl_backend_word(equation)});
    }
    virtual void glGetFixedv(GLenum pname, GLfixed *params) {
        (void)invoke("glGetFixedv", {gl_backend_word(pname), gl_backend_word(params)});
    }
    virtual void glGetLightfv(GLenum light, GLenum pname, GLfloat *params) {
        (void)invoke("glGetLightfv", {gl_backend_word(light), gl_backend_word(pname),
                                      gl_backend_word(params)});
    }
    virtual void glGetLightxv(GLenum light, GLenum pname, GLfixed *params) {
        (void)invoke("glGetLightxv", {gl_backend_word(light), gl_backend_word(pname),
                                      gl_backend_word(params)});
    }
    virtual void glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params) {
        (void)invoke("glGetMaterialfv", {gl_backend_word(face), gl_backend_word(pname),
                                         gl_backend_word(params)});
    }
    virtual void glGetMaterialxv(GLenum face, GLenum pname, GLfixed *params) {
        (void)invoke("glGetMaterialxv", {gl_backend_word(face), gl_backend_word(pname),
                                         gl_backend_word(params)});
    }
    virtual void glGetPointerv(GLenum pname, void **params) {
        (void)invoke("glGetPointerv", {gl_backend_word(pname), gl_backend_word(params)});
    }
    virtual void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params) {
        (void)invoke("glGetTexEnvfv", {gl_backend_word(target), gl_backend_word(pname),
                                       gl_backend_word(params)});
    }
    virtual void glGetTexEnviv(GLenum target, GLenum pname, GLint *params) {
        (void)invoke("glGetTexEnviv", {gl_backend_word(target), gl_backend_word(pname),
                                       gl_backend_word(params)});
    }
    virtual void glGetTexEnvxv(GLenum target, GLenum pname, GLfixed *params) {
        (void)invoke("glGetTexEnvxv", {gl_backend_word(target), gl_backend_word(pname),
                                       gl_backend_word(params)});
    }
    virtual void glGetTexParameterxv(GLenum target, GLenum pname, GLfixed *params) {
        (void)invoke("glGetTexParameterxv", {gl_backend_word(target), gl_backend_word(pname),
                                             gl_backend_word(params)});
    }
    virtual void glLightModelf(GLenum pname, GLfloat param) {
        (void)invoke("glLightModelf", {gl_backend_word(pname), gl_backend_word(param)});
    }
    virtual void glLightModelfv(GLenum pname, const GLfloat *params) {
        (void)invoke("glLightModelfv", {gl_backend_word(pname), gl_backend_word(params)});
    }
    virtual void glLightModelx(GLenum pname, GLfixed param) {
        (void)invoke("glLightModelx", {gl_backend_word(pname), gl_backend_word(param)});
    }
    virtual void glLightModelxv(GLenum pname, const GLfixed *params) {
        (void)invoke("glLightModelxv", {gl_backend_word(pname), gl_backend_word(params)});
    }
    virtual void glLightf(GLenum light, GLenum pname, GLfloat param) {
        (void)invoke("glLightf", {gl_backend_word(light), gl_backend_word(pname),
                                  gl_backend_word(param)});
    }
    virtual void glLightfv(GLenum light, GLenum pname, const GLfloat *params) {
        (void)invoke("glLightfv", {gl_backend_word(light), gl_backend_word(pname),
                                   gl_backend_word(params)});
    }
    virtual void glLightx(GLenum light, GLenum pname, GLfixed param) {
        (void)invoke("glLightx", {gl_backend_word(light), gl_backend_word(pname),
                                  gl_backend_word(param)});
    }
    virtual void glLightxv(GLenum light, GLenum pname, const GLfixed *params) {
        (void)invoke("glLightxv", {gl_backend_word(light), gl_backend_word(pname),
                                   gl_backend_word(params)});
    }
    virtual void glLineWidthx(GLfixed width) {
        (void)invoke("glLineWidthx", {gl_backend_word(width)});
    }
    virtual void glLoadIdentity(void) {
        (void)invoke("glLoadIdentity", {});
    }
    virtual void glLoadMatrixf(const GLfloat *m) {
        (void)invoke("glLoadMatrixf", {gl_backend_word(m)});
    }
    virtual void glLoadMatrixx(const GLfixed *m) {
        (void)invoke("glLoadMatrixx", {gl_backend_word(m)});
    }
    virtual void glLogicOp(GLenum opcode) {
        (void)invoke("glLogicOp", {gl_backend_word(opcode)});
    }
    virtual void glMaterialf(GLenum face, GLenum pname, GLfloat param) {
        (void)invoke("glMaterialf", {gl_backend_word(face), gl_backend_word(pname),
                                     gl_backend_word(param)});
    }
    virtual void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) {
        (void)invoke("glMaterialfv", {gl_backend_word(face), gl_backend_word(pname),
                                      gl_backend_word(params)});
    }
    virtual void glMaterialx(GLenum face, GLenum pname, GLfixed param) {
        (void)invoke("glMaterialx", {gl_backend_word(face), gl_backend_word(pname),
                                     gl_backend_word(param)});
    }
    virtual void glMaterialxv(GLenum face, GLenum pname, const GLfixed *param) {
        (void)invoke("glMaterialxv", {gl_backend_word(face), gl_backend_word(pname),
                                      gl_backend_word(param)});
    }
    virtual void glMatrixMode(GLenum mode) {
        (void)invoke("glMatrixMode", {gl_backend_word(mode)});
    }
    virtual void glMultMatrixf(const GLfloat *m) {
        (void)invoke("glMultMatrixf", {gl_backend_word(m)});
    }
    virtual void glMultMatrixx(const GLfixed *m) {
        (void)invoke("glMultMatrixx", {gl_backend_word(m)});
    }
    virtual void glMultiTexCoord4f(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) {
        (void)invoke("glMultiTexCoord4f", {gl_backend_word(target), gl_backend_word(s),
                                           gl_backend_word(t), gl_backend_word(r),
                                           gl_backend_word(q)});
    }
    virtual void glMultiTexCoord4x(GLenum texture, GLfixed s, GLfixed t, GLfixed r, GLfixed q) {
        (void)invoke("glMultiTexCoord4x", {gl_backend_word(texture), gl_backend_word(s),
                                           gl_backend_word(t), gl_backend_word(r),
                                           gl_backend_word(q)});
    }
    virtual void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) {
        (void)invoke("glNormal3f", {gl_backend_word(nx), gl_backend_word(ny),
                                    gl_backend_word(nz)});
    }
    virtual void glNormal3x(GLfixed nx, GLfixed ny, GLfixed nz) {
        (void)invoke("glNormal3x", {gl_backend_word(nx), gl_backend_word(ny),
                                    gl_backend_word(nz)});
    }
    virtual void glNormalPointer(GLenum type, GLsizei stride, const void *pointer) {
        (void)invoke("glNormalPointer", {gl_backend_word(type), gl_backend_word(stride),
                                         gl_backend_word(pointer)});
    }
    virtual void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
        (void)invoke("glOrthof", {gl_backend_word(l), gl_backend_word(r), gl_backend_word(b),
                                  gl_backend_word(t), gl_backend_word(n), gl_backend_word(f)});
    }
    virtual void glOrthox(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f) {
        (void)invoke("glOrthox", {gl_backend_word(l), gl_backend_word(r), gl_backend_word(b),
                                  gl_backend_word(t), gl_backend_word(n), gl_backend_word(f)});
    }
    virtual void glPointParameterf(GLenum pname, GLfloat param) {
        (void)invoke("glPointParameterf", {gl_backend_word(pname), gl_backend_word(param)});
    }
    virtual void glPointParameterfv(GLenum pname, const GLfloat *params) {
        (void)invoke("glPointParameterfv", {gl_backend_word(pname), gl_backend_word(params)});
    }
    virtual void glPointParameterx(GLenum pname, GLfixed param) {
        (void)invoke("glPointParameterx", {gl_backend_word(pname), gl_backend_word(param)});
    }
    virtual void glPointParameterxv(GLenum pname, const GLfixed *params) {
        (void)invoke("glPointParameterxv", {gl_backend_word(pname), gl_backend_word(params)});
    }
    virtual void glPointSize(GLfloat size) {
        (void)invoke("glPointSize", {gl_backend_word(size)});
    }
    virtual void glPointSizePointerOES(GLenum type, GLsizei stride, const void *pointer) {
        (void)invoke("glPointSizePointerOES", {gl_backend_word(type), gl_backend_word(stride),
                                               gl_backend_word(pointer)});
    }
    virtual void glPointSizex(GLfixed size) {
        (void)invoke("glPointSizex", {gl_backend_word(size)});
    }
    virtual void glPolygonOffsetx(GLfixed factor, GLfixed units) {
        (void)invoke("glPolygonOffsetx", {gl_backend_word(factor), gl_backend_word(units)});
    }
    virtual void glPopMatrix(void) {
        (void)invoke("glPopMatrix", {});
    }
    virtual void glPushMatrix(void) {
        (void)invoke("glPushMatrix", {});
    }
    virtual void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
        (void)invoke("glRotatef", {gl_backend_word(angle), gl_backend_word(x),
                                   gl_backend_word(y), gl_backend_word(z)});
    }
    virtual void glRotatex(GLfixed angle, GLfixed x, GLfixed y, GLfixed z) {
        (void)invoke("glRotatex", {gl_backend_word(angle), gl_backend_word(x),
                                   gl_backend_word(y), gl_backend_word(z)});
    }
    virtual void glSampleCoveragex(GLclampx value, GLboolean invert) {
        (void)invoke("glSampleCoveragex", {gl_backend_word(value), gl_backend_word(invert)});
    }
    virtual void glScalef(GLfloat x, GLfloat y, GLfloat z) {
        (void)invoke("glScalef", {gl_backend_word(x), gl_backend_word(y), gl_backend_word(z)});
    }
    virtual void glScalex(GLfixed x, GLfixed y, GLfixed z) {
        (void)invoke("glScalex", {gl_backend_word(x), gl_backend_word(y), gl_backend_word(z)});
    }
    virtual void glShadeModel(GLenum mode) {
        (void)invoke("glShadeModel", {gl_backend_word(mode)});
    }
    virtual void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *pointer) {
        (void)invoke("glTexCoordPointer", {gl_backend_word(size), gl_backend_word(type),
                                           gl_backend_word(stride), gl_backend_word(pointer)});
    }
    virtual void glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
        (void)invoke("glTexEnvf", {gl_backend_word(target), gl_backend_word(pname),
                                   gl_backend_word(param)});
    }
    virtual void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params) {
        (void)invoke("glTexEnvfv", {gl_backend_word(target), gl_backend_word(pname),
                                    gl_backend_word(params)});
    }
    virtual void glTexEnvi(GLenum target, GLenum pname, GLint param) {
        (void)invoke("glTexEnvi", {gl_backend_word(target), gl_backend_word(pname),
                                   gl_backend_word(param)});
    }
    virtual void glTexEnviv(GLenum target, GLenum pname, const GLint *params) {
        (void)invoke("glTexEnviv", {gl_backend_word(target), gl_backend_word(pname),
                                    gl_backend_word(params)});
    }
    virtual void glTexEnvx(GLenum target, GLenum pname, GLfixed param) {
        (void)invoke("glTexEnvx", {gl_backend_word(target), gl_backend_word(pname),
                                   gl_backend_word(param)});
    }
    virtual void glTexEnvxv(GLenum target, GLenum pname, const GLfixed *params) {
        (void)invoke("glTexEnvxv", {gl_backend_word(target), gl_backend_word(pname),
                                    gl_backend_word(params)});
    }
    virtual void glTexParameterx(GLenum target, GLenum pname, GLfixed param) {
        (void)invoke("glTexParameterx", {gl_backend_word(target), gl_backend_word(pname),
                                         gl_backend_word(param)});
    }
    virtual void glTexParameterxv(GLenum target, GLenum pname, const GLfixed *params) {
        (void)invoke("glTexParameterxv", {gl_backend_word(target), gl_backend_word(pname),
                                          gl_backend_word(params)});
    }
    virtual void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
        (void)invoke("glTranslatef", {gl_backend_word(x), gl_backend_word(y),
                                      gl_backend_word(z)});
    }
    virtual void glTranslatex(GLfixed x, GLfixed y, GLfixed z) {
        (void)invoke("glTranslatex", {gl_backend_word(x), gl_backend_word(y),
                                      gl_backend_word(z)});
    }
    virtual void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *pointer) {
        (void)invoke("glVertexPointer", {gl_backend_word(size), gl_backend_word(type),
                                         gl_backend_word(stride), gl_backend_word(pointer)});
    }

protected:
    // The single hook a mock implements. Only default bodies above call it; the Android
    // backend overrides every entry point directly.
    virtual std::uint64_t invoke(const char* name,
                                 std::initializer_list<std::uint64_t> arguments) = 0;
};

}  // namespace zb
