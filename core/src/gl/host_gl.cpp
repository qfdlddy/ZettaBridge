#include "zb/host_gl.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gl/frame_probe.h"
#include "gl/gl_diagnostics.h"
#include "zb/gles1_hostcalls.h"
#include "zb/gl_hostcalls.h"
#include "zb/library_protocol.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

// One name lookup for both GL profiles: the 2.0/3.0/extension blocks and the GLES 1.1-only
// block never overlap, so the order is free.
const char* gl_profile_name(std::uint32_t index) {
    return is_gl1_host_call(index) ? gl1_host_call_name(index) : gl_host_call_name(index);
}

}  // namespace

HostGl::Call::Call(HostGl& host, GuestThread& thread, std::uint32_t index)
    : host_(host), thread_(thread), index_(index),
      regs_{thread.regs()[0], thread.regs()[1], thread.regs()[2], thread.regs()[3]} {
    // Capture arguments before installing the default void/zero result.
    thread_.regs()[0] = 0;
    thread_.regs()[1] = 0;
}

std::uint32_t HostGl::Call::arg(unsigned position) {
    if (position < 4) return regs_[position];
    const std::uint64_t address = static_cast<std::uint64_t>(thread_.regs()[13]) + 4u * (position - 4u);
    if (address > UINT32_MAX) {
        fail(kGlInvalidValue, "argument address overflowed the guest stack");
        return 0;
    }
    const std::uint8_t* source =
        host_.runtime().memory().host_ptr(static_cast<std::uint32_t>(address), 4, kPageRead);
    if (source == nullptr) {
        fail(kGlInvalidValue, "argument is not on a readable guest stack");
        return 0;
    }
    std::uint32_t value;
    std::memcpy(&value, source, sizeof value);
    return value;
}

void HostGl::Call::fail(GLenum error, const char* reason) {
    if (!valid_) return;
    valid_ = false;
    host_.reject(*this, error, reason);
}

void HostGl::reject(Call& call, GLenum error, const char* reason) {
    const char* name = gl_profile_name(call.index());
    log("GLES %s rejected: %s", name, reason);
    // Guests rarely call glGetError, so a rejection is otherwise silent (a texture that never
    // got its pixels renders black). Count them, and keep the first few with their arguments.
    static std::atomic<std::uint64_t> rejections{0};
    const std::uint64_t number = ++rejections;
    runtime_report().note_gl_detail("rejections", std::to_string(number), true);
    if (number <= 4) {
        char text[256];
        std::snprintf(text, sizeof text, "%s: %s args=0x%x,0x%x,0x%x,0x%x", name, reason, call.arg(0),
                      call.arg(1), call.arg(2), call.arg(3));
        runtime_report().note_gl_detail("rejection-" + std::to_string(number), text, false);
    }
    backend_.set_error(error);
}

#include "gen/gl_manual.inc"

#include "gen/gl_dispatch.inc"

namespace {

bool gl_trace_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ZB_GL_TRACE");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

#if defined(ZB_NO_TELEMETRY)
inline void record_gl_census(std::uint32_t, std::uint32_t, std::uint64_t) {}
#else
void record_gl_census(std::uint32_t index, std::uint32_t flush_batch, std::uint64_t report_stride) {
    // Per-function census. Accumulated in thread-local counters to avoid shared cache-line
    // contention on every GL call, then flushed in batches to the global report. The limit
    // covers the highest GL range (GLES 1.1 at 492-578); every other stub range is below it.
    constexpr std::uint32_t kCensusSlotLimit = 640;
    struct GlobalCensus {
        std::atomic<std::uint64_t> counts[kCensusSlotLimit]{};
        std::atomic<std::uint64_t> total{0};
        std::atomic<std::uint64_t> next_report_at{1000};
    };
    static GlobalCensus g_census;
    struct ThreadCensus {
        std::uint32_t counts[kCensusSlotLimit]{};
        std::uint32_t pending = 0;
        std::uint64_t report_stride = 100000;

        void flush() {
            if (pending == 0) return;
            for (std::uint32_t i = 0; i < kCensusSlotLimit; ++i) {
                const std::uint32_t delta = counts[i];
                if (delta != 0) {
                    g_census.counts[i].fetch_add(delta, std::memory_order_relaxed);
                    counts[i] = 0;
                }
            }
            const std::uint64_t total_after =
                g_census.total.fetch_add(pending, std::memory_order_relaxed) + pending;
            pending = 0;
            std::uint64_t threshold = g_census.next_report_at.load(std::memory_order_relaxed);
            if (total_after >= threshold &&
                g_census.next_report_at.compare_exchange_strong(
                    threshold, total_after + report_stride, std::memory_order_relaxed)) {
                // Chunked: RuntimeReport truncates one value at a few hundred characters, and a
                // truncated census silently drops exactly the functions nobody called.
                std::string chunk;
                std::uint32_t chunk_number = 0;
                const auto flush_chunk = [&] {
                    if (chunk.empty()) return;
                    ++chunk_number;
                    const std::string key = "census-" + std::to_string(chunk_number);
                    runtime_report().note_gl_detail(key, chunk, true);
                    chunk.clear();
                };
                for (std::uint32_t i = 0; i < kCensusSlotLimit; ++i) {
                    const std::uint64_t n = g_census.counts[i].load(std::memory_order_relaxed);
                    if (n == 0) continue;
                    if (chunk.size() > 300) flush_chunk();
                    if (!chunk.empty()) chunk += " ";
                    chunk += gl_profile_name(i);
                    chunk += "=";
                    chunk += std::to_string(n);
                }
                flush_chunk();
                if (chunk_number == 0) runtime_report().note_gl_detail("census-1", "(none)", true);
            }
        }

        ~ThreadCensus() { flush(); }
    };
    thread_local ThreadCensus t_census;
    t_census.report_stride = report_stride;
    if (index < kCensusSlotLimit) ++t_census.counts[index];
    ++t_census.pending;
    if (__builtin_expect(t_census.pending >= flush_batch, 0)) {
        t_census.flush();
    }
}
#endif  // ZB_NO_TELEMETRY

bool gl_call_can_run_fast(const HostGl& host, std::uint32_t index, GuestThread& thread) {
    switch (index) {
    case ZB_GL_HC_glGetString:
    case ZB_GL_HC_glGetStringi:
    case ZB_GL_HC_glMapBufferOES:
    case ZB_GL_HC_glGetError:
        return false;
    case ZB_GL_HC_glMapBufferRange:
        return host.can_fast_map_buffer_range(thread);
    case ZB_GL_HC_glUnmapBuffer:
    case ZB_GL_HC_glUnmapBufferOES:
        return host.can_fast_unmap_buffer(thread);
    default:
        return true;
    }
}

float u32_to_f32(std::uint32_t bits) {
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

}  // namespace

void HostGl::flush_batch(GuestThread& thread) {
    auto* hdr = static_cast<zb_gl_batch_header*>(thread.gl_batch_host_ptr());
    if (__builtin_expect(hdr == nullptr, 0)) {
        const std::uint32_t batch_addr = thread.gl_batch_addr();
        if (__builtin_expect(batch_addr == 0, 1)) return;
        hdr = reinterpret_cast<zb_gl_batch_header*>(
            runtime_.memory().host_ptr(batch_addr, sizeof(zb_gl_batch_header), kPageRead | kPageWrite));
        if (__builtin_expect(hdr == nullptr, 0)) return;
        thread.set_gl_batch_host_ptr(hdr);
    }
    if (__builtin_expect(gl_trace_enabled() || gl_diagnostics_enabled(), 0)) {
        hdr->disabled = 1u;
    }
    const std::uint32_t count = hdr->count;
    if (__builtin_expect(count == 0, 1)) return;
    invalidate_state_filter();
    const std::uint32_t used_words = std::min<std::uint32_t>(hdr->used_words, ZB_GL_BATCH_WORDS);
    const std::uint32_t* w = hdr->words;
    std::uint32_t pos = 0;
    for (std::uint32_t i = 0; i < count && pos < used_words; ++i) {
        const std::uint32_t header = w[pos++];
        const std::uint32_t op = header & 0xFFu;
        const std::uint32_t nwords = header >> 8;
        if (pos + nwords > used_words) break;
        switch (op) {
        case ZB_GL_OP_ACTIVE_TEXTURE:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glActiveTexture, 4096u, 250000u);
                backend_.glActiveTexture(w[pos]);
            }
            break;
        case ZB_GL_OP_BIND_BUFFER:
            if (nwords == 2) {
                record_gl_census(ZB_GL_HC_glBindBuffer, 4096u, 250000u);
                backend_.glBindBuffer(w[pos], w[pos + 1]);
                note_bind_buffer(w[pos], w[pos + 1]);
            }
            break;
        case ZB_GL_OP_BIND_FRAMEBUFFER:
            if (nwords == 2) {
                record_gl_census(ZB_GL_HC_glBindFramebuffer, 4096u, 250000u);
                backend_.glBindFramebuffer(w[pos], w[pos + 1]);
            }
            break;
        case ZB_GL_OP_BIND_TEXTURE:
            if (nwords == 2) {
                record_gl_census(ZB_GL_HC_glBindTexture, 4096u, 250000u);
                backend_.glBindTexture(w[pos], w[pos + 1]);
            }
            break;
        case ZB_GL_OP_USE_PROGRAM:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glUseProgram, 4096u, 250000u);
                backend_.glUseProgram(w[pos]);
            }
            break;
        case ZB_GL_OP_BIND_VAO:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glBindVertexArray, 4096u, 250000u);
                backend_.glBindVertexArray(w[pos]);
                note_bind_vertex_array(w[pos]);
            }
            break;
        case ZB_GL_OP_BIND_VAO_OES:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glBindVertexArrayOES, 4096u, 250000u);
                backend_.glBindVertexArrayOES(w[pos]);
                note_bind_vertex_array(w[pos]);
            }
            break;
        case ZB_GL_OP_ENABLE:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glEnable, 4096u, 250000u);
                backend_.glEnable(w[pos]);
            }
            break;
        case ZB_GL_OP_DISABLE:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glDisable, 4096u, 250000u);
                backend_.glDisable(w[pos]);
            }
            break;
        case ZB_GL_OP_ENABLE_ATTRIB:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glEnableVertexAttribArray, 4096u, 250000u);
                backend_.glEnableVertexAttribArray(w[pos]);
                note_vertex_attrib_enabled(w[pos], true);
            }
            break;
        case ZB_GL_OP_DISABLE_ATTRIB:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glDisableVertexAttribArray, 4096u, 250000u);
                backend_.glDisableVertexAttribArray(w[pos]);
                note_vertex_attrib_enabled(w[pos], false);
            }
            break;
        case ZB_GL_OP_PIXEL_STOREI:
            if (nwords == 2) {
                record_gl_census(ZB_GL_HC_glPixelStorei, 4096u, 250000u);
                backend_.glPixelStorei(w[pos], static_cast<GLint>(w[pos + 1]));
                note_pixel_store(w[pos], static_cast<GLint>(w[pos + 1]));
            }
            break;
        case ZB_GL_OP_DEPTH_MASK:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glDepthMask, 4096u, 250000u);
                backend_.glDepthMask(static_cast<GLboolean>(w[pos]));
            }
            break;
        case ZB_GL_OP_DEPTH_FUNC:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glDepthFunc, 4096u, 250000u);
                backend_.glDepthFunc(w[pos]);
            }
            break;
        case ZB_GL_OP_CLEAR_DEPTHF:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glClearDepthf, 4096u, 250000u);
                backend_.glClearDepthf(u32_to_f32(w[pos]));
            }
            break;
        case ZB_GL_OP_STENCIL_MASK:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glStencilMask, 4096u, 250000u);
                backend_.glStencilMask(w[pos]);
            }
            break;
        case ZB_GL_OP_STENCIL_MASK_SEP:
            if (nwords == 2) {
                record_gl_census(ZB_GL_HC_glStencilMaskSeparate, 4096u, 250000u);
                backend_.glStencilMaskSeparate(w[pos], w[pos + 1]);
            }
            break;
        case ZB_GL_OP_STENCIL_FUNC:
            if (nwords == 3) {
                record_gl_census(ZB_GL_HC_glStencilFunc, 4096u, 250000u);
                backend_.glStencilFunc(w[pos], static_cast<GLint>(w[pos + 1]), w[pos + 2]);
            }
            break;
        case ZB_GL_OP_STENCIL_FUNC_SEP:
            if (nwords == 4) {
                record_gl_census(ZB_GL_HC_glStencilFuncSeparate, 4096u, 250000u);
                backend_.glStencilFuncSeparate(w[pos], w[pos + 1], static_cast<GLint>(w[pos + 2]),
                                               w[pos + 3]);
            }
            break;
        case ZB_GL_OP_STENCIL_OP:
            if (nwords == 3) {
                record_gl_census(ZB_GL_HC_glStencilOp, 4096u, 250000u);
                backend_.glStencilOp(w[pos], w[pos + 1], w[pos + 2]);
            }
            break;
        case ZB_GL_OP_STENCIL_OP_SEP:
            if (nwords == 4) {
                record_gl_census(ZB_GL_HC_glStencilOpSeparate, 4096u, 250000u);
                backend_.glStencilOpSeparate(w[pos], w[pos + 1], w[pos + 2], w[pos + 3]);
            }
            break;
        case ZB_GL_OP_CULL_FACE:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glCullFace, 4096u, 250000u);
                backend_.glCullFace(w[pos]);
            }
            break;
        case ZB_GL_OP_FRONT_FACE:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glFrontFace, 4096u, 250000u);
                backend_.glFrontFace(w[pos]);
            }
            break;
        case ZB_GL_OP_BLEND_FUNC:
            if (nwords == 2) {
                record_gl_census(ZB_GL_HC_glBlendFunc, 4096u, 250000u);
                backend_.glBlendFunc(w[pos], w[pos + 1]);
            }
            break;
        case ZB_GL_OP_BLEND_FUNC_SEP:
            if (nwords == 4) {
                record_gl_census(ZB_GL_HC_glBlendFuncSeparate, 4096u, 250000u);
                backend_.glBlendFuncSeparate(w[pos], w[pos + 1], w[pos + 2], w[pos + 3]);
            }
            break;
        case ZB_GL_OP_BLEND_EQ:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glBlendEquation, 4096u, 250000u);
                backend_.glBlendEquation(w[pos]);
            }
            break;
        case ZB_GL_OP_BLEND_EQ_SEP:
            if (nwords == 2) {
                record_gl_census(ZB_GL_HC_glBlendEquationSeparate, 4096u, 250000u);
                backend_.glBlendEquationSeparate(w[pos], w[pos + 1]);
            }
            break;
        case ZB_GL_OP_COLOR_MASK:
            if (nwords == 4) {
                record_gl_census(ZB_GL_HC_glColorMask, 4096u, 250000u);
                backend_.glColorMask(static_cast<GLboolean>(w[pos]),
                                     static_cast<GLboolean>(w[pos + 1]),
                                     static_cast<GLboolean>(w[pos + 2]),
                                     static_cast<GLboolean>(w[pos + 3]));
            }
            break;
        case ZB_GL_OP_VIEWPORT:
            if (nwords == 4) {
                record_gl_census(ZB_GL_HC_glViewport, 4096u, 250000u);
                backend_.glViewport(static_cast<GLint>(w[pos]), static_cast<GLint>(w[pos + 1]),
                                    static_cast<GLsizei>(w[pos + 2]),
                                    static_cast<GLsizei>(w[pos + 3]));
            }
            break;
        case ZB_GL_OP_SCISSOR:
            if (nwords == 4) {
                record_gl_census(ZB_GL_HC_glScissor, 4096u, 250000u);
                backend_.glScissor(static_cast<GLint>(w[pos]), static_cast<GLint>(w[pos + 1]),
                                   static_cast<GLsizei>(w[pos + 2]),
                                   static_cast<GLsizei>(w[pos + 3]));
            }
            break;
        case ZB_GL_OP_DEPTH_RANGEF:
            if (nwords == 2) {
                record_gl_census(ZB_GL_HC_glDepthRangef, 4096u, 250000u);
                backend_.glDepthRangef(u32_to_f32(w[pos]), u32_to_f32(w[pos + 1]));
            }
            break;
        case ZB_GL_OP_CLEAR_COLOR:
            if (nwords == 4) {
                record_gl_census(ZB_GL_HC_glClearColor, 4096u, 250000u);
                backend_.glClearColor(u32_to_f32(w[pos]), u32_to_f32(w[pos + 1]),
                                      u32_to_f32(w[pos + 2]), u32_to_f32(w[pos + 3]));
            }
            break;
        case ZB_GL_OP_VERTEX_ATTRIB_PTR:
            if (nwords == 6) {
                record_gl_census(ZB_GL_HC_glVertexAttribPointer, 4096u, 250000u);
                apply_batched_vertex_attrib_pointer(
                    w[pos], static_cast<GLint>(w[pos + 1]), w[pos + 2],
                    static_cast<GLboolean>(w[pos + 3]), static_cast<GLsizei>(w[pos + 4]),
                    w[pos + 5]);
            }
            break;
        case ZB_GL_OP_UNIFORM_1I:
            if (nwords == 2) {
                record_gl_census(ZB_GL_HC_glUniform1i, 4096u, 250000u);
                backend_.glUniform1i(static_cast<GLint>(w[pos]), static_cast<GLint>(w[pos + 1]));
            }
            break;
        case ZB_GL_OP_UNIFORM_1FV:
            if (nwords >= 2) {
                const auto cnt = static_cast<GLsizei>(w[pos + 1]);
                if (cnt >= 1 && nwords == 2u + static_cast<std::uint32_t>(cnt)) {
                    record_gl_census(ZB_GL_HC_glUniform1fv, 4096u, 250000u);
                    backend_.glUniform1fv(static_cast<GLint>(w[pos]), cnt,
                                          reinterpret_cast<const GLfloat*>(w + pos + 2));
                }
            }
            break;
        case ZB_GL_OP_UNIFORM_3FV:
            if (nwords >= 2) {
                const auto cnt = static_cast<GLsizei>(w[pos + 1]);
                if (cnt >= 1 && nwords == 2u + 3u * static_cast<std::uint32_t>(cnt)) {
                    record_gl_census(ZB_GL_HC_glUniform3fv, 4096u, 250000u);
                    backend_.glUniform3fv(static_cast<GLint>(w[pos]), cnt,
                                          reinterpret_cast<const GLfloat*>(w + pos + 2));
                }
            }
            break;
        case ZB_GL_OP_UNIFORM_4FV:
            if (nwords >= 2) {
                const auto cnt = static_cast<GLsizei>(w[pos + 1]);
                if (cnt >= 1 && nwords == 2u + 4u * static_cast<std::uint32_t>(cnt)) {
                    record_gl_census(ZB_GL_HC_glUniform4fv, 4096u, 250000u);
                    backend_.glUniform4fv(static_cast<GLint>(w[pos]), cnt,
                                          reinterpret_cast<const GLfloat*>(w + pos + 2));
                }
            }
            break;
        case ZB_GL_OP_DRAW_ELEMENTS:
            if (nwords == 4) {
                record_gl_census(ZB_GL_HC_glDrawElements, 4096u, 250000u);
                apply_batched_draw_elements(thread, w[pos], static_cast<GLsizei>(w[pos + 1]),
                                            w[pos + 2], w[pos + 3]);
            }
            break;
        case ZB_GL_OP_DRAW_ARRAYS:
            if (nwords == 3) {
                record_gl_census(ZB_GL_HC_glDrawArrays, 4096u, 250000u);
                apply_batched_draw_arrays(thread, w[pos], static_cast<GLint>(w[pos + 1]),
                                          static_cast<GLsizei>(w[pos + 2]));
            }
            break;
        case ZB_GL_OP_CLEAR:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glClear, 4096u, 250000u);
                backend_.glClear(w[pos]);
            }
            break;
        case ZB_GL_OP_POLYGON_OFFSET:
            if (nwords == 2) {
                record_gl_census(ZB_GL_HC_glPolygonOffset, 4096u, 250000u);
                backend_.glPolygonOffset(u32_to_f32(w[pos]), u32_to_f32(w[pos + 1]));
            }
            break;
        case ZB_GL_OP_VERTEX_ATTRIB_4FV:
            if (nwords == 5) {
                record_gl_census(ZB_GL_HC_glVertexAttrib4fv, 4096u, 250000u);
                backend_.glVertexAttrib4fv(w[pos],
                                           reinterpret_cast<const GLfloat*>(w + pos + 1));
            }
            break;
        case ZB_GL_OP_INVALIDATE_FRAMEBUFFER:
            if (nwords >= 2) {
                const auto cnt = static_cast<GLsizei>(w[pos + 1]);
                if (cnt >= 0 && cnt <= 8 && nwords == 2u + static_cast<std::uint32_t>(cnt)) {
                    record_gl_census(ZB_GL_HC_glInvalidateFramebuffer, 4096u, 250000u);
                    backend_.glInvalidateFramebuffer(
                        w[pos], cnt,
                        cnt == 0 ? nullptr : reinterpret_cast<const GLenum*>(w + pos + 2));
                }
            }
            break;
        case ZB_GL_OP_BIND_SAMPLER:
            if (nwords == 2) {
                record_gl_census(ZB_GL_HC_glBindSampler, 4096u, 250000u);
                backend_.glBindSampler(w[pos], w[pos + 1]);
            }
            break;
        case ZB_GL_OP_BUFFER_SUB_DATA:
            if (nwords >= 3) {
                const auto offset = static_cast<GLintptr>(static_cast<std::int32_t>(w[pos + 1]));
                const auto size = static_cast<GLsizeiptr>(static_cast<std::int32_t>(w[pos + 2]));
                if (offset >= 0 && size >= 0 &&
                    nwords == 3u + ((static_cast<std::uint32_t>(size) + 3u) >> 2)) {
                    record_gl_census(ZB_GL_HC_glBufferSubData, 4096u, 250000u);
                    backend_.glBufferSubData(
                        w[pos], offset, size,
                        size == 0 ? nullptr : reinterpret_cast<const void*>(w + pos + 3));
                }
            }
            break;
        case ZB_GL_OP_CLEAR_STENCIL:
            if (nwords == 1) {
                record_gl_census(ZB_GL_HC_glClearStencil, 4096u, 250000u);
                backend_.glClearStencil(static_cast<GLint>(w[pos]));
            }
            break;
        case ZB_GL_OP_TEX_PARAMETERI:
            if (nwords == 3) {
                record_gl_census(ZB_GL_HC_glTexParameteri, 4096u, 250000u);
                backend_.glTexParameteri(w[pos], w[pos + 1], static_cast<GLint>(w[pos + 2]));
            }
            break;
        case ZB_GL_OP_BIND_BUFFER_BASE:
            if (nwords == 3) {
                record_gl_census(ZB_GL_HC_glBindBufferBase, 4096u, 250000u);
                backend_.glBindBufferBase(w[pos], w[pos + 1], w[pos + 2]);
                note_bind_buffer(w[pos], w[pos + 2]);
            }
            break;
        case ZB_GL_OP_BIND_BUFFER_RANGE:
            if (nwords == 5) {
                const auto offset = static_cast<GLintptr>(static_cast<std::int32_t>(w[pos + 3]));
                const auto size = static_cast<GLsizeiptr>(static_cast<std::int32_t>(w[pos + 4]));
                if (offset >= 0 && size >= 0) {
                    record_gl_census(ZB_GL_HC_glBindBufferRange, 4096u, 250000u);
                    backend_.glBindBufferRange(w[pos], w[pos + 1], w[pos + 2], offset, size);
                    note_bind_buffer(w[pos], w[pos + 2]);
                }
            }
            break;
        case ZB_GL_OP_UNIFORM_2I:
            if (nwords == 3) {
                record_gl_census(ZB_GL_HC_glUniform2i, 4096u, 250000u);
                backend_.glUniform2i(static_cast<GLint>(w[pos]), static_cast<GLint>(w[pos + 1]),
                                     static_cast<GLint>(w[pos + 2]));
            }
            break;
        case ZB_GL_OP_UNIFORM_2FV:
            if (nwords >= 2) {
                const auto cnt = static_cast<GLsizei>(w[pos + 1]);
                if (cnt >= 1 && cnt <= 64 && nwords == 2u + static_cast<std::uint32_t>(cnt) * 2u) {
                    record_gl_census(ZB_GL_HC_glUniform2fv, 4096u, 250000u);
                    backend_.glUniform2fv(static_cast<GLint>(w[pos]), cnt,
                                          reinterpret_cast<const GLfloat*>(w + pos + 2));
                }
            }
            break;
        case ZB_GL_OP_UNIFORM_MATRIX_3FV:
            if (nwords >= 3) {
                const auto cnt = static_cast<GLsizei>(w[pos + 1]);
                if (cnt >= 1 && cnt <= 16 && nwords == 3u + static_cast<std::uint32_t>(cnt) * 9u) {
                    record_gl_census(ZB_GL_HC_glUniformMatrix3fv, 4096u, 250000u);
                    backend_.glUniformMatrix3fv(static_cast<GLint>(w[pos]), cnt,
                                                static_cast<GLboolean>(w[pos + 2] != 0),
                                                reinterpret_cast<const GLfloat*>(w + pos + 3));
                }
            }
            break;
        case ZB_GL_OP_UNIFORM_MATRIX_4FV:
            if (nwords >= 3) {
                const auto cnt = static_cast<GLsizei>(w[pos + 1]);
                if (cnt >= 1 && cnt <= 16 && nwords == 3u + static_cast<std::uint32_t>(cnt) * 16u) {
                    record_gl_census(ZB_GL_HC_glUniformMatrix4fv, 4096u, 250000u);
                    backend_.glUniformMatrix4fv(static_cast<GLint>(w[pos]), cnt,
                                                static_cast<GLboolean>(w[pos + 2] != 0),
                                                reinterpret_cast<const GLfloat*>(w + pos + 3));
                }
            }
            break;
        default:
            break;
        }
        pos += nwords;
    }
    hdr->count = 0;
    hdr->used_words = 0;
}

bool HostGl::handle_fast_host_call(std::uint32_t index, GuestThread& thread) {
    if (!is_gl_host_call(index) || !gl_call_can_run_fast(*this, index, thread)) return false;
    if (__builtin_expect(gl_trace_enabled() || gl_diagnostics_enabled(), 0)) return false;
    if (__builtin_expect(!egl_context_checked_.load(std::memory_order_relaxed), 0)) return false;
    const std::uint64_t egl_generation = runtime_report().egl_current_generation();
    if (__builtin_expect(!thread_generation_matches(egl_generation), 0)) {
        return false;
    }

    const std::uint64_t host_enter_ticks = frame_probe_call_now_ticks();
    flush_batch(thread);
    const char* name = gl_host_call_name(index);
    record_gl_census(index, 4096u, 250000u);
    if (state_filter_enabled_ && try_skip_redundant_call(index, thread, egl_generation)) {
        thread.regs()[0] = 0;
        thread.regs()[1] = 0;
        frame_probe_host_call_ticks(name, frame_probe_call_now_ticks() - host_enter_ticks);
        return true;
    }
    Call call(*this, thread, index);
    if (dispatch(call)) {
        if (state_filter_enabled_) note_after_dispatch(index, call, egl_generation);
        frame_probe_host_call_ticks(name, frame_probe_call_now_ticks() - host_enter_ticks);
        return true;
    }
    log("GLES host call index %u has no generated handler", index);
    std::abort();
}

bool HostGl::handle_host_call(std::uint32_t index, GuestThread& thread) {
    const bool gl1 = is_gl1_host_call(index);
    if (!gl1 && !is_gl_host_call(index)) return false;
    const std::uint64_t host_enter_ticks = frame_probe_call_now_ticks();
    flush_batch(thread);
    const char* name = gl_profile_name(index);
    if (__builtin_expect(gl_trace_enabled(), 0)) {
        log("GLES trace: %s(0x%x, 0x%x, 0x%x, 0x%x)", name, thread.regs()[0], thread.regs()[1],
            thread.regs()[2], thread.regs()[3]);
    }
    thread_local std::uint64_t t_host_tid = 0;
    if (__builtin_expect(t_host_tid == 0, 0)) t_host_tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
    const std::uint64_t host_tid = t_host_tid;
    runtime_report().note_gl_call(name, host_tid);
    record_gl_census(index, 1000u, 100000u);
    const std::uint64_t egl_generation = runtime_report().egl_current_generation();
    if (__builtin_expect(check_and_update_thread_generation(egl_generation), 0)) {
        runtime_report().note_gl_thread(host_tid);
    }
    if (__builtin_expect(!egl_context_checked_.load(std::memory_order_relaxed), 0)) {
        if (!egl_context_checked_.exchange(true, std::memory_order_relaxed) && egl_context_probe_) {
            runtime_report().note_gl_egl_context(egl_context() != 0);
        }
    }
    if (state_filter_enabled_ && !gl1 && !gl_diagnostics_enabled() &&
        try_skip_redundant_call(index, thread, egl_generation)) {
        thread.regs()[0] = 0;
        thread.regs()[1] = 0;
        frame_probe_host_call_ticks(name, frame_probe_call_now_ticks() - host_enter_ticks);
        return true;
    }
    Call call(*this, thread, index);
    if (!gl1 && __builtin_expect(gl_diagnostics_enabled(), 0)) gl_diagnose_before(*this, call);
    if (gl1 ? dispatch_gl1(call) : dispatch(call)) {
        if (state_filter_enabled_ && !gl1) note_after_dispatch(index, call, egl_generation);
        // Visibility diagnostics query the driver, so they run only once the Android runtime
        // enables them; mock-backend tests keep exact call logs.
        if (!gl1 && call.valid() && __builtin_expect(gl_diagnostics_enabled(), 0)) gl_diagnose(*this, call);
        if (call.valid() && index == ZB_GL_HC_glGetError) {
            const std::uint32_t error = thread.regs()[0];
            if (error != 0) runtime_report().note_gl_error(name, error);
        }
        frame_probe_host_call_ticks(name, frame_probe_call_now_ticks() - host_enter_ticks);
        return true;
    }
    log("GLES host call index %u has no generated handler", index);
    std::abort();
}

}  // namespace zb
