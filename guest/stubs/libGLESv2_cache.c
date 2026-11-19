/* Guest-side per-thread GL state cache and void-command batch buffer compiled into libGLESv2.so
 * (-nostdlib). Redundant state calls stay completely inside translated arm32 guest code with zero
 * SVC exit; non-redundant void state/attrib/uniform calls append to a per-thread batch header that
 * HostGl drains automatically before any unbatched GL/EGL call or when the buffer fills. */

#include <stdint.h>

#include "zb/library_protocol.h"

typedef uint8_t GLboolean;
typedef uint32_t GLenum;
typedef uint32_t GLuint;
typedef uint32_t GLbitfield;
typedef int32_t GLint;
typedef int32_t GLsizei;
typedef int32_t GLintptr;
typedef int32_t GLsizeiptr;
typedef float GLfloat;

#define ZB_GL_TLS_SLOTS 32u
#define ZB_GL_TEX_UNITS 32u
#define ZB_GL_TEX_TARGETS 4u
#define ZB_GL_BUF_TARGETS 8u
#define ZB_GL_ATTRIB_SLOTS 16u
#define ZB_GL_SCALAR_SLOTS 19u

#define MISC_ACTIVE_TEX (1u << 0)
#define MISC_PROGRAM    (1u << 1)
#define MISC_VAO        (1u << 2)
#define MISC_FB_DRAW    (1u << 3)
#define MISC_FB_READ    (1u << 4)

#define SCALAR_DEPTH_MASK     0u
#define SCALAR_DEPTH_FUNC     1u
#define SCALAR_CULL_FACE      2u
#define SCALAR_FRONT_FACE     3u
#define SCALAR_BLEND_EQ       4u
#define SCALAR_BLEND_FUNC     5u
#define SCALAR_COLOR_MASK     6u
#define SCALAR_VIEWPORT       7u
#define SCALAR_SCISSOR        8u
#define SCALAR_DEPTH_RNG      9u
#define SCALAR_CLEAR_COL      10u
#define SCALAR_CLEAR_DEPTH    11u
#define SCALAR_STENCIL_MASK_F 12u
#define SCALAR_STENCIL_MASK_B 13u
#define SCALAR_STENCIL_FUNC_F 14u
#define SCALAR_STENCIL_FUNC_B 15u
#define SCALAR_STENCIL_OP_F   16u
#define SCALAR_STENCIL_OP_B   17u
#define SCALAR_CLEAR_STENCIL  18u

/* Raw SVC thunks generated in guest/stubs/gen/libGLESv2.S. */
extern void __zb_svc_glActiveTexture(GLenum texture);
extern void __zb_svc_glBindBuffer(GLenum target, GLuint buffer);
extern void __zb_svc_glBindFramebuffer(GLenum target, GLuint framebuffer);
extern void __zb_svc_glBindTexture(GLenum target, GLuint texture);
extern void __zb_svc_glUseProgram(GLuint program);
extern void __zb_svc_glBindVertexArray(GLuint array);
extern void __zb_svc_glBindVertexArrayOES(GLuint array);
extern void __zb_svc_glDeleteBuffers(GLsizei n, const GLuint* buffers);
extern void __zb_svc_glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers);
extern void __zb_svc_glDeleteTextures(GLsizei n, const GLuint* textures);
extern void __zb_svc_glDeleteProgram(GLuint program);
extern void __zb_svc_glDeleteVertexArrays(GLsizei n, const GLuint* arrays);
extern void __zb_svc_glDeleteVertexArraysOES(GLsizei n, const GLuint* arrays);
extern void __zb_svc_glEnable(GLenum cap);
extern void __zb_svc_glDisable(GLenum cap);
extern void __zb_svc_glEnableVertexAttribArray(GLuint index);
extern void __zb_svc_glDisableVertexAttribArray(GLuint index);
extern void __zb_svc_glPixelStorei(GLenum pname, GLint param);
extern void __zb_svc_glDepthMask(GLboolean flag);
extern void __zb_svc_glDepthFunc(GLenum func);
extern void __zb_svc_glClearDepthf(GLfloat d);
extern void __zb_svc_glStencilMask(GLuint mask);
extern void __zb_svc_glStencilMaskSeparate(GLenum face, GLuint mask);
extern void __zb_svc_glStencilFunc(GLenum func, GLint ref, GLuint mask);
extern void __zb_svc_glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask);
extern void __zb_svc_glStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
extern void __zb_svc_glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
extern void __zb_svc_glCullFace(GLenum mode);
extern void __zb_svc_glFrontFace(GLenum mode);
extern void __zb_svc_glBlendFunc(GLenum sfactor, GLenum dfactor);
extern void __zb_svc_glBlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da);
extern void __zb_svc_glBlendEquation(GLenum mode);
extern void __zb_svc_glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
extern void __zb_svc_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
extern void __zb_svc_glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
extern void __zb_svc_glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
extern void __zb_svc_glDepthRangef(GLfloat n, GLfloat f);
extern void __zb_svc_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
extern void __zb_svc_glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                           GLboolean normalized, GLsizei stride,
                                           const void* pointer);
extern void __zb_svc_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride,
                                            const void* pointer);
extern void __zb_svc_glUniform1i(GLint location, GLint v0);
extern void __zb_svc_glUniform2i(GLint location, GLint v0, GLint v1);
extern void __zb_svc_glUniform1fv(GLint location, GLsizei count, const GLfloat* value);
extern void __zb_svc_glUniform2fv(GLint location, GLsizei count, const GLfloat* value);
extern void __zb_svc_glUniform3fv(GLint location, GLsizei count, const GLfloat* value);
extern void __zb_svc_glUniform4fv(GLint location, GLsizei count, const GLfloat* value);
extern void __zb_svc_glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose,
                                        const GLfloat* value);
extern void __zb_svc_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                                        const GLfloat* value);
extern void __zb_svc_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
extern void __zb_svc_glDrawArrays(GLenum mode, GLint first, GLsizei count);
extern void __zb_svc_glClear(GLbitfield mask);
extern void __zb_svc_glPolygonOffset(GLfloat factor, GLfloat units);
extern void __zb_svc_glVertexAttrib4fv(GLuint index, const GLfloat* v);
extern void __zb_svc_glInvalidateFramebuffer(GLenum target, GLsizei numAttachments,
                                             const GLenum* attachments);
extern void __zb_svc_glBindSampler(GLuint unit, GLuint sampler);
extern void __zb_svc_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                                     const void* data);
extern void __zb_svc_glClearStencil(GLint s);
extern void __zb_svc_glTexParameteri(GLenum target, GLenum pname, GLint param);

struct GlAttribPtr {
    uint32_t buffer;
    int32_t size;
    uint32_t type;
    uint32_t normalized;
    int32_t stride;
    uint32_t pointer;
};

struct GlTlsSlot {
    uint32_t tag;
    uint32_t batch_registered;
    uint32_t host_epoch;
    uint32_t delete_epoch;
    uint32_t misc_known;
    uint32_t active_tex;
    uint32_t program;
    uint32_t vao;
    uint32_t fb_draw;
    uint32_t fb_read;
    uint32_t attrib_known;
    uint32_t attrib_enabled;
    uint32_t attrib_ptr_known;
    uint32_t client_attrib_mask;
    uint32_t high_client_attrib;
    struct GlAttribPtr attrib_ptr[ZB_GL_ATTRIB_SLOTS];
    uint32_t px_known;
    int32_t px_val[3];
    uint32_t buf_known;
    uint32_t buf[ZB_GL_BUF_TARGETS];
    uint32_t cap_known;
    uint32_t cap_val;
    uint32_t scalar_known;
    uint32_t scalar[ZB_GL_SCALAR_SLOTS][4];
    uint32_t tex[ZB_GL_TEX_UNITS][ZB_GL_TEX_TARGETS];
    uint8_t tex_known[ZB_GL_TEX_UNITS];
    struct zb_gl_batch_header batch;
};

static uint32_t g_host_epoch = 1u;
static uint32_t g_delete_epoch = 1u;
static uint32_t g_epoch_registered = 0u;
static struct GlTlsSlot g_slots[ZB_GL_TLS_SLOTS];

static inline uint32_t read_tpidruro(void) {
    uint32_t tp;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
    return tp;
}

static inline uint32_t f32_bits(GLfloat v) {
    union {
        GLfloat f;
        uint32_t u;
    } u = {v};
    return u.u;
}

__attribute__((target("arm"), noinline)) static void register_epoch_once(void) {
    uint32_t expected = 0u;
    if (!__atomic_compare_exchange_n(&g_epoch_registered, &expected, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&g_epoch_registered, __ATOMIC_ACQUIRE) == 1u) {
            __asm__ volatile("" ::: "memory");
        }
        return;
    }
    register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&g_host_epoch;
    register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)&g_delete_epoch;
    __asm__ volatile("svc %2"
                     : "+r"(r0)
                     : "r"(r1), "i"(0x5A0000u | ZB_GL_EPOCH_REGISTER_INDEX)
                     : "memory");
    __atomic_store_n(&g_epoch_registered, 2u, __ATOMIC_RELEASE);
}

__attribute__((target("arm"), noinline)) static uint32_t register_batch_svc(
    struct zb_gl_batch_header* hdr, uint32_t* slot_tag) {
    register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)hdr;
    register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)slot_tag;
    __asm__ volatile("svc %2"
                     : "+r"(r0)
                     : "r"(r1), "i"(0x5A0000u | ZB_GL_BATCH_REGISTER_INDEX)
                     : "memory");
    return r0;
}

__attribute__((target("arm"), noinline)) static void batch_flush_svc(void) {
    __asm__ volatile("svc %0" : : "i"(0x5A0000u | ZB_GL_BATCH_FLUSH_INDEX) : "memory");
}

static inline void reset_slot(struct GlTlsSlot* s, uint32_t he, uint32_t de) {
    s->host_epoch = he;
    s->delete_epoch = de;
    s->misc_known = 0u;
    s->attrib_known = 0u;
    s->attrib_enabled = 0u;
    s->attrib_ptr_known = 0u;
    s->client_attrib_mask = 0xFFFFFFFFu;
    s->high_client_attrib = 0u;
    s->px_known = 0u;
    s->buf_known = 0u;
    s->cap_known = 0u;
    s->cap_val = 0u;
    s->scalar_known = 0u;
    for (uint32_t i = 0; i < ZB_GL_TEX_UNITS; ++i) s->tex_known[i] = 0u;
}

static inline void reset_deleted_objects(struct GlTlsSlot* s, uint32_t de) {
    s->delete_epoch = de;
    /* Deleting buffers/textures/programs/VAOs cannot change cap/scalar/pixelStore/activeTex state,
     * only cached object IDs and buffer-backed attribute assumptions. */
    s->misc_known &= MISC_ACTIVE_TEX;
    s->buf_known = 0u;
    s->attrib_ptr_known = 0u;
    s->client_attrib_mask = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < ZB_GL_TEX_UNITS; ++i) s->tex_known[i] = 0u;
}

/* Returns this thread's cache slot, or NULL if TPIDRURO is 0 or the bucket belongs to another
 * live thread (collision-safe fallback directly to SVC). Slots are reclaimed automatically by the
 * host Process when a GuestThread exits or a borrower is destroyed. */
static inline struct GlTlsSlot* acquire_slot(void) {
    const uint32_t tp = read_tpidruro();
    if (__builtin_expect(tp == 0u, 0)) return (struct GlTlsSlot*)0;
    if (__builtin_expect(__atomic_load_n(&g_epoch_registered, __ATOMIC_ACQUIRE) != 2u, 0)) {
        register_epoch_once();
    }
    const uint32_t idx = ((tp >> 4) ^ (tp >> 11)) & (ZB_GL_TLS_SLOTS - 1u);
    struct GlTlsSlot* s = &g_slots[idx];
    uint32_t tag = __atomic_load_n(&s->tag, __ATOMIC_ACQUIRE);
    if (__builtin_expect(tag != tp, 0)) {
        if (tag != 0u) return (struct GlTlsSlot*)0;
        uint32_t expected = 0u;
        if (!__atomic_compare_exchange_n(&s->tag, &expected, tp, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            return (struct GlTlsSlot*)0;
        }
        s->batch_registered = 0u;
        s->host_epoch = 0u;
        s->delete_epoch = 0u;
    }
    if (__builtin_expect(s->batch_registered == 0u, 0)) {
        s->batch.count = 0u;
        s->batch.used_words = 0u;
        s->batch.disabled = 0u;
        if (register_batch_svc(&s->batch, &s->tag) != 0u) s->batch.disabled = 1u;
        s->batch_registered = 1u;
    }
    const uint32_t he = __atomic_load_n(&g_host_epoch, __ATOMIC_ACQUIRE);
    const uint32_t de = __atomic_load_n(&g_delete_epoch, __ATOMIC_ACQUIRE);
    if (__builtin_expect(s->host_epoch != he, 0)) {
        reset_slot(s, he, de);
    } else if (__builtin_expect(s->delete_epoch != de, 0)) {
        reset_deleted_objects(s, de);
    }
    return s;
}

static inline int batch_active(const struct GlTlsSlot* s) {
    return s != 0 && s->batch.disabled == 0u;
}

static inline uint32_t* batch_reserve(struct GlTlsSlot* s, uint32_t op, uint32_t nwords) {
    const uint32_t need = 1u + nwords;
    if (__builtin_expect(s->batch.used_words + need > ZB_GL_BATCH_WORDS, 0)) {
        batch_flush_svc();
        if (__builtin_expect(s->batch.used_words + need > ZB_GL_BATCH_WORDS, 0)) {
            /* Thread TLS address was reused before registration; re-register and flush once. */
            if (register_batch_svc(&s->batch, &s->tag) == 0u) batch_flush_svc();
            if (s->batch.used_words + need > ZB_GL_BATCH_WORDS) {
                s->batch.count = 0u;
                s->batch.used_words = 0u;
                s->batch.disabled = 1u;
            }
        }
    }
    uint32_t pos = s->batch.used_words;
    s->batch.words[pos] = (op & 0xFFu) | (nwords << 8);
    s->batch.used_words = pos + need;
    s->batch.count += 1u;
    return &s->batch.words[pos + 1u];
}

static inline void batch_emit1(struct GlTlsSlot* s, uint32_t op, uint32_t a0) {
    uint32_t* p = batch_reserve(s, op, 1u);
    p[0] = a0;
}

static inline void batch_emit2(struct GlTlsSlot* s, uint32_t op, uint32_t a0, uint32_t a1) {
    uint32_t* p = batch_reserve(s, op, 2u);
    p[0] = a0;
    p[1] = a1;
}

static inline void batch_emit3(struct GlTlsSlot* s, uint32_t op, uint32_t a0, uint32_t a1,
                               uint32_t a2) {
    uint32_t* p = batch_reserve(s, op, 3u);
    p[0] = a0;
    p[1] = a1;
    p[2] = a2;
}

static inline void batch_emit4(struct GlTlsSlot* s, uint32_t op, uint32_t a0, uint32_t a1,
                               uint32_t a2, uint32_t a3) {
    uint32_t* p = batch_reserve(s, op, 4u);
    p[0] = a0;
    p[1] = a1;
    p[2] = a2;
    p[3] = a3;
}

static inline void batch_emit6(struct GlTlsSlot* s, uint32_t op, uint32_t a0, uint32_t a1,
                               uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    uint32_t* p = batch_reserve(s, op, 6u);
    p[0] = a0;
    p[1] = a1;
    p[2] = a2;
    p[3] = a3;
    p[4] = a4;
    p[5] = a5;
}

static inline int buffer_target_idx(GLenum target) {
    switch (target) {
    case 0x8892u: return 0; /* GL_ARRAY_BUFFER */
    case 0x8893u: return 1; /* GL_ELEMENT_ARRAY_BUFFER */
    case 0x88EBu: return 2; /* GL_PIXEL_PACK_BUFFER */
    case 0x88ECu: return 3; /* GL_PIXEL_UNPACK_BUFFER */
    case 0x8A11u: return 4; /* GL_UNIFORM_BUFFER */
    case 0x8C8Eu: return 5; /* GL_TRANSFORM_FEEDBACK_BUFFER */
    case 0x8F36u: return 6; /* GL_COPY_READ_BUFFER */
    case 0x8F37u: return 7; /* GL_COPY_WRITE_BUFFER */
    default: return -1;
    }
}

static inline int texture_target_idx(GLenum target) {
    switch (target) {
    case 0x0DE1u: return 0; /* GL_TEXTURE_2D */
    case 0x8513u: return 1; /* GL_TEXTURE_CUBE_MAP */
    case 0x8C1Au: return 2; /* GL_TEXTURE_2D_ARRAY */
    case 0x806Fu: return 3; /* GL_TEXTURE_3D */
    default: return -1;
    }
}

static inline int cap_bit(GLenum cap) {
    switch (cap) {
    case 0x0BE2u: return 0; /* GL_BLEND */
    case 0x0B44u: return 1; /* GL_CULL_FACE */
    case 0x0B71u: return 2; /* GL_DEPTH_TEST */
    case 0x0C11u: return 3; /* GL_SCISSOR_TEST */
    case 0x0B90u: return 4; /* GL_STENCIL_TEST */
    case 0x0BD0u: return 5; /* GL_DITHER */
    case 0x8037u: return 6; /* GL_POLYGON_OFFSET_FILL */
    case 0x809Eu: return 7; /* GL_SAMPLE_ALPHA_TO_COVERAGE */
    case 0x80A0u: return 8; /* GL_SAMPLE_COVERAGE */
    default: return -1;
    }
}

static inline int scalar_hit_or_store(struct GlTlsSlot* s, uint32_t slot_id, uint32_t w0, uint32_t w1,
                                      uint32_t w2, uint32_t w3) {
    const uint32_t bit = 1u << slot_id;
    uint32_t* dst = s->scalar[slot_id];
    if ((s->scalar_known & bit) != 0u && dst[0] == w0 && dst[1] == w1 && dst[2] == w2 &&
        dst[3] == w3) {
        return 1;
    }
    dst[0] = w0;
    dst[1] = w1;
    dst[2] = w2;
    dst[3] = w3;
    s->scalar_known |= bit;
    return 0;
}

static inline int scalar_matches(const struct GlTlsSlot* s, uint32_t slot_id, uint32_t w0,
                                 uint32_t w1, uint32_t w2) {
    const uint32_t bit = 1u << slot_id;
    const uint32_t* dst = s->scalar[slot_id];
    return (s->scalar_known & bit) != 0u && dst[0] == w0 && dst[1] == w1 && dst[2] == w2;
}

__attribute__((visibility("default"))) void glActiveTexture(GLenum texture) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && texture >= 0x84C0u && texture < 0x84C0u + ZB_GL_TEX_UNITS) {
        const uint32_t unit = texture - 0x84C0u;
        if ((s->misc_known & MISC_ACTIVE_TEX) != 0u && s->active_tex == unit) return;
        if (batch_active(s)) {
            batch_emit1(s, ZB_GL_OP_ACTIVE_TEXTURE, texture);
        } else {
            __zb_svc_glActiveTexture(texture);
        }
        s->active_tex = unit;
        s->misc_known |= MISC_ACTIVE_TEX;
        return;
    }
    if (s != 0) s->misc_known &= ~MISC_ACTIVE_TEX;
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_ACTIVE_TEXTURE, texture);
    } else {
        __zb_svc_glActiveTexture(texture);
    }
}

__attribute__((visibility("default"))) void glBindBuffer(GLenum target, GLuint buffer) {
    struct GlTlsSlot* s = acquire_slot();
    const int idx = buffer_target_idx(target);
    if (s != 0 && idx >= 0) {
        const uint32_t bit = 1u << (uint32_t)idx;
        if ((s->buf_known & bit) != 0u && s->buf[idx] == buffer) return;
        if (batch_active(s)) {
            batch_emit2(s, ZB_GL_OP_BIND_BUFFER, target, buffer);
        } else {
            __zb_svc_glBindBuffer(target, buffer);
        }
        s->buf[idx] = buffer;
        s->buf_known |= bit;
        return;
    }
    if (batch_active(s)) {
        batch_emit2(s, ZB_GL_OP_BIND_BUFFER, target, buffer);
    } else {
        __zb_svc_glBindBuffer(target, buffer);
    }
}

extern void __zb_svc_glBindBufferBase(GLenum target, GLuint index, GLuint buffer);
extern void __zb_svc_glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset,
                                       GLsizeiptr size);

__attribute__((visibility("default"))) void glBindBufferBase(GLenum target, GLuint index,
                                                             GLuint buffer) {
    struct GlTlsSlot* s = acquire_slot();
    const int idx = buffer_target_idx(target);
    if (s != 0 && idx >= 0) {
        s->buf[idx] = buffer;
        s->buf_known |= (1u << (uint32_t)idx);
    }
    if (batch_active(s)) {
        batch_emit3(s, ZB_GL_OP_BIND_BUFFER_BASE, target, index, buffer);
        return;
    }
    __zb_svc_glBindBufferBase(target, index, buffer);
}

__attribute__((visibility("default"))) void glBindBufferRange(GLenum target, GLuint index,
                                                              GLuint buffer, GLintptr offset,
                                                              GLsizeiptr size) {
    struct GlTlsSlot* s = acquire_slot();
    const int idx = buffer_target_idx(target);
    if (s != 0 && idx >= 0) {
        s->buf[idx] = buffer;
        s->buf_known |= (1u << (uint32_t)idx);
    }
    if (batch_active(s) && offset >= 0 && size >= 0) {
        uint32_t* p = batch_reserve(s, ZB_GL_OP_BIND_BUFFER_RANGE, 5u);
        p[0] = target;
        p[1] = index;
        p[2] = buffer;
        p[3] = (uint32_t)offset;
        p[4] = (uint32_t)size;
        return;
    }
    __zb_svc_glBindBufferRange(target, index, buffer, offset, size);
}

__attribute__((visibility("default"))) void glBindTexture(GLenum target, GLuint texture) {
    struct GlTlsSlot* s = acquire_slot();
    const int slot_idx = texture_target_idx(target);
    if (s != 0 && (s->misc_known & MISC_ACTIVE_TEX) != 0u && slot_idx >= 0) {
        const uint32_t unit = s->active_tex;
        const uint8_t bit = (uint8_t)(1u << (uint32_t)slot_idx);
        if ((s->tex_known[unit] & bit) != 0u && s->tex[unit][slot_idx] == texture) return;
        if (batch_active(s)) {
            batch_emit2(s, ZB_GL_OP_BIND_TEXTURE, target, texture);
        } else {
            __zb_svc_glBindTexture(target, texture);
        }
        s->tex[unit][slot_idx] = texture;
        s->tex_known[unit] |= bit;
        return;
    }
    if (batch_active(s)) {
        batch_emit2(s, ZB_GL_OP_BIND_TEXTURE, target, texture);
    } else {
        __zb_svc_glBindTexture(target, texture);
    }
}

__attribute__((visibility("default"))) void glUseProgram(GLuint program) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0) {
        if ((s->misc_known & MISC_PROGRAM) != 0u && s->program == program) return;
        if (batch_active(s)) {
            batch_emit1(s, ZB_GL_OP_USE_PROGRAM, program);
        } else {
            __zb_svc_glUseProgram(program);
        }
        s->program = program;
        s->misc_known |= MISC_PROGRAM;
        return;
    }
    __zb_svc_glUseProgram(program);
}

__attribute__((visibility("default"))) void glBindVertexArray(GLuint array) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0) {
        if ((s->misc_known & MISC_VAO) != 0u && s->vao == array) return;
        if (batch_active(s)) {
            batch_emit1(s, ZB_GL_OP_BIND_VAO, array);
        } else {
            __zb_svc_glBindVertexArray(array);
        }
        s->vao = array;
        s->misc_known |= MISC_VAO;
        /* GL_ELEMENT_ARRAY_BUFFER and vertex attrib enables/pointers live in the VAO. */
        s->buf_known &= ~(1u << 1);
        s->attrib_known = 0u;
        s->attrib_enabled = 0u;
        s->attrib_ptr_known = 0u;
        s->client_attrib_mask = 0xFFFFFFFFu;
        s->high_client_attrib = 0u;
        return;
    }
    __zb_svc_glBindVertexArray(array);
}

__attribute__((visibility("default"))) void glBindVertexArrayOES(GLuint array) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0) {
        if ((s->misc_known & MISC_VAO) != 0u && s->vao == array) return;
        if (batch_active(s)) {
            batch_emit1(s, ZB_GL_OP_BIND_VAO_OES, array);
        } else {
            __zb_svc_glBindVertexArrayOES(array);
        }
        s->vao = array;
        s->misc_known |= MISC_VAO;
        s->buf_known &= ~(1u << 1);
        s->attrib_known = 0u;
        s->attrib_enabled = 0u;
        s->attrib_ptr_known = 0u;
        s->client_attrib_mask = 0xFFFFFFFFu;
        s->high_client_attrib = 0u;
        return;
    }
    __zb_svc_glBindVertexArrayOES(array);
}

__attribute__((visibility("default"))) void glBindFramebuffer(GLenum target, GLuint framebuffer) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0) {
        if (target == 0x8D40u) { /* GL_FRAMEBUFFER */
            if ((s->misc_known & (MISC_FB_DRAW | MISC_FB_READ)) == (MISC_FB_DRAW | MISC_FB_READ) &&
                s->fb_draw == framebuffer && s->fb_read == framebuffer) {
                return;
            }
            if (batch_active(s)) {
                batch_emit2(s, ZB_GL_OP_BIND_FRAMEBUFFER, target, framebuffer);
            } else {
                __zb_svc_glBindFramebuffer(target, framebuffer);
            }
            s->fb_draw = framebuffer;
            s->fb_read = framebuffer;
            s->misc_known |= (MISC_FB_DRAW | MISC_FB_READ);
            return;
        }
        if (target == 0x8CA9u) { /* GL_DRAW_FRAMEBUFFER */
            if ((s->misc_known & MISC_FB_DRAW) != 0u && s->fb_draw == framebuffer) return;
            if (batch_active(s)) {
                batch_emit2(s, ZB_GL_OP_BIND_FRAMEBUFFER, target, framebuffer);
            } else {
                __zb_svc_glBindFramebuffer(target, framebuffer);
            }
            s->fb_draw = framebuffer;
            s->misc_known |= MISC_FB_DRAW;
            return;
        }
        if (target == 0x8CA8u) { /* GL_READ_FRAMEBUFFER */
            if ((s->misc_known & MISC_FB_READ) != 0u && s->fb_read == framebuffer) return;
            if (batch_active(s)) {
                batch_emit2(s, ZB_GL_OP_BIND_FRAMEBUFFER, target, framebuffer);
            } else {
                __zb_svc_glBindFramebuffer(target, framebuffer);
            }
            s->fb_read = framebuffer;
            s->misc_known |= MISC_FB_READ;
            return;
        }
    }
    if (batch_active(s)) {
        batch_emit2(s, ZB_GL_OP_BIND_FRAMEBUFFER, target, framebuffer);
    } else {
        __zb_svc_glBindFramebuffer(target, framebuffer);
    }
}

__attribute__((visibility("default"))) void glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    __zb_svc_glDeleteBuffers(n, buffers);
    if (n > 0) __atomic_fetch_add(&g_delete_epoch, 1u, __ATOMIC_RELEASE);
}

__attribute__((visibility("default"))) void glDeleteFramebuffers(GLsizei n,
                                                                 const GLuint* framebuffers) {
    __zb_svc_glDeleteFramebuffers(n, framebuffers);
    if (n > 0) __atomic_fetch_add(&g_delete_epoch, 1u, __ATOMIC_RELEASE);
}

__attribute__((visibility("default"))) void glDeleteTextures(GLsizei n, const GLuint* textures) {
    __zb_svc_glDeleteTextures(n, textures);
    if (n > 0) __atomic_fetch_add(&g_delete_epoch, 1u, __ATOMIC_RELEASE);
}

__attribute__((visibility("default"))) void glDeleteProgram(GLuint program) {
    __zb_svc_glDeleteProgram(program);
    if (program != 0u) __atomic_fetch_add(&g_delete_epoch, 1u, __ATOMIC_RELEASE);
}

__attribute__((visibility("default"))) void glDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
    __zb_svc_glDeleteVertexArrays(n, arrays);
    if (n > 0) __atomic_fetch_add(&g_delete_epoch, 1u, __ATOMIC_RELEASE);
}

__attribute__((visibility("default"))) void glDeleteVertexArraysOES(GLsizei n, const GLuint* arrays) {
    __zb_svc_glDeleteVertexArraysOES(n, arrays);
    if (n > 0) __atomic_fetch_add(&g_delete_epoch, 1u, __ATOMIC_RELEASE);
}

__attribute__((visibility("default"))) void glEnableVertexAttribArray(GLuint index) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && index < 32u) {
        const uint32_t mask = 1u << index;
        if ((s->attrib_known & mask) != 0u && (s->attrib_enabled & mask) != 0u) return;
        if (batch_active(s)) {
            batch_emit1(s, ZB_GL_OP_ENABLE_ATTRIB, index);
        } else {
            __zb_svc_glEnableVertexAttribArray(index);
        }
        s->attrib_known |= mask;
        s->attrib_enabled |= mask;
        return;
    }
    if (s != 0 && index >= 32u) s->high_client_attrib = 1u;
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_ENABLE_ATTRIB, index);
    } else {
        __zb_svc_glEnableVertexAttribArray(index);
    }
}

__attribute__((visibility("default"))) void glDisableVertexAttribArray(GLuint index) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && index < 32u) {
        const uint32_t mask = 1u << index;
        if ((s->attrib_known & mask) != 0u && (s->attrib_enabled & mask) == 0u) return;
        if (batch_active(s)) {
            batch_emit1(s, ZB_GL_OP_DISABLE_ATTRIB, index);
        } else {
            __zb_svc_glDisableVertexAttribArray(index);
        }
        s->attrib_known |= mask;
        s->attrib_enabled &= ~mask;
        return;
    }
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_DISABLE_ATTRIB, index);
    } else {
        __zb_svc_glDisableVertexAttribArray(index);
    }
}

__attribute__((visibility("default"))) void glVertexAttribPointer(GLuint index, GLint size,
                                                                  GLenum type,
                                                                  GLboolean normalized,
                                                                  GLsizei stride,
                                                                  const void* pointer) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && index < ZB_GL_ATTRIB_SLOTS && (s->buf_known & 1u) != 0u && s->buf[0] != 0u) {
        const uint32_t bit = 1u << index;
        const uint32_t buf = s->buf[0];
        const uint32_t norm = (normalized != 0u) ? 1u : 0u;
        const uint32_t ptr_u32 = (uint32_t)(uintptr_t)pointer;
        struct GlAttribPtr* ap = &s->attrib_ptr[index];
        if ((s->attrib_ptr_known & bit) != 0u && ap->buffer == buf && ap->size == size &&
            ap->type == type && ap->normalized == norm && ap->stride == stride &&
            ap->pointer == ptr_u32) {
            return;
        }
        if (batch_active(s)) {
            batch_emit6(s, ZB_GL_OP_VERTEX_ATTRIB_PTR, index, (uint32_t)size, type, norm,
                        (uint32_t)stride, ptr_u32);
        } else {
            __zb_svc_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
        }
        ap->buffer = buf;
        ap->size = size;
        ap->type = type;
        ap->normalized = norm;
        ap->stride = stride;
        ap->pointer = ptr_u32;
        s->attrib_ptr_known |= bit;
        s->client_attrib_mask &= ~(1u << index);
        return;
    }
    if (s != 0) {
        if (index < ZB_GL_ATTRIB_SLOTS) s->attrib_ptr_known &= ~(1u << index);
        if (index < 32u) s->client_attrib_mask |= (1u << index);
        else s->high_client_attrib = 1u;
    }
    __zb_svc_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

__attribute__((visibility("default"))) void glVertexAttribIPointer(GLuint index, GLint size,
                                                                   GLenum type, GLsizei stride,
                                                                   const void* pointer) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0) {
        if (index < ZB_GL_ATTRIB_SLOTS) s->attrib_ptr_known &= ~(1u << index);
        if (index < 32u) s->client_attrib_mask |= (1u << index);
        else s->high_client_attrib = 1u;
    }
    __zb_svc_glVertexAttribIPointer(index, size, type, stride, pointer);
}

__attribute__((visibility("default"))) void glUniform1i(GLint location, GLint v0) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s)) {
        batch_emit2(s, ZB_GL_OP_UNIFORM_1I, (uint32_t)location, (uint32_t)v0);
        return;
    }
    __zb_svc_glUniform1i(location, v0);
}

__attribute__((visibility("default"))) void glUniform1fv(GLint location, GLsizei count,
                                                         const GLfloat* value) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s) && count >= 1 && count <= 64 && value != 0) {
        const uint32_t n = (uint32_t)count;
        uint32_t* p = batch_reserve(s, ZB_GL_OP_UNIFORM_1FV, 2u + n);
        p[0] = (uint32_t)location;
        p[1] = n;
        for (uint32_t i = 0; i < n; ++i) p[2u + i] = f32_bits(value[i]);
        return;
    }
    __zb_svc_glUniform1fv(location, count, value);
}

__attribute__((visibility("default"))) void glUniform3fv(GLint location, GLsizei count,
                                                         const GLfloat* value) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s) && count >= 1 && count <= 32 && value != 0) {
        const uint32_t n = (uint32_t)count * 3u;
        uint32_t* p = batch_reserve(s, ZB_GL_OP_UNIFORM_3FV, 2u + n);
        p[0] = (uint32_t)location;
        p[1] = (uint32_t)count;
        for (uint32_t i = 0; i < n; ++i) p[2u + i] = f32_bits(value[i]);
        return;
    }
    __zb_svc_glUniform3fv(location, count, value);
}

__attribute__((visibility("default"))) void glUniform4fv(GLint location, GLsizei count,
                                                         const GLfloat* value) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s) && count >= 1 && count <= 64 && value != 0) {
        const uint32_t n = (uint32_t)count * 4u;
        uint32_t* p = batch_reserve(s, ZB_GL_OP_UNIFORM_4FV, 2u + n);
        p[0] = (uint32_t)location;
        p[1] = (uint32_t)count;
        for (uint32_t i = 0; i < n; ++i) p[2u + i] = f32_bits(value[i]);
        return;
    }
    __zb_svc_glUniform4fv(location, count, value);
}

__attribute__((visibility("default"))) void glUniform2i(GLint location, GLint v0, GLint v1) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s)) {
        batch_emit3(s, ZB_GL_OP_UNIFORM_2I, (uint32_t)location, (uint32_t)v0, (uint32_t)v1);
        return;
    }
    __zb_svc_glUniform2i(location, v0, v1);
}

__attribute__((visibility("default"))) void glUniform2fv(GLint location, GLsizei count,
                                                         const GLfloat* value) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s) && count >= 1 && count <= 64 && value != 0) {
        const uint32_t n = (uint32_t)count * 2u;
        uint32_t* p = batch_reserve(s, ZB_GL_OP_UNIFORM_2FV, 2u + n);
        p[0] = (uint32_t)location;
        p[1] = (uint32_t)count;
        for (uint32_t i = 0; i < n; ++i) p[2u + i] = f32_bits(value[i]);
        return;
    }
    __zb_svc_glUniform2fv(location, count, value);
}

__attribute__((visibility("default"))) void glUniformMatrix3fv(GLint location, GLsizei count,
                                                               GLboolean transpose,
                                                               const GLfloat* value) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s) && count >= 1 && count <= 16 && value != 0) {
        const uint32_t n = (uint32_t)count * 9u;
        uint32_t* p = batch_reserve(s, ZB_GL_OP_UNIFORM_MATRIX_3FV, 3u + n);
        p[0] = (uint32_t)location;
        p[1] = (uint32_t)count;
        p[2] = (transpose != 0u) ? 1u : 0u;
        for (uint32_t i = 0; i < n; ++i) p[3u + i] = f32_bits(value[i]);
        return;
    }
    __zb_svc_glUniformMatrix3fv(location, count, transpose, value);
}

__attribute__((visibility("default"))) void glUniformMatrix4fv(GLint location, GLsizei count,
                                                               GLboolean transpose,
                                                               const GLfloat* value) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s) && count >= 1 && count <= 16 && value != 0) {
        const uint32_t n = (uint32_t)count * 16u;
        uint32_t* p = batch_reserve(s, ZB_GL_OP_UNIFORM_MATRIX_4FV, 3u + n);
        p[0] = (uint32_t)location;
        p[1] = (uint32_t)count;
        p[2] = (transpose != 0u) ? 1u : 0u;
        for (uint32_t i = 0; i < n; ++i) p[3u + i] = f32_bits(value[i]);
        return;
    }
    __zb_svc_glUniformMatrix4fv(location, count, transpose, value);
}

static inline int can_batch_draw_state(const struct GlTlsSlot* s) {
    if (!batch_active(s)) return 0;
    if (s->high_client_attrib != 0u || s->attrib_known == 0u) return 0;
    if ((s->attrib_enabled & ~s->attrib_known) != 0u) return 0;
    if ((s->attrib_enabled & s->client_attrib_mask) != 0u) return 0;
    if ((s->attrib_enabled & ~s->attrib_ptr_known) != 0u) return 0;
    return 1;
}

__attribute__((visibility("default"))) void glDrawElements(GLenum mode, GLsizei count, GLenum type,
                                                           const void* indices) {
    struct GlTlsSlot* s = acquire_slot();
    if (can_batch_draw_state(s) && count >= 0 && (s->buf_known & (1u << 1)) != 0u &&
        s->buf[1] != 0u) {
        batch_emit4(s, ZB_GL_OP_DRAW_ELEMENTS, mode, (uint32_t)count, type,
                    (uint32_t)(uintptr_t)indices);
        return;
    }
    __zb_svc_glDrawElements(mode, count, type, indices);
}

__attribute__((visibility("default"))) void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    struct GlTlsSlot* s = acquire_slot();
    if (can_batch_draw_state(s) && first >= 0 && count >= 0) {
        batch_emit3(s, ZB_GL_OP_DRAW_ARRAYS, mode, (uint32_t)first, (uint32_t)count);
        return;
    }
    __zb_svc_glDrawArrays(mode, first, count);
}

__attribute__((visibility("default"))) void glClear(GLbitfield mask) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_CLEAR, mask);
        return;
    }
    __zb_svc_glClear(mask);
}

__attribute__((visibility("default"))) void glPolygonOffset(GLfloat factor, GLfloat units) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s)) {
        batch_emit2(s, ZB_GL_OP_POLYGON_OFFSET, f32_bits(factor), f32_bits(units));
        return;
    }
    __zb_svc_glPolygonOffset(factor, units);
}

__attribute__((visibility("default"))) void glVertexAttrib4fv(GLuint index, const GLfloat* v) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s) && v != 0) {
        uint32_t* p = batch_reserve(s, ZB_GL_OP_VERTEX_ATTRIB_4FV, 5u);
        p[0] = index;
        p[1] = f32_bits(v[0]);
        p[2] = f32_bits(v[1]);
        p[3] = f32_bits(v[2]);
        p[4] = f32_bits(v[3]);
        return;
    }
    __zb_svc_glVertexAttrib4fv(index, v);
}

__attribute__((visibility("default"))) void glInvalidateFramebuffer(GLenum target,
                                                                    GLsizei numAttachments,
                                                                    const GLenum* attachments) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s) && numAttachments >= 0 && numAttachments <= 8 &&
        (numAttachments == 0 || attachments != 0)) {
        const uint32_t n = (uint32_t)numAttachments;
        uint32_t* p = batch_reserve(s, ZB_GL_OP_INVALIDATE_FRAMEBUFFER, 2u + n);
        p[0] = target;
        p[1] = n;
        for (uint32_t i = 0; i < n; ++i) p[2u + i] = attachments[i];
        return;
    }
    __zb_svc_glInvalidateFramebuffer(target, numAttachments, attachments);
}

__attribute__((visibility("default"))) void glBindSampler(GLuint unit, GLuint sampler) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s)) {
        batch_emit2(s, ZB_GL_OP_BIND_SAMPLER, unit, sampler);
        return;
    }
    __zb_svc_glBindSampler(unit, sampler);
}

__attribute__((visibility("default"))) void glBufferSubData(GLenum target, GLintptr offset,
                                                          GLsizeiptr size, const void* data) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s) && offset >= 0 && size > 0 && size <= 256 && data != 0) {
        const uint32_t bytes = (uint32_t)size;
        const uint32_t nwords = (bytes + 3u) >> 2;
        uint32_t* p = batch_reserve(s, ZB_GL_OP_BUFFER_SUB_DATA, 3u + nwords);
        p[0] = target;
        p[1] = (uint32_t)offset;
        p[2] = bytes;
        p[3u + nwords - 1u] = 0u;
        const uint8_t* src = (const uint8_t*)data;
        uint8_t* dst = (uint8_t*)&p[3];
        for (uint32_t i = 0; i < bytes; ++i) dst[i] = src[i];
        return;
    }
    __zb_svc_glBufferSubData(target, offset, size, data);
}

__attribute__((visibility("default"))) void glPixelStorei(GLenum pname, GLint param) {
    struct GlTlsSlot* s = acquire_slot();
    int idx = -1;
    if (pname == 0x0CF2u) idx = 0;      /* GL_UNPACK_ROW_LENGTH */
    else if (pname == 0x0D05u) idx = 1; /* GL_PACK_ALIGNMENT */
    else if (pname == 0x0CF5u) idx = 2; /* GL_UNPACK_ALIGNMENT */
    if (s != 0 && idx >= 0) {
        const uint32_t bit = 1u << (uint32_t)idx;
        if ((s->px_known & bit) != 0u && s->px_val[idx] == param) return;
        if (batch_active(s)) {
            batch_emit2(s, ZB_GL_OP_PIXEL_STOREI, pname, (uint32_t)param);
        } else {
            __zb_svc_glPixelStorei(pname, param);
        }
        s->px_val[idx] = param;
        s->px_known |= bit;
        return;
    }
    if (batch_active(s)) {
        batch_emit2(s, ZB_GL_OP_PIXEL_STOREI, pname, (uint32_t)param);
    } else {
        __zb_svc_glPixelStorei(pname, param);
    }
}

__attribute__((visibility("default"))) void glEnable(GLenum cap) {
    struct GlTlsSlot* s = acquire_slot();
    const int bit = cap_bit(cap);
    if (s != 0 && bit >= 0) {
        const uint32_t mask = 1u << (uint32_t)bit;
        if ((s->cap_known & mask) != 0u && (s->cap_val & mask) != 0u) return;
        if (batch_active(s)) {
            batch_emit1(s, ZB_GL_OP_ENABLE, cap);
        } else {
            __zb_svc_glEnable(cap);
        }
        s->cap_known |= mask;
        s->cap_val |= mask;
        return;
    }
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_ENABLE, cap);
    } else {
        __zb_svc_glEnable(cap);
    }
}

__attribute__((visibility("default"))) void glDisable(GLenum cap) {
    struct GlTlsSlot* s = acquire_slot();
    const int bit = cap_bit(cap);
    if (s != 0 && bit >= 0) {
        const uint32_t mask = 1u << (uint32_t)bit;
        if ((s->cap_known & mask) != 0u && (s->cap_val & mask) == 0u) return;
        if (batch_active(s)) {
            batch_emit1(s, ZB_GL_OP_DISABLE, cap);
        } else {
            __zb_svc_glDisable(cap);
        }
        s->cap_known |= mask;
        s->cap_val &= ~mask;
        return;
    }
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_DISABLE, cap);
    } else {
        __zb_svc_glDisable(cap);
    }
}

__attribute__((visibility("default"))) void glDepthMask(GLboolean flag) {
    struct GlTlsSlot* s = acquire_slot();
    const uint32_t v = (flag != 0u) ? 1u : 0u;
    if (s != 0 && (s->scalar_known & (1u << SCALAR_DEPTH_MASK)) != 0u &&
        s->scalar[SCALAR_DEPTH_MASK][0] == v) {
        return;
    }
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_DEPTH_MASK, v);
    } else {
        __zb_svc_glDepthMask(flag);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_DEPTH_MASK, v, 0u, 0u, 0u);
}

__attribute__((visibility("default"))) void glDepthFunc(GLenum func) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && (s->scalar_known & (1u << SCALAR_DEPTH_FUNC)) != 0u &&
        s->scalar[SCALAR_DEPTH_FUNC][0] == func) {
        return;
    }
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_DEPTH_FUNC, func);
    } else {
        __zb_svc_glDepthFunc(func);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_DEPTH_FUNC, func, 0u, 0u, 0u);
}

__attribute__((visibility("default"))) void glCullFace(GLenum mode) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && (s->scalar_known & (1u << SCALAR_CULL_FACE)) != 0u &&
        s->scalar[SCALAR_CULL_FACE][0] == mode) {
        return;
    }
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_CULL_FACE, mode);
    } else {
        __zb_svc_glCullFace(mode);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_CULL_FACE, mode, 0u, 0u, 0u);
}

__attribute__((visibility("default"))) void glFrontFace(GLenum mode) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && (s->scalar_known & (1u << SCALAR_FRONT_FACE)) != 0u &&
        s->scalar[SCALAR_FRONT_FACE][0] == mode) {
        return;
    }
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_FRONT_FACE, mode);
    } else {
        __zb_svc_glFrontFace(mode);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_FRONT_FACE, mode, 0u, 0u, 0u);
}

__attribute__((visibility("default"))) void glBlendEquation(GLenum mode) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && (s->scalar_known & (1u << SCALAR_BLEND_EQ)) != 0u &&
        s->scalar[SCALAR_BLEND_EQ][0] == mode && s->scalar[SCALAR_BLEND_EQ][1] == mode) {
        return;
    }
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_BLEND_EQ, mode);
    } else {
        __zb_svc_glBlendEquation(mode);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_BLEND_EQ, mode, mode, 0u, 0u);
}

__attribute__((visibility("default"))) void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && (s->scalar_known & (1u << SCALAR_BLEND_EQ)) != 0u &&
        s->scalar[SCALAR_BLEND_EQ][0] == modeRGB && s->scalar[SCALAR_BLEND_EQ][1] == modeAlpha) {
        return;
    }
    if (batch_active(s)) {
        batch_emit2(s, ZB_GL_OP_BLEND_EQ_SEP, modeRGB, modeAlpha);
    } else {
        __zb_svc_glBlendEquationSeparate(modeRGB, modeAlpha);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_BLEND_EQ, modeRGB, modeAlpha, 0u, 0u);
}

__attribute__((visibility("default"))) void glBlendFunc(GLenum sfactor, GLenum dfactor) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && (s->scalar_known & (1u << SCALAR_BLEND_FUNC)) != 0u &&
        s->scalar[SCALAR_BLEND_FUNC][0] == sfactor && s->scalar[SCALAR_BLEND_FUNC][1] == dfactor &&
        s->scalar[SCALAR_BLEND_FUNC][2] == sfactor && s->scalar[SCALAR_BLEND_FUNC][3] == dfactor) {
        return;
    }
    if (batch_active(s)) {
        batch_emit2(s, ZB_GL_OP_BLEND_FUNC, sfactor, dfactor);
    } else {
        __zb_svc_glBlendFunc(sfactor, dfactor);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_BLEND_FUNC, sfactor, dfactor, sfactor, dfactor);
}

__attribute__((visibility("default"))) void glBlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa,
                                                                GLenum da) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && (s->scalar_known & (1u << SCALAR_BLEND_FUNC)) != 0u &&
        s->scalar[SCALAR_BLEND_FUNC][0] == sr && s->scalar[SCALAR_BLEND_FUNC][1] == dr &&
        s->scalar[SCALAR_BLEND_FUNC][2] == sa && s->scalar[SCALAR_BLEND_FUNC][3] == da) {
        return;
    }
    if (batch_active(s)) {
        batch_emit4(s, ZB_GL_OP_BLEND_FUNC_SEP, sr, dr, sa, da);
    } else {
        __zb_svc_glBlendFuncSeparate(sr, dr, sa, da);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_BLEND_FUNC, sr, dr, sa, da);
}

__attribute__((visibility("default"))) void glColorMask(GLboolean r, GLboolean g, GLboolean b,
                                                        GLboolean a) {
    struct GlTlsSlot* s = acquire_slot();
    const uint32_t wr = (r != 0u) ? 1u : 0u;
    const uint32_t wg = (g != 0u) ? 1u : 0u;
    const uint32_t wb = (b != 0u) ? 1u : 0u;
    const uint32_t wa = (a != 0u) ? 1u : 0u;
    if (s != 0 && (s->scalar_known & (1u << SCALAR_COLOR_MASK)) != 0u &&
        s->scalar[SCALAR_COLOR_MASK][0] == wr && s->scalar[SCALAR_COLOR_MASK][1] == wg &&
        s->scalar[SCALAR_COLOR_MASK][2] == wb && s->scalar[SCALAR_COLOR_MASK][3] == wa) {
        return;
    }
    if (batch_active(s)) {
        batch_emit4(s, ZB_GL_OP_COLOR_MASK, wr, wg, wb, wa);
    } else {
        __zb_svc_glColorMask(r, g, b, a);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_COLOR_MASK, wr, wg, wb, wa);
}

__attribute__((visibility("default"))) void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && w >= 0 && h >= 0 && (s->scalar_known & (1u << SCALAR_VIEWPORT)) != 0u &&
        s->scalar[SCALAR_VIEWPORT][0] == (uint32_t)x &&
        s->scalar[SCALAR_VIEWPORT][1] == (uint32_t)y &&
        s->scalar[SCALAR_VIEWPORT][2] == (uint32_t)w &&
        s->scalar[SCALAR_VIEWPORT][3] == (uint32_t)h) {
        return;
    }
    if (w >= 0 && h >= 0 && batch_active(s)) {
        batch_emit4(s, ZB_GL_OP_VIEWPORT, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
    } else {
        __zb_svc_glViewport(x, y, w, h);
    }
    if (s != 0 && w >= 0 && h >= 0) {
        scalar_hit_or_store(s, SCALAR_VIEWPORT, (uint32_t)x, (uint32_t)y, (uint32_t)w,
                            (uint32_t)h);
    }
}

__attribute__((visibility("default"))) void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && w >= 0 && h >= 0 && (s->scalar_known & (1u << SCALAR_SCISSOR)) != 0u &&
        s->scalar[SCALAR_SCISSOR][0] == (uint32_t)x &&
        s->scalar[SCALAR_SCISSOR][1] == (uint32_t)y &&
        s->scalar[SCALAR_SCISSOR][2] == (uint32_t)w &&
        s->scalar[SCALAR_SCISSOR][3] == (uint32_t)h) {
        return;
    }
    if (w >= 0 && h >= 0 && batch_active(s)) {
        batch_emit4(s, ZB_GL_OP_SCISSOR, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
    } else {
        __zb_svc_glScissor(x, y, w, h);
    }
    if (s != 0 && w >= 0 && h >= 0) {
        scalar_hit_or_store(s, SCALAR_SCISSOR, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
    }
}

__attribute__((visibility("default"))) void glDepthRangef(GLfloat n, GLfloat f) {
    struct GlTlsSlot* s = acquire_slot();
    const uint32_t wn = f32_bits(n);
    const uint32_t wf = f32_bits(f);
    if (s != 0 && (s->scalar_known & (1u << SCALAR_DEPTH_RNG)) != 0u &&
        s->scalar[SCALAR_DEPTH_RNG][0] == wn && s->scalar[SCALAR_DEPTH_RNG][1] == wf) {
        return;
    }
    if (batch_active(s)) {
        batch_emit2(s, ZB_GL_OP_DEPTH_RANGEF, wn, wf);
    } else {
        __zb_svc_glDepthRangef(n, f);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_DEPTH_RNG, wn, wf, 0u, 0u);
}

__attribute__((visibility("default"))) void glClearColor(GLfloat r, GLfloat g, GLfloat b,
                                                         GLfloat a) {
    struct GlTlsSlot* s = acquire_slot();
    const uint32_t wr = f32_bits(r);
    const uint32_t wg = f32_bits(g);
    const uint32_t wb = f32_bits(b);
    const uint32_t wa = f32_bits(a);
    if (s != 0 && (s->scalar_known & (1u << SCALAR_CLEAR_COL)) != 0u &&
        s->scalar[SCALAR_CLEAR_COL][0] == wr && s->scalar[SCALAR_CLEAR_COL][1] == wg &&
        s->scalar[SCALAR_CLEAR_COL][2] == wb && s->scalar[SCALAR_CLEAR_COL][3] == wa) {
        return;
    }
    if (batch_active(s)) {
        batch_emit4(s, ZB_GL_OP_CLEAR_COLOR, wr, wg, wb, wa);
    } else {
        __zb_svc_glClearColor(r, g, b, a);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_CLEAR_COL, wr, wg, wb, wa);
}

__attribute__((visibility("default"))) void glClearDepthf(GLfloat d) {
    struct GlTlsSlot* s = acquire_slot();
    const uint32_t wd = f32_bits(d);
    if (s != 0 && (s->scalar_known & (1u << SCALAR_CLEAR_DEPTH)) != 0u &&
        s->scalar[SCALAR_CLEAR_DEPTH][0] == wd) {
        return;
    }
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_CLEAR_DEPTHF, wd);
    } else {
        __zb_svc_glClearDepthf(d);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_CLEAR_DEPTH, wd, 0u, 0u, 0u);
}

__attribute__((visibility("default"))) void glClearStencil(GLint stencil) {
    struct GlTlsSlot* s = acquire_slot();
    const uint32_t ws = (uint32_t)stencil;
    if (s != 0 && (s->scalar_known & (1u << SCALAR_CLEAR_STENCIL)) != 0u &&
        s->scalar[SCALAR_CLEAR_STENCIL][0] == ws) {
        return;
    }
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_CLEAR_STENCIL, ws);
    } else {
        __zb_svc_glClearStencil(stencil);
    }
    if (s != 0) scalar_hit_or_store(s, SCALAR_CLEAR_STENCIL, ws, 0u, 0u, 0u);
}

__attribute__((visibility("default"))) void glTexParameteri(GLenum target, GLenum pname,
                                                            GLint param) {
    struct GlTlsSlot* s = acquire_slot();
    if (batch_active(s)) {
        batch_emit3(s, ZB_GL_OP_TEX_PARAMETERI, target, pname, (uint32_t)param);
        return;
    }
    __zb_svc_glTexParameteri(target, pname, param);
}

__attribute__((visibility("default"))) void glStencilMask(GLuint mask) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && scalar_matches(s, SCALAR_STENCIL_MASK_F, mask, 0u, 0u) &&
        scalar_matches(s, SCALAR_STENCIL_MASK_B, mask, 0u, 0u)) {
        return;
    }
    if (batch_active(s)) {
        batch_emit1(s, ZB_GL_OP_STENCIL_MASK, mask);
    } else {
        __zb_svc_glStencilMask(mask);
    }
    if (s != 0) {
        scalar_hit_or_store(s, SCALAR_STENCIL_MASK_F, mask, 0u, 0u, 0u);
        scalar_hit_or_store(s, SCALAR_STENCIL_MASK_B, mask, 0u, 0u, 0u);
    }
}

__attribute__((visibility("default"))) void glStencilMaskSeparate(GLenum face, GLuint mask) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0) {
        if (face == 0x0404u) {
            if (scalar_matches(s, SCALAR_STENCIL_MASK_F, mask, 0u, 0u)) return;
            if (batch_active(s)) batch_emit2(s, ZB_GL_OP_STENCIL_MASK_SEP, face, mask);
            else __zb_svc_glStencilMaskSeparate(face, mask);
            scalar_hit_or_store(s, SCALAR_STENCIL_MASK_F, mask, 0u, 0u, 0u);
            return;
        }
        if (face == 0x0405u) {
            if (scalar_matches(s, SCALAR_STENCIL_MASK_B, mask, 0u, 0u)) return;
            if (batch_active(s)) batch_emit2(s, ZB_GL_OP_STENCIL_MASK_SEP, face, mask);
            else __zb_svc_glStencilMaskSeparate(face, mask);
            scalar_hit_or_store(s, SCALAR_STENCIL_MASK_B, mask, 0u, 0u, 0u);
            return;
        }
        if (face == 0x0408u) {
            if (scalar_matches(s, SCALAR_STENCIL_MASK_F, mask, 0u, 0u) &&
                scalar_matches(s, SCALAR_STENCIL_MASK_B, mask, 0u, 0u)) {
                return;
            }
            if (batch_active(s)) batch_emit2(s, ZB_GL_OP_STENCIL_MASK_SEP, face, mask);
            else __zb_svc_glStencilMaskSeparate(face, mask);
            scalar_hit_or_store(s, SCALAR_STENCIL_MASK_F, mask, 0u, 0u, 0u);
            scalar_hit_or_store(s, SCALAR_STENCIL_MASK_B, mask, 0u, 0u, 0u);
            return;
        }
    }
    __zb_svc_glStencilMaskSeparate(face, mask);
}

__attribute__((visibility("default"))) void glStencilFunc(GLenum func, GLint ref, GLuint mask) {
    struct GlTlsSlot* s = acquire_slot();
    const uint32_t r = (uint32_t)ref;
    if (s != 0 && scalar_matches(s, SCALAR_STENCIL_FUNC_F, func, r, mask) &&
        scalar_matches(s, SCALAR_STENCIL_FUNC_B, func, r, mask)) {
        return;
    }
    if (batch_active(s)) {
        batch_emit3(s, ZB_GL_OP_STENCIL_FUNC, func, r, mask);
    } else {
        __zb_svc_glStencilFunc(func, ref, mask);
    }
    if (s != 0) {
        scalar_hit_or_store(s, SCALAR_STENCIL_FUNC_F, func, r, mask, 0u);
        scalar_hit_or_store(s, SCALAR_STENCIL_FUNC_B, func, r, mask, 0u);
    }
}

__attribute__((visibility("default"))) void glStencilFuncSeparate(GLenum face, GLenum func,
                                                                  GLint ref, GLuint mask) {
    struct GlTlsSlot* s = acquire_slot();
    const uint32_t r = (uint32_t)ref;
    if (s != 0) {
        if (face == 0x0404u) {
            if (scalar_matches(s, SCALAR_STENCIL_FUNC_F, func, r, mask)) return;
            if (batch_active(s)) batch_emit4(s, ZB_GL_OP_STENCIL_FUNC_SEP, face, func, r, mask);
            else __zb_svc_glStencilFuncSeparate(face, func, ref, mask);
            scalar_hit_or_store(s, SCALAR_STENCIL_FUNC_F, func, r, mask, 0u);
            return;
        }
        if (face == 0x0405u) {
            if (scalar_matches(s, SCALAR_STENCIL_FUNC_B, func, r, mask)) return;
            if (batch_active(s)) batch_emit4(s, ZB_GL_OP_STENCIL_FUNC_SEP, face, func, r, mask);
            else __zb_svc_glStencilFuncSeparate(face, func, ref, mask);
            scalar_hit_or_store(s, SCALAR_STENCIL_FUNC_B, func, r, mask, 0u);
            return;
        }
        if (face == 0x0408u) {
            if (scalar_matches(s, SCALAR_STENCIL_FUNC_F, func, r, mask) &&
                scalar_matches(s, SCALAR_STENCIL_FUNC_B, func, r, mask)) {
                return;
            }
            if (batch_active(s)) batch_emit4(s, ZB_GL_OP_STENCIL_FUNC_SEP, face, func, r, mask);
            else __zb_svc_glStencilFuncSeparate(face, func, ref, mask);
            scalar_hit_or_store(s, SCALAR_STENCIL_FUNC_F, func, r, mask, 0u);
            scalar_hit_or_store(s, SCALAR_STENCIL_FUNC_B, func, r, mask, 0u);
            return;
        }
    }
    __zb_svc_glStencilFuncSeparate(face, func, ref, mask);
}

__attribute__((visibility("default"))) void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0 && scalar_matches(s, SCALAR_STENCIL_OP_F, fail, zfail, zpass) &&
        scalar_matches(s, SCALAR_STENCIL_OP_B, fail, zfail, zpass)) {
        return;
    }
    if (batch_active(s)) {
        batch_emit3(s, ZB_GL_OP_STENCIL_OP, fail, zfail, zpass);
    } else {
        __zb_svc_glStencilOp(fail, zfail, zpass);
    }
    if (s != 0) {
        scalar_hit_or_store(s, SCALAR_STENCIL_OP_F, fail, zfail, zpass, 0u);
        scalar_hit_or_store(s, SCALAR_STENCIL_OP_B, fail, zfail, zpass, 0u);
    }
}

__attribute__((visibility("default"))) void glStencilOpSeparate(GLenum face, GLenum sfail,
                                                                GLenum dpfail, GLenum dppass) {
    struct GlTlsSlot* s = acquire_slot();
    if (s != 0) {
        if (face == 0x0404u) {
            if (scalar_matches(s, SCALAR_STENCIL_OP_F, sfail, dpfail, dppass)) return;
            if (batch_active(s)) batch_emit4(s, ZB_GL_OP_STENCIL_OP_SEP, face, sfail, dpfail, dppass);
            else __zb_svc_glStencilOpSeparate(face, sfail, dpfail, dppass);
            scalar_hit_or_store(s, SCALAR_STENCIL_OP_F, sfail, dpfail, dppass, 0u);
            return;
        }
        if (face == 0x0405u) {
            if (scalar_matches(s, SCALAR_STENCIL_OP_B, sfail, dpfail, dppass)) return;
            if (batch_active(s)) batch_emit4(s, ZB_GL_OP_STENCIL_OP_SEP, face, sfail, dpfail, dppass);
            else __zb_svc_glStencilOpSeparate(face, sfail, dpfail, dppass);
            scalar_hit_or_store(s, SCALAR_STENCIL_OP_B, sfail, dpfail, dppass, 0u);
            return;
        }
        if (face == 0x0408u) {
            if (scalar_matches(s, SCALAR_STENCIL_OP_F, sfail, dpfail, dppass) &&
                scalar_matches(s, SCALAR_STENCIL_OP_B, sfail, dpfail, dppass)) {
                return;
            }
            if (batch_active(s)) batch_emit4(s, ZB_GL_OP_STENCIL_OP_SEP, face, sfail, dpfail, dppass);
            else __zb_svc_glStencilOpSeparate(face, sfail, dpfail, dppass);
            scalar_hit_or_store(s, SCALAR_STENCIL_OP_F, sfail, dpfail, dppass, 0u);
            scalar_hit_or_store(s, SCALAR_STENCIL_OP_B, sfail, dpfail, dppass, 0u);
            return;
        }
    }
    __zb_svc_glStencilOpSeparate(face, sfail, dpfail, dppass);
}
