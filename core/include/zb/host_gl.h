#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "zb/gl_backend.h"
#include "zb/guest_thread.h"
#include "zb/library_runtime.h"

namespace zb {

// GLES 2.0 + 3.0 host-call dispatcher. Guest code passes AAPCS32 words in r0-r3 and on its stack;
// HostGl translates those words to the portable GlBackend seam after Dynarmic has stopped.
// GL visibility diagnostics in the runtime report (gl_diagnostics.cpp). Off by default: they
// add driver queries to the call stream. The Android runtime turns them on.
void enable_gl_diagnostics();
bool gl_diagnostics_enabled();

class HostGl {
public:
    using GuestAllocator = std::function<std::optional<std::uint32_t>(std::size_t)>;
    class Call {
    public:
        Call(HostGl& host, GuestThread& thread, std::uint32_t index);

        std::uint32_t index() const { return index_; }
        std::uint32_t arg(unsigned position);
        bool valid() const { return valid_; }

        template <typename T>
        T scalar(unsigned position) {
            const std::uint32_t word = arg(position);
            if constexpr (std::is_floating_point_v<T>) {
                static_assert(sizeof(T) == sizeof(word));
                return std::bit_cast<T>(word);
            } else if constexpr (std::is_signed_v<T>) {
                const std::int32_t signed_word = std::bit_cast<std::int32_t>(word);
                return static_cast<T>(signed_word);
            } else {
                return static_cast<T>(word);
            }
        }

        template <typename T>
        void set_result(T value) {
            static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
            thread_.regs()[0] = static_cast<std::uint32_t>(value);
        }

        template <typename T>
        std::uint64_t length(T value, std::uint64_t multiplier = 1) {
            static_assert(std::is_integral_v<T>);
            if constexpr (std::is_signed_v<T>) {
                if (value < 0) {
                    fail(kGlInvalidValue, "array length is negative");
                    return 0;
                }
            }
            const std::uint64_t unsigned_value = static_cast<std::uint64_t>(value);
            if (multiplier != 0 &&
                unsigned_value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
                fail(kGlInvalidValue, "array length overflowed");
                return 0;
            }
            return unsigned_value * multiplier;
        }

        template <typename T>
        T* pointer(unsigned position, std::uint64_t elements, std::uint8_t need) {
            const std::uint32_t address = arg(position);
            constexpr std::uint64_t element_size = [] {
                if constexpr (std::is_void_v<std::remove_cv_t<T>>) {
                    return std::uint64_t{1};
                } else {
                    return static_cast<std::uint64_t>(sizeof(T));
                }
            }();
            if (elements > kGuestSpaceSize / element_size) {
                fail(kGlInvalidValue, "array byte size exceeds the guest address space");
                return nullptr;
            }
            const std::uint64_t bytes = elements * element_size;
            if (address == 0) return nullptr;
            std::uint8_t* host = host_.runtime().memory().host_ptr(address, bytes, need);
            if (host == nullptr) {
                fail(kGlInvalidValue, "array is outside accessible guest memory");
                return nullptr;
            }
            return reinterpret_cast<T*>(host);
        }

        void fail(GLenum error, const char* reason);

    private:
        HostGl& host_;
        GuestThread& thread_;
        std::uint32_t index_;
        std::uint32_t regs_[4];
        bool valid_ = true;
    };

    // Returns the current host EGLContext as an opaque integer. Called once for the top-level
    // report check and by bounded mapped-buffer diagnostics. Left empty on the host (no EGL);
    // the Android wiring supplies it.
    using EglContextProbe = std::function<std::uintptr_t()>;

    struct MirrorBlock {
        std::uint32_t address = 0;
        std::size_t capacity = 0;
    };
    struct MirrorPoolState {
        std::mutex mutex;
        std::vector<MirrorBlock> blocks;
        std::size_t bytes = 0;
        std::atomic<std::size_t> bytes_approx{0};
        std::atomic<bool> alive{true};
    };
    static constexpr std::size_t kMaxMirrorPoolBytes = 16u << 20;
    static constexpr std::size_t kMaxMirrorPoolEntries = 64;

    HostGl(LibraryRuntime& runtime, GlBackend& backend, GuestAllocator allocator = {},
           EglContextProbe egl_context_probe = {})
        : id_(next_id()), runtime_(runtime), backend_(backend), allocator_(std::move(allocator)),
          egl_context_probe_(std::move(egl_context_probe)),
          mirror_pool_state_(std::make_shared<MirrorPoolState>()) {}
    ~HostGl() {
        if (mirror_pool_state_) mirror_pool_state_->alive.store(false, std::memory_order_release);
    }

    std::uint64_t id() const { return id_; }
    const std::shared_ptr<MirrorPoolState>& mirror_pool_state() const { return mirror_pool_state_; }

    // Serves the two GLES ranges (2.0 at 0-141, 3.0 at 228-331) and leaves 142-160 for
    // HostAssets, 160-167 for HostNativeWindow and 168-211 for HostEgl.
    bool handle_host_call(std::uint32_t index, GuestThread& thread);
    // Synchronous in-JIT fast path for pure-leaf GL calls that never invoke call_on_current.
    // Returns false for calls needing guest allocation/freeing, trace/diagnostics, or EGL epoch sync.
    bool handle_fast_host_call(std::uint32_t index, GuestThread& thread);
    void flush_batch(GuestThread& thread);
    void apply_batched_vertex_attrib_pointer(GLuint index, GLint size, GLenum type,
                                             GLboolean normalized, GLsizei stride,
                                             std::uint32_t pointer);
    void apply_batched_draw_arrays(GuestThread& thread, GLenum mode, GLint first, GLsizei count);
    void apply_batched_draw_elements(GuestThread& thread, GLenum mode, GLsizei count, GLenum type,
                                     std::uint32_t guest_indices);
    void set_state_filter_enabled(bool enabled) { state_filter_enabled_ = enabled; }
    bool state_filter_enabled() const { return state_filter_enabled_; }
    // Drops cached host-side filter assumptions after a guest batch directly updated the driver.
    void invalidate_state_filter();
    LibraryRuntime& runtime() { return runtime_; }
    GlBackend& backend() { return backend_; }
    std::uintptr_t egl_context() const { return egl_context_probe_ ? egl_context_probe_() : 0; }
    void reject(Call& call, GLenum error, const char* reason);
    void note_pixel_store(GLenum pname, GLint param);
    void note_bind_buffer(GLenum target, GLuint buffer);
    void note_delete_buffers(GLsizei n, const GLuint* buffers);
    void note_bind_vertex_array(GLuint array);
    void note_delete_vertex_arrays(GLsizei n, const GLuint* arrays);
    bool buffer_binding(GLenum target, GLuint& buffer) const;
    // The GLES 3.0 pixel buffer bound to GL_PIXEL_PACK_BUFFER / GL_PIXEL_UNPACK_BUFFER, if any:
    // while one is bound, a pixel "pointer" argument is a buffer offset, not a guest address.
    GLuint pixel_buffer(bool pack) const;
    void note_vertex_attrib_enabled(GLuint index, bool enabled);
    void invalidate_uniforms(GLuint program);
    GLint pixel_alignment(bool pack) const;
    std::optional<std::uint64_t> uniform_elements(GLuint program, GLint location);
    std::optional<std::uint32_t> allocate_guest(std::size_t size);
    // Releases an allocate_guest() block. A HostGl built with a test GuestAllocator has no
    // matching deallocator, so the block is simply left to that allocator's arena.
    void free_guest(std::uint32_t address);
    // Acquires/releases a guest buffer mirror, reusing pooled blocks when running without a test allocator.
    std::optional<std::uint32_t> acquire_mirror(std::size_t size, std::size_t& capacity_out);
    void release_mirror(std::uint32_t address, std::size_t capacity);
    // True when glMapBufferRange / glUnmapBuffer can stay entirely inside the thread-local mirror pool
    // without allocating or freeing guest memory via call_on_current.
    bool can_fast_map_buffer_range(GuestThread& thread) const;
    bool can_fast_unmap_buffer(GuestThread& thread) const;
    bool multi_thread_gl_active() const {
        return gl_threads_seen_.load(std::memory_order_relaxed) > 1;
    }

private:
    static std::uint64_t next_id() {
        static std::atomic<std::uint64_t> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    bool try_skip_redundant_call(std::uint32_t index, GuestThread& thread, std::uint64_t egl_generation);
    void note_after_dispatch(std::uint32_t index, Call& call, std::uint64_t egl_generation);
    bool dispatch(Call& call);

    std::uint64_t id_;
    LibraryRuntime& runtime_;
    GlBackend& backend_;
    GuestAllocator allocator_;
    EglContextProbe egl_context_probe_;
    std::atomic<bool> egl_context_checked_{false};
    bool state_filter_enabled_ = false;
    std::shared_ptr<MirrorPoolState> mirror_pool_state_;
    bool thread_generation_matches(std::uint64_t egl_generation) const;
    bool check_and_update_thread_generation(std::uint64_t egl_generation);

    // The last runtime_report().egl_current_generation() is tracked per-thread in GlThreadState;
    // gl_threads_seen_ counts distinct host threads that have dispatched GL calls so cross-thread
    // cache epoch invalidation activates only when multiple GL threads exist.
    std::atomic<std::uint32_t> gl_threads_seen_{0};
};

std::uint64_t gl_pname_count(GlBackend& backend, GLenum pname);
std::optional<std::uint64_t> gl_pixel_bytes(GLenum format, GLenum type, GLsizei width,
                                            GLsizei height, GLint alignment);

}  // namespace zb
