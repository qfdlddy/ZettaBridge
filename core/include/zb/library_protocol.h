#pragma once

/* Library-mode handshake between the host LibraryRuntime and guest/zbhost/zbhost.c.
 * C-compatible: the arm32 guest includes this header too. */

#include <stdint.h>

#define ZB_SERVICE_PROTOCOL_VERSION 2u

/* Host-call indices 0xFE00-0xFEFF belong to the library runtime; tools/gen_stubs.py keeps
 * generated stub indices below this range. */
#define ZB_RUNTIME_HOST_CALL_FIRST 0xFE00u
#define ZB_RUNTIME_HOST_CALL_LAST 0xFEFFu
#define ZB_SERVICE_READY_INDEX 0xFE00u
#define ZB_CARRIER_PARK_INDEX 0xFE01u
#define ZB_GL_EPOCH_REGISTER_INDEX 0xFE02u
#define ZB_GL_BATCH_REGISTER_INDEX 0xFE03u
#define ZB_GL_BATCH_FLUSH_INDEX 0xFE04u

/* Shared per-thread GL void-command batch buffer allocated in guest static storage inside
 * libGLESv2.so. Guest appends packed commands without trapping; HostGl drains them on the next
 * unbatched GL/EGL host call or when the buffer triggers ZB_GL_BATCH_FLUSH_INDEX. */
#define ZB_GL_BATCH_WORDS 2048u

enum zb_gl_batch_op {
    ZB_GL_OP_ACTIVE_TEXTURE = 1,
    ZB_GL_OP_BIND_BUFFER = 2,
    ZB_GL_OP_BIND_FRAMEBUFFER = 3,
    ZB_GL_OP_BIND_TEXTURE = 4,
    ZB_GL_OP_USE_PROGRAM = 5,
    ZB_GL_OP_BIND_VAO = 6,
    ZB_GL_OP_BIND_VAO_OES = 7,
    ZB_GL_OP_ENABLE = 8,
    ZB_GL_OP_DISABLE = 9,
    ZB_GL_OP_ENABLE_ATTRIB = 10,
    ZB_GL_OP_DISABLE_ATTRIB = 11,
    ZB_GL_OP_PIXEL_STOREI = 12,
    ZB_GL_OP_DEPTH_MASK = 13,
    ZB_GL_OP_DEPTH_FUNC = 14,
    ZB_GL_OP_CLEAR_DEPTHF = 15,
    ZB_GL_OP_STENCIL_MASK = 16,
    ZB_GL_OP_STENCIL_MASK_SEP = 17,
    ZB_GL_OP_STENCIL_FUNC = 18,
    ZB_GL_OP_STENCIL_FUNC_SEP = 19,
    ZB_GL_OP_STENCIL_OP = 20,
    ZB_GL_OP_STENCIL_OP_SEP = 21,
    ZB_GL_OP_CULL_FACE = 22,
    ZB_GL_OP_FRONT_FACE = 23,
    ZB_GL_OP_BLEND_FUNC = 24,
    ZB_GL_OP_BLEND_FUNC_SEP = 25,
    ZB_GL_OP_BLEND_EQ = 26,
    ZB_GL_OP_BLEND_EQ_SEP = 27,
    ZB_GL_OP_COLOR_MASK = 28,
    ZB_GL_OP_VIEWPORT = 29,
    ZB_GL_OP_SCISSOR = 30,
    ZB_GL_OP_DEPTH_RANGEF = 31,
    ZB_GL_OP_CLEAR_COLOR = 32,
    ZB_GL_OP_VERTEX_ATTRIB_PTR = 33,
    ZB_GL_OP_UNIFORM_1I = 34,
    ZB_GL_OP_UNIFORM_1FV = 35,
    ZB_GL_OP_UNIFORM_3FV = 36,
    ZB_GL_OP_UNIFORM_4FV = 37,
    ZB_GL_OP_DRAW_ELEMENTS = 38,
    ZB_GL_OP_DRAW_ARRAYS = 39,
    ZB_GL_OP_CLEAR = 40,
    ZB_GL_OP_POLYGON_OFFSET = 41,
    ZB_GL_OP_VERTEX_ATTRIB_4FV = 42,
    ZB_GL_OP_INVALIDATE_FRAMEBUFFER = 43,
    ZB_GL_OP_BIND_SAMPLER = 44,
    ZB_GL_OP_BUFFER_SUB_DATA = 45,
    ZB_GL_OP_CLEAR_STENCIL = 46,
    ZB_GL_OP_TEX_PARAMETERI = 47,
    ZB_GL_OP_BIND_BUFFER_BASE = 48,
    ZB_GL_OP_BIND_BUFFER_RANGE = 49,
    ZB_GL_OP_UNIFORM_2I = 50,
    ZB_GL_OP_UNIFORM_2FV = 51,
    ZB_GL_OP_UNIFORM_MATRIX_3FV = 52,
    ZB_GL_OP_UNIFORM_MATRIX_4FV = 53,
};

struct zb_gl_batch_header {
    uint32_t count;
    uint32_t used_words;
    uint32_t disabled;
    uint32_t reserved;
    uint32_t words[ZB_GL_BATCH_WORDS];
};

/* r0 from READY or PARK: return to guest code so pending signals are delivered, then make
 * the same host call again. */
#define ZB_SERVICE_AGAIN 0xFFFFFFF5u

#define ZB_SERVICE_SCRATCH_SIZE 4096u

/* zbhost exit statuses before READY. */
#define ZB_HOST_EXIT_USAGE 2
#define ZB_HOST_EXIT_TARGET_SDK 3
#define ZB_HOST_EXIT_PRELOAD 4

/* 32-bit bionic dlopen flags and handles. Host <dlfcn.h> values differ (host RTLD_NOW is 2,
 * RTLD_GLOBAL 0x100, RTLD_DEFAULT 0). */
#define ZB_GUEST_RTLD_NOW 0u
#define ZB_GUEST_RTLD_LAZY 1u
#define ZB_GUEST_RTLD_GLOBAL 2u
#define ZB_GUEST_RTLD_NOLOAD 4u
#define ZB_GUEST_RTLD_NODELETE 0x1000u
#define ZB_GUEST_RTLD_DEFAULT 0xffffffffu

struct zb_service_api {
    uint32_t size;
    uint32_t version;
    uint32_t dlopen_fn;        /* uint32_t (const char* path, uint32_t guest_flags) */
    uint32_t dlsym_fn;         /* uint32_t (uint32_t handle, const char* name) */
    uint32_t dlerror_fn;       /* uint32_t (void): const char* or 0, per guest thread */
    uint32_t spawn_carrier_fn; /* uint32_t (void): pthread_create result */
    uint32_t malloc_fn;        /* uint32_t (uint32_t size) */
    uint32_t free_fn;          /* void (uint32_t pointer) */
    uint32_t scratch;          /* service-thread string buffer */
    uint32_t scratch_size;
};
