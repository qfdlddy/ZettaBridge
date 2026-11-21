#include "zb/host_gl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gl/frame_probe.h"
#include "gl/gl_diagnostics.h"
#include "zb/gl_hostcalls.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

constexpr GLenum kGlPackAlignment = 0x0D05;
constexpr GLenum kGlUnpackAlignment = 0x0CF5;
constexpr GLenum kGlActiveUniforms = 0x8B86;
constexpr GLenum kGlActiveUniformMaxLength = 0x8B87;
constexpr GLenum kGlNumCompressedTextureFormats = 0x86A2;
constexpr GLenum kGlCompressedTextureFormats = 0x86A3;
constexpr GLenum kGlNumShaderBinaryFormats = 0x8DF9;
constexpr GLenum kGlShaderBinaryFormats = 0x8DF8;
constexpr GLenum kGlArrayBuffer = 0x8892;
constexpr GLenum kGlElementArrayBuffer = 0x8893;
constexpr GLenum kGlPixelPackBuffer = 0x88EB;
constexpr GLenum kGlPixelUnpackBuffer = 0x88EC;
constexpr GLenum kGlBufferMapPointer = 0x88BD;
constexpr GLenum kGlUniformBlockActiveUniforms = 0x8A42;
constexpr GLenum kGlUniformBlockActiveUniformIndices = 0x8A43;
// glMapBufferRange access bits.
constexpr GLbitfield kGlMapRead = 0x0001;
constexpr GLbitfield kGlMapWrite = 0x0002;
constexpr GLbitfield kGlMapInvalidateRange = 0x0004;
constexpr GLbitfield kGlMapInvalidateBuffer = 0x0008;
constexpr GLbitfield kGlMapFlushExplicit = 0x0010;

struct BufferMapping {
    std::uint32_t guest = 0;
    std::uint8_t* data = nullptr;
    std::uint64_t length = 0;
    std::size_t capacity = 0;
    GLbitfield access = 0;
    GLintptr offset = 0;
    bool shadow_only = false;
    GlMapDiagnostic diagnostic;
};

struct GlThreadState {
    struct Attribute {
        bool defined = false;
        bool enabled = false;
        GLint size = 4;
        GLenum type = 0x1406;
        GLboolean normalized = 0;
        GLsizei stride = 0;
        std::uint32_t guest_pointer = 0;
        GLuint buffer = 0;
        // GLES 3.0 glVertexAttribIPointer: the attribute must be re-issued through the integer
        // entry point when its client array is materialized.
        bool integer = false;
    };

    struct LocalMapping {
        std::uint64_t key = 0;
        BufferMapping mapping;
    };

    struct ThreadMirrorBlock {
        std::uint32_t address = 0;
        std::size_t capacity = 0;
    };

    std::uint64_t owner_id = 0;
    std::weak_ptr<HostGl::MirrorPoolState> owner_pool;
    std::array<LocalMapping, 16> local_mappings{};
    std::size_t local_mapping_count = 0;
    std::vector<ThreadMirrorBlock> local_mirror_pool;
    std::size_t local_mirror_pool_bytes = 0;

    void recycle_to_pool() {
        if (local_mirror_pool.empty()) return;
        if (const auto pool = owner_pool.lock()) {
            if (pool->alive.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(pool->mutex);
                for (const auto& block : local_mirror_pool) {
                    if (pool->blocks.size() >= HostGl::kMaxMirrorPoolEntries ||
                        pool->bytes + block.capacity > HostGl::kMaxMirrorPoolBytes) {
                        break;
                    }
                    pool->blocks.push_back(HostGl::MirrorBlock{block.address, block.capacity});
                    pool->bytes += block.capacity;
                }
                pool->bytes_approx.store(pool->bytes, std::memory_order_relaxed);
            }
        }
        local_mirror_pool.clear();
        local_mirror_pool_bytes = 0;
    }

    ~GlThreadState() { recycle_to_pool(); }
    GLint pack_alignment = 4;
    GLint unpack_alignment = 4;
    GLuint array_buffer = 0;
    GLuint element_array_buffer = 0;
    GLuint pixel_pack_buffer = 0;
    GLuint pixel_unpack_buffer = 0;
    std::array<GLuint, 8> bound_buffers{};
    std::uint8_t known_bindings = 0;
    static constexpr std::size_t kFastAttributeCount = 16;
    static constexpr std::size_t kCachedTextureUnits = 32;
    static constexpr std::size_t kCachedTextureTargets = 4;
    static constexpr std::size_t kCachedScalarStates = 11;

    struct ScalarStateEntry {
        std::array<std::uint32_t, 4> words{};
        bool known = false;
    };

    std::uint64_t filter_egl_generation = UINT64_MAX;
    std::uint64_t strings_egl_generation = UINT64_MAX;
    std::uint64_t sampled_egl_generation = UINT64_MAX;
    bool active_texture_known = false;
    GLenum active_texture = 0x84C0;
    bool current_program_known = false;
    GLuint current_program = 0;
    bool current_vao_known = false;
    std::uint32_t cap_known = 0;
    std::uint32_t cap_value = 0;
    std::array<ScalarStateEntry, kCachedScalarStates> scalar_states{};
    std::array<std::array<GLuint, kCachedTextureTargets>, kCachedTextureUnits> bound_textures{};
    std::array<std::uint8_t, kCachedTextureUnits> bound_texture_known{};

    void reset_dynamic_state_filter(std::uint64_t generation) {
        filter_egl_generation = generation;
        active_texture_known = false;
        current_program_known = false;
        current_vao_known = false;
        cap_known = 0;
        cap_value = 0;
        for (auto& entry : scalar_states) entry.known = false;
        bound_texture_known.fill(0);
        known_bindings = 0;
    }

    void ensure_strings_epoch(std::uint64_t generation) {
        if (strings_egl_generation == generation) return;
        strings_egl_generation = generation;
        strings.clear();
        indexed_strings.clear();
        extensions.clear();
        extensions_known = false;
    }

    struct VaoAttributes {
        std::array<Attribute, kFastAttributeCount> fast_attributes{};
        std::unordered_map<GLuint, Attribute> overflow_attributes;
        std::uint16_t fast_client_mask = 0;
        std::uint32_t overflow_client_count = 0;
    };

    GLuint current_vao = 0;
    std::unordered_map<GLuint, GLuint> vao_element_buffers;
    std::unordered_map<GLuint, VaoAttributes> vao_attributes;
    std::array<Attribute, kFastAttributeCount> fast_attributes{};
    std::unordered_map<GLuint, Attribute> overflow_attributes;
    std::uint16_t fast_client_mask = 0;
    std::uint32_t overflow_client_count = 0;
    std::unordered_map<GLuint, std::unordered_map<GLint, std::uint64_t>> uniforms;
    std::unordered_map<GLenum, std::uint32_t> strings;
    std::unordered_map<std::uint64_t, std::uint32_t> indexed_strings;
    // The driver's extensions minus the ones this bridge cannot serve, in driver order.
    std::vector<std::string> extensions;
    bool extensions_known = false;

    void switch_vao_attributes(GLuint next_vao) {
        if (next_vao == current_vao) return;
        VaoAttributes& saved = vao_attributes[current_vao];
        saved.fast_attributes = fast_attributes;
        saved.overflow_attributes = std::move(overflow_attributes);
        saved.fast_client_mask = fast_client_mask;
        saved.overflow_client_count = overflow_client_count;
        current_vao = next_vao;
        const auto found = vao_attributes.find(next_vao);
        if (found != vao_attributes.end()) {
            fast_attributes = found->second.fast_attributes;
            overflow_attributes = found->second.overflow_attributes;
            fast_client_mask = found->second.fast_client_mask;
            overflow_client_count = found->second.overflow_client_count;
        } else {
            fast_attributes = {};
            overflow_attributes.clear();
            fast_client_mask = 0;
            overflow_client_count = 0;
        }
    }

    static bool is_client_array(const Attribute& attr) {
        return attr.enabled && attr.defined && attr.buffer == 0;
    }

    Attribute& attribute_mut(GLuint index) {
        if (index < kFastAttributeCount) return fast_attributes[index];
        return overflow_attributes[index];
    }

    const Attribute* find_attribute(GLuint index) const {
        if (index < kFastAttributeCount) return &fast_attributes[index];
        const auto found = overflow_attributes.find(index);
        return found == overflow_attributes.end() ? nullptr : &found->second;
    }

    void refresh_client_flag(GLuint index, const Attribute& attr) {
        const bool active = is_client_array(attr);
        if (index < kFastAttributeCount) {
            const std::uint16_t bit = static_cast<std::uint16_t>(1u << index);
            if (active) {
                fast_client_mask |= bit;
            } else {
                fast_client_mask &= static_cast<std::uint16_t>(~bit);
            }
            return;
        }
        std::uint32_t count = 0;
        for (const auto& [_, entry] : overflow_attributes) {
            if (is_client_array(entry)) ++count;
        }
        overflow_client_count = count;
    }
};

constexpr GLenum kGlVersion = 0x1F02;
constexpr GLenum kGlExtensions = 0x1F03;
constexpr GLenum kGlMajorVersion = 0x821B;
constexpr GLenum kGlMinorVersion = 0x821C;
constexpr GLenum kGlNumExtensions = 0x821D;
constexpr GLenum kGlShadingLanguageVersion = 0x8B8C;

// Extensions whose entry points this bridge does not serve. A driver advertises what the hardware
// can do, and an engine that reads an extension out of the string calls its functions without
// checking what eglGetProcAddress returned: Unity jumps straight into a null pointer that way.
// Dropping them from the string is what a driver without the extension already looks like.
constexpr std::string_view kUnsupportedExtensions[] = {
#define ZB_GL_UNSUPPORTED_EXTENSION(name) name,
#include "gen/gl_unsupported_extensions.inc"
#undef ZB_GL_UNSUPPORTED_EXTENSION
};

bool extension_served(std::string_view name) {
    // The generated list is sorted, so the lookup is a binary search.
    return !std::binary_search(std::begin(kUnsupportedExtensions), std::end(kUnsupportedExtensions), name);
}

thread_local GlThreadState t_state;

GlThreadState& state(const HostGl& host) {
    if (t_state.owner_id != host.id()) {
        t_state.recycle_to_pool();
        t_state = {};
        t_state.owner_id = host.id();
        t_state.owner_pool = host.mirror_pool_state();
    }
    return t_state;
}

// The driver's extensions this bridge can serve, in the order the driver lists them. GLES 3.0
// enumerates them one by one; a GLES 2.0 context has only the single space-separated string.
const std::vector<std::string>& served_extensions(HostGl& host) {
    GlThreadState& current = state(host);
    const std::uint64_t egl_gen = runtime_report().egl_current_generation();
    current.ensure_strings_epoch(egl_gen);
    if (current.extensions_known) return current.extensions;

    GLint count = 0;
    host.backend().glGetIntegerv(kGlNumExtensions, &count);
    if (count > 0) {
        current.extensions_known = true;
        for (GLint i = 0; i < count; ++i) {
            const GLubyte* entry = host.backend().glGetStringi(kGlExtensions, static_cast<GLuint>(i));
            if (entry == nullptr) continue;
            const std::string name(reinterpret_cast<const char*>(entry));
            if (extension_served(name)) current.extensions.push_back(name);
        }
        return current.extensions;
    }
    const GLubyte* all = host.backend().glGetString(kGlExtensions);
    if (all == nullptr) return current.extensions;
    current.extensions_known = true;
    std::string_view rest(reinterpret_cast<const char*>(all));
    while (!rest.empty()) {
        const std::size_t space = rest.find(' ');
        const std::string_view name = rest.substr(0, space);
        if (!name.empty() && extension_served(name)) current.extensions.emplace_back(name);
        if (space == std::string_view::npos) break;
        rest.remove_prefix(space + 1);
    }
    return current.extensions;
}

// Copies a host string into guest memory and answers the call with its guest address.
std::uint32_t answer_with_string(HostGl& host, HostGl::Call& call, std::string_view text) {
    const auto address = host.allocate_guest(text.size() + 1);
    std::uint8_t* destination =
        address ? host.runtime().memory().host_ptr(*address, text.size() + 1, kPageRead | kPageWrite) : nullptr;
    if (destination == nullptr) {
        host.reject(call, kGlOutOfMemory, "guest allocation for driver string failed");
        return 0;
    }
    std::memcpy(destination, text.data(), text.size());
    destination[text.size()] = 0;
    call.set_result(*address);
    return *address;
}


std::uint64_t uniform_components(GLenum type) {
    switch (type) {
    case 0x1404:  // GL_INT
    case 0x1405:  // GL_UNSIGNED_INT
    case 0x1406:  // GL_FLOAT
    case 0x8B56:  // GL_BOOL
    case 0x8B5E:  // GL_SAMPLER_2D
    case 0x8B5F:  // GL_SAMPLER_3D
    case 0x8B60:  // GL_SAMPLER_CUBE
    case 0x8B62:  // GL_SAMPLER_2D_SHADOW
    case 0x8DC1:  // GL_SAMPLER_2D_ARRAY
    case 0x8DC4:  // GL_SAMPLER_2D_ARRAY_SHADOW
    case 0x8DC5:  // GL_SAMPLER_CUBE_SHADOW
    case 0x8DCA:  // GL_INT_SAMPLER_2D
    case 0x8DCB:  // GL_INT_SAMPLER_3D
    case 0x8DCC:  // GL_INT_SAMPLER_CUBE
    case 0x8DCF:  // GL_INT_SAMPLER_2D_ARRAY
    case 0x8DD2:  // GL_UNSIGNED_INT_SAMPLER_2D
    case 0x8DD3:  // GL_UNSIGNED_INT_SAMPLER_3D
    case 0x8DD4:  // GL_UNSIGNED_INT_SAMPLER_CUBE
    case 0x8DD7:  // GL_UNSIGNED_INT_SAMPLER_2D_ARRAY
        return 1;
    case 0x8B50:  // GL_FLOAT_VEC2
    case 0x8B53:  // GL_INT_VEC2
    case 0x8B57:  // GL_BOOL_VEC2
    case 0x8DC6:  // GL_UNSIGNED_INT_VEC2
        return 2;
    case 0x8B51:  // GL_FLOAT_VEC3
    case 0x8B54:  // GL_INT_VEC3
    case 0x8B58:  // GL_BOOL_VEC3
    case 0x8DC7:  // GL_UNSIGNED_INT_VEC3
        return 3;
    case 0x8B52:  // GL_FLOAT_VEC4
    case 0x8B55:  // GL_INT_VEC4
    case 0x8B59:  // GL_BOOL_VEC4
    case 0x8DC8:  // GL_UNSIGNED_INT_VEC4
    case 0x8B5A:  // GL_FLOAT_MAT2
        return 4;
    case 0x8B65:  // GL_FLOAT_MAT2x3
    case 0x8B67:  // GL_FLOAT_MAT3x2
        return 6;
    case 0x8B66:  // GL_FLOAT_MAT2x4
    case 0x8B69:  // GL_FLOAT_MAT4x2
        return 8;
    case 0x8B5B:  // GL_FLOAT_MAT3
        return 9;
    case 0x8B68:  // GL_FLOAT_MAT3x4
    case 0x8B6A:  // GL_FLOAT_MAT4x3
        return 12;
    case 0x8B5C:  // GL_FLOAT_MAT4
        return 16;
    default:
        return 0;
    }
}

bool checked_multiply(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) return false;
    result = a * b;
    return true;
}

// Clamps an "<prefix><major>.<minor>[...]" GL/GLSL version string down to replacement (ES 3.0 /
// GLSL ES 3.00) because this bridge only serves GLES 2.0 + 3.0 entry points; advertising 3.1+
// causes engines like Unity to call unbridged core functions such as glGetProgramInterfaceiv
// without null checking.
std::optional<std::string> clamp_version_prefix(std::string_view text, std::string_view prefix,
                                                std::string_view replacement) {
    if (!text.starts_with(prefix)) return std::nullopt;
    std::string_view rest = text.substr(prefix.size());
    std::size_t pos = 0;
    unsigned major = 0;
    while (pos < rest.size() && rest[pos] >= '0' && rest[pos] <= '9') {
        major = major * 10u + static_cast<unsigned>(rest[pos] - '0');
        ++pos;
    }
    if (pos == 0 || pos >= rest.size() || rest[pos] != '.') return std::nullopt;
    ++pos;
    const std::size_t minor_start = pos;
    unsigned minor = 0;
    while (pos < rest.size() && rest[pos] >= '0' && rest[pos] <= '9') {
        minor = minor * 10u + static_cast<unsigned>(rest[pos] - '0');
        ++pos;
    }
    if (pos == minor_start) return std::nullopt;
    if (pos < rest.size() && rest[pos] != ' ' && rest[pos] != '\t') return std::nullopt;
    if (major < 3u || (major == 3u && minor == 0u)) return std::nullopt;
    std::string out(replacement);
    out.append(rest.substr(pos));
    return out;
}

std::optional<std::string> clamp_gl_version_string(GLenum name, std::string_view text) {
    if (name == kGlVersion) return clamp_version_prefix(text, "OpenGL ES ", "OpenGL ES 3.0");
    if (name == kGlShadingLanguageVersion) {
        return clamp_version_prefix(text, "OpenGL ES GLSL ES ", "OpenGL ES GLSL ES 3.00");
    }
    return std::nullopt;
}

template <typename T>
bool serve_pname(HostGl& host, HostGl::Call& call, void (GlBackend::*function)(GLenum, T*)) {
    const GLenum pname = call.scalar<GLenum>(0);
    if (pname == kGlNumExtensions) {
        // Must match what glGetStringi hands out, or the guest reads past the filtered list.
        T* data = call.pointer<T>(1, 1, kPageRead | kPageWrite);
        if (!call.valid()) return true;
        *data = static_cast<T>(served_extensions(host).size());
        return true;
    }
    const std::uint64_t count = gl_pname_count(host.backend(), pname);
    T* data = call.pointer<T>(1, count, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    (host.backend().*function)(pname, data);
    if (pname == kGlMajorVersion && *data > static_cast<T>(3)) {
        *data = static_cast<T>(3);
    } else if (pname == kGlMinorVersion && *data > static_cast<T>(0)) {
        GLint major = 0;
        host.backend().glGetIntegerv(kGlMajorVersion, &major);
        if (major >= 3) *data = static_cast<T>(0);
    }
    return true;
}

// The pixel argument of a texture or read-back call. With a GLES 3.0 pixel buffer object bound
// the argument is a byte offset into that buffer and is passed through untouched; otherwise it is
// a guest address whose whole image (depth images of height padded rows) must be accessible.
bool pixel_pointer(HostGl& host, HostGl::Call& call, bool pack, GLenum format, GLenum type,
                   GLsizei width, GLsizei height, GLsizei depth, GLint alignment,
                   unsigned position, std::uint8_t permission, void*& out) {
    if (host.pixel_buffer(pack) != 0) {
        out = reinterpret_cast<void*>(static_cast<std::uintptr_t>(call.arg(position)));
        return call.valid();
    }
    if (call.arg(position) == 0) {
        // Null pixels pointer (e.g. glTexImage2D allocating storage without initial upload)
        // never dereferences guest memory.
        out = nullptr;
        return call.valid();
    }
    if (depth < 0) {
        call.fail(kGlInvalidValue, "image depth is negative");
        return false;
    }
    std::uint64_t rows = 0;
    if (!checked_multiply(static_cast<std::uint64_t>(height), static_cast<std::uint64_t>(depth),
                          rows) ||
        rows > static_cast<std::uint64_t>(std::numeric_limits<GLsizei>::max())) {
        call.fail(kGlInvalidValue, "image dimensions overflowed");
        return false;
    }
    // Images are contiguous, so depth images of height padded rows measure exactly as one
    // height*depth tall image: only the very last row is unpadded.
    const auto bytes = gl_pixel_bytes(format, type, width, static_cast<GLsizei>(rows), alignment);
    if (!bytes) {
        // Naming the pair is the whole point: a rejected upload is a texture that stays black,
        // and which format and type were refused decides what to add. Orange Roulette's black
        // screen was one such pair (GL_BGRA_EXT), found the same way.
        char reason[128];
        std::snprintf(reason, sizeof reason,
                      "pixel format 0x%x type 0x%x %dx%d alignment %d is not accepted",
                      static_cast<unsigned>(format), static_cast<unsigned>(type), width,
                      static_cast<int>(rows), static_cast<int>(alignment));
        call.fail(kGlInvalidValue, reason);
        return false;
    }
    out = call.pointer<void>(position, *bytes, permission);
    return call.valid();
}

const GLchar* guest_string(HostGl& host, HostGl::Call& call, std::uint32_t address) {
    constexpr std::uint64_t kMaxGuestString = 64u << 20;
    if (address == 0) {
        call.fail(kGlInvalidValue, "string pointer is null");
        return nullptr;
    }
    std::uint64_t scanned = 0;
    while (scanned < kMaxGuestString) {
        const std::uint64_t current = static_cast<std::uint64_t>(address) + scanned;
        if (current >= kGuestSpaceSize) break;
        const std::uint64_t page_left = kPageSize - (current & kPageMask);
        const std::uint64_t chunk = std::min({page_left, kMaxGuestString - scanned,
                                              kGuestSpaceSize - current});
        const std::uint8_t* data = host.runtime().memory().host_ptr(
            static_cast<std::uint32_t>(current), chunk, kPageRead);
        if (data == nullptr) break;
        if (std::memchr(data, 0, static_cast<std::size_t>(chunk)) != nullptr) {
            return reinterpret_cast<const GLchar*>(host.runtime().memory().base() + address);
        }
        scanned += chunk;
    }
    call.fail(kGlInvalidValue, "string is unreadable or not terminated within 64 MiB");
    return nullptr;
}

std::uint64_t attribute_component_bytes(GLenum type) {
    switch (type) {
    case 0x1400:  // GL_BYTE
    case 0x1401:  // GL_UNSIGNED_BYTE
        return 1;
    case 0x1402:  // GL_SHORT
    case 0x1403:  // GL_UNSIGNED_SHORT
        return 2;
    case 0x140B:  // GL_HALF_FLOAT
        return 2;
    case 0x1404:  // GL_INT
    case 0x1405:  // GL_UNSIGNED_INT
    case 0x1406:  // GL_FLOAT
    case 0x140C:  // GL_FIXED
        return 4;
    default:
        return 0;
    }
}

// The byte width of one index of a glDrawElements-family type, or 0 when the type is invalid.
std::uint64_t element_index_bytes(GLenum type) {
    switch (type) {
    case 0x1401:  // GL_UNSIGNED_BYTE
        return 1;
    case 0x1403:  // GL_UNSIGNED_SHORT
        return 2;
    case 0x1405:  // GL_UNSIGNED_INT (GLES 3.0)
        return 4;
    default:
        return 0;
    }
}

bool materialize_client_arrays(HostGl& host, HostGl::Call& call, std::uint64_t first,
                               std::uint64_t last) {
    struct MaterializedAttribute {
        GLuint index;
        const GlThreadState::Attribute* attribute;
        const void* pointer;
    };

    GlThreadState& current = state(host);
    // Common GLES3 VBO path: zero client arrays enabled, skip scanning attributes completely.
    if (current.fast_client_mask == 0 && current.overflow_client_count == 0) return true;

    std::array<MaterializedAttribute, GlThreadState::kFastAttributeCount> stack_items{};
    std::size_t stack_count = 0;
    std::vector<MaterializedAttribute> heap_items;
    const auto collect = [&](GLuint index, const GlThreadState::Attribute& attribute) -> bool {
        const std::uint64_t component = attribute_component_bytes(attribute.type);
        if (attribute.size < 1 || attribute.size > 4 || component == 0 || attribute.stride < 0) {
            call.fail(kGlInvalidOperation, "client vertex attribute layout is invalid");
            return false;
        }
        const std::uint64_t element = static_cast<std::uint64_t>(attribute.size) * component;
        const std::uint64_t stride = attribute.stride == 0
                                         ? element
                                         : static_cast<std::uint64_t>(attribute.stride);
        std::uint64_t first_offset = 0;
        std::uint64_t last_offset = 0;
        if (!checked_multiply(first, stride, first_offset) ||
            !checked_multiply(last, stride, last_offset) ||
            last_offset > std::numeric_limits<std::uint64_t>::max() - element) {
            call.fail(kGlInvalidOperation, "client vertex attribute range overflowed");
            return false;
        }
        const std::uint64_t range_start =
            static_cast<std::uint64_t>(attribute.guest_pointer) + first_offset;
        const std::uint64_t range_end =
            static_cast<std::uint64_t>(attribute.guest_pointer) + last_offset + element;
        if (range_start >= kGuestSpaceSize || range_end > kGuestSpaceSize ||
            range_end < range_start ||
            host.runtime().memory().host_ptr(static_cast<std::uint32_t>(range_start),
                                             range_end - range_start, kPageRead) == nullptr) {
            call.fail(kGlInvalidOperation, "client vertex attribute range is unreadable");
            return false;
        }
        const void* pointer = host.runtime().memory().base() + attribute.guest_pointer;
        if (current.overflow_client_count == 0 && stack_count < stack_items.size()) {
            stack_items[stack_count++] = {index, &attribute, pointer};
        } else {
            if (heap_items.empty() && stack_count > 0) {
                heap_items.assign(stack_items.begin(), stack_items.begin() + stack_count);
                stack_count = 0;
            }
            heap_items.push_back({index, &attribute, pointer});
        }
        return true;
    };

    std::uint16_t mask = current.fast_client_mask;
    while (mask != 0) {
        const auto bit = static_cast<GLuint>(std::countr_zero(mask));
        mask &= static_cast<std::uint16_t>(mask - 1);
        if (!collect(bit, current.fast_attributes[bit])) return false;
    }
    if (current.overflow_client_count != 0) {
        for (const auto& [index, attribute] : current.overflow_attributes) {
            if (!GlThreadState::is_client_array(attribute)) continue;
            if (!collect(index, attribute)) return false;
        }
    }

    const MaterializedAttribute* items = heap_items.empty() ? stack_items.data() : heap_items.data();
    const std::size_t item_count = heap_items.empty() ? stack_count : heap_items.size();
    const bool rebound = item_count != 0 && current.array_buffer != 0;
    if (rebound) host.backend().glBindBuffer(kGlArrayBuffer, 0);
    for (std::size_t i = 0; i < item_count; ++i) {
        const auto& item = items[i];
        if (item.attribute->integer) {
            host.backend().glVertexAttribIPointer(item.index, item.attribute->size,
                                                  item.attribute->type, item.attribute->stride,
                                                  item.pointer);
        } else {
            host.backend().glVertexAttribPointer(
                item.index, item.attribute->size, item.attribute->type,
                item.attribute->normalized, item.attribute->stride, item.pointer);
        }
    }
    if (rebound) host.backend().glBindBuffer(kGlArrayBuffer, current.array_buffer);
    return true;
}

}  // namespace

std::uint64_t gl_pname_count(GlBackend& backend, GLenum pname) {
    switch (pname) {
    case 0x846D:  // GL_ALIASED_POINT_SIZE_RANGE
    case 0x846E:  // GL_ALIASED_LINE_WIDTH_RANGE
    case 0x0B70:  // GL_DEPTH_RANGE
    case 0x0D3A:  // GL_MAX_VIEWPORT_DIMS
        return 2;
    case 0x8005:  // GL_BLEND_COLOR
    case 0x0C22:  // GL_COLOR_CLEAR_VALUE
    case 0x0C23:  // GL_COLOR_WRITEMASK
    case 0x0C10:  // GL_SCISSOR_BOX
    case 0x0BA2:  // GL_VIEWPORT
        return 4;
    case 0x821B:  // GL_MAJOR_VERSION
    case 0x821C:  // GL_MINOR_VERSION
    case 0x821D:  // GL_NUM_EXTENSIONS
        return 1;
    // GLES 1.1 sizes. These pnames do not exist in GLES 2.0, so adding them cannot change an
    // ES2 result; without them glGetFloatv(GL_MODELVIEW_MATRIX) would validate four bytes and
    // let the driver write 64 into guest memory.
    case 0x0BA6:  // GL_MODELVIEW_MATRIX
    case 0x0BA7:  // GL_PROJECTION_MATRIX
    case 0x0BA8:  // GL_TEXTURE_MATRIX
        return 16;
    case 0x0B02:  // GL_CURRENT_NORMAL
        return 3;
    case 0x0B00:  // GL_CURRENT_COLOR
    case 0x0B03:  // GL_CURRENT_TEXTURE_COORDS
    case 0x0B53:  // GL_LIGHT_MODEL_AMBIENT
    case 0x0B66:  // GL_FOG_COLOR
        return 4;
    case 0x0B12:  // GL_POINT_SIZE_RANGE
    case 0x0B22:  // GL_SMOOTH_LINE_WIDTH_RANGE
        return 2;
    case kGlCompressedTextureFormats:
    case kGlShaderBinaryFormats:
    case 0x87FF: {  // GL_PROGRAM_BINARY_FORMATS
        GLint count = 0;
        const GLenum num_pname = pname == kGlCompressedTextureFormats ? kGlNumCompressedTextureFormats
                                 : pname == kGlShaderBinaryFormats    ? kGlNumShaderBinaryFormats
                                                                      : static_cast<GLenum>(0x87FE);
        backend.glGetIntegerv(num_pname, &count);
        return count > 0 ? static_cast<std::uint64_t>(count) : 0;
    }
    default: {
        static std::mutex mutex;
        static std::unordered_set<GLenum> logged;
        std::lock_guard<std::mutex> lock(mutex);
        if (logged.insert(pname).second) {
            log("GLES pname 0x%x assumed to have one result element", pname);
        }
        return 1;
    }
    }
}

std::optional<std::uint64_t> gl_pixel_bytes(GLenum format, GLenum type, GLsizei width,
                                            GLsizei height, GLint alignment) {
    if (width < 0 || height < 0 || (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8)) {
        return std::nullopt;
    }
    std::uint64_t components = 0;
    switch (format) {
    case 0x1901:  // GL_STENCIL_INDEX
    case 0x1902:  // GL_DEPTH_COMPONENT
    case 0x1903:  // GL_RED
    case 0x1906:  // GL_ALPHA
    case 0x1909:  // GL_LUMINANCE
    case 0x84F9:  // GL_DEPTH_STENCIL
    case 0x8D94:  // GL_RED_INTEGER
        components = 1;
        break;
    case 0x190A:  // GL_LUMINANCE_ALPHA
    case 0x8227:  // GL_RG
    case 0x8228:  // GL_RG_INTEGER
        components = 2;
        break;
    case 0x1907:  // GL_RGB
    case 0x8D98:  // GL_RGB_INTEGER
        components = 3;
        break;
    case 0x1908:  // GL_RGBA
    case 0x80E1:  // GL_BGRA_EXT (EXT_texture_format_BGRA8888, used by old NME/lime builds)
    case 0x8D99:  // GL_RGBA_INTEGER
        components = 4;
        break;
    default:
        return std::nullopt;
    }

    // A packed type fixes the whole pixel size and pairs with one or two formats only.
    struct PackedType {
        GLenum type;
        std::uint64_t bytes;
        GLenum format;
        GLenum other_format;
    };
    static constexpr PackedType kPacked[] = {
        {0x8363, 2, 0x1907, 0},       // GL_UNSIGNED_SHORT_5_6_5 with GL_RGB
        {0x8033, 2, 0x1908, 0},       // GL_UNSIGNED_SHORT_4_4_4_4 with GL_RGBA
        {0x8034, 2, 0x1908, 0},       // GL_UNSIGNED_SHORT_5_5_5_1 with GL_RGBA
        {0x8368, 4, 0x1908, 0x8D99},  // GL_UNSIGNED_INT_2_10_10_10_REV, also RGBA_INTEGER
        {0x8C3B, 4, 0x1907, 0},       // GL_UNSIGNED_INT_10F_11F_11F_REV with GL_RGB
        {0x8C3E, 4, 0x1907, 0},       // GL_UNSIGNED_INT_5_9_9_9_REV with GL_RGB
        {0x84FA, 4, 0x84F9, 0},       // GL_UNSIGNED_INT_24_8 with GL_DEPTH_STENCIL
        {0x8DAD, 8, 0x84F9, 0},       // GL_FLOAT_32_UNSIGNED_INT_24_8_REV with GL_DEPTH_STENCIL
    };
    std::uint64_t bytes_per_pixel = 0;
    for (const PackedType& packed : kPacked) {
        if (packed.type != type) continue;
        if (packed.format != format && packed.other_format != format) return std::nullopt;
        bytes_per_pixel = packed.bytes;
    }
    if (bytes_per_pixel == 0) {
        std::uint64_t component_bytes = 0;
        switch (type) {
        case 0x1400:  // GL_BYTE
        case 0x1401:  // GL_UNSIGNED_BYTE
            component_bytes = 1;
            break;
        case 0x1402:  // GL_SHORT
        case 0x1403:  // GL_UNSIGNED_SHORT
        case 0x140B:  // GL_HALF_FLOAT
        // OES_texture_half_float gave the same type a different number, and engines still use it:
        // Unity 4.5 probes for half-float support by uploading an 8x8 texture with it.
        case 0x8D61:  // GL_HALF_FLOAT_OES
            component_bytes = 2;
            break;
        case 0x1404:  // GL_INT
        case 0x1405:  // GL_UNSIGNED_INT
        case 0x1406:  // GL_FLOAT
            component_bytes = 4;
            break;
        default:
            return std::nullopt;
        }
        bytes_per_pixel = components * component_bytes;
    }

    std::uint64_t row = 0;
    if (!checked_multiply(static_cast<std::uint64_t>(width), bytes_per_pixel, row)) return std::nullopt;
    if (height == 0 || row == 0) return 0;
    const std::uint64_t align = static_cast<std::uint64_t>(alignment);
    if (row > std::numeric_limits<std::uint64_t>::max() - (align - 1)) return std::nullopt;
    const std::uint64_t stride = (row + align - 1) & ~(align - 1);
    std::uint64_t preceding = 0;
    if (!checked_multiply(static_cast<std::uint64_t>(height - 1), stride, preceding) ||
        preceding > std::numeric_limits<std::uint64_t>::max() - row) {
        return std::nullopt;
    }
    return preceding + row;
}

bool HostGl::thread_generation_matches(std::uint64_t egl_generation) const {
    return state(*this).sampled_egl_generation == egl_generation;
}

bool HostGl::check_and_update_thread_generation(std::uint64_t egl_generation) {
    GlThreadState& current = state(*this);
    if (current.sampled_egl_generation == egl_generation) return false;
    if (current.sampled_egl_generation == UINT64_MAX) {
        gl_threads_seen_.fetch_add(1, std::memory_order_relaxed);
    }
    current.sampled_egl_generation = egl_generation;
    return true;
}

void HostGl::note_pixel_store(GLenum pname, GLint param) {
    if (param != 1 && param != 2 && param != 4 && param != 8) return;
    GlThreadState& current = state(*this);
    if (pname == kGlPackAlignment) current.pack_alignment = param;
    if (pname == kGlUnpackAlignment) current.unpack_alignment = param;
}

namespace {

void evict_buffer_mapping(HostGl& host, GLuint id);

constexpr std::size_t kArrayBufferIndex = 0;
constexpr std::size_t kElementArrayBufferIndex = 1;
constexpr std::size_t kPixelPackBufferIndex = 2;
constexpr std::size_t kPixelUnpackBufferIndex = 3;

int buffer_target_index(GLenum target) {
    switch (target) {
    case kGlArrayBuffer: return 0;
    case kGlElementArrayBuffer: return 1;
    case kGlPixelPackBuffer: return 2;
    case kGlPixelUnpackBuffer: return 3;
    case 0x8A11: return 4;  // GL_UNIFORM_BUFFER
    case 0x8C8E: return 5;  // GL_TRANSFORM_FEEDBACK_BUFFER
    case 0x8F36: return 6;  // GL_COPY_READ_BUFFER
    case 0x8F37: return 7;  // GL_COPY_WRITE_BUFFER
    default: return -1;
    }
}

void sync_legacy_binding_fields(GlThreadState& current) {
    current.array_buffer = current.bound_buffers[kArrayBufferIndex];
    current.element_array_buffer = current.bound_buffers[kElementArrayBufferIndex];
    current.pixel_pack_buffer = current.bound_buffers[kPixelPackBufferIndex];
    current.pixel_unpack_buffer = current.bound_buffers[kPixelUnpackBufferIndex];
}

}  // namespace

void HostGl::note_bind_buffer(GLenum target, GLuint buffer) {
    GlThreadState& current = state(*this);
    const int idx = buffer_target_index(target);
    if (idx < 0) return;
    current.bound_buffers[static_cast<std::size_t>(idx)] = buffer;
    current.known_bindings |= static_cast<std::uint8_t>(1u << idx);
    if (static_cast<std::size_t>(idx) == kElementArrayBufferIndex) {
        current.vao_element_buffers[current.current_vao] = buffer;
    }
    sync_legacy_binding_fields(current);
}

void HostGl::note_delete_buffers(GLsizei n, const GLuint* buffers) {
    if (n <= 0 || buffers == nullptr) return;
    GlThreadState& current = state(*this);
    for (GLsizei i = 0; i < n; ++i) {
        const GLuint id = buffers[i];
        if (id == 0) continue;
        for (std::size_t slot = 0; slot < current.bound_buffers.size(); ++slot) {
            if (current.bound_buffers[slot] == id) {
                current.bound_buffers[slot] = 0;
                current.known_bindings &= static_cast<std::uint8_t>(~(1u << slot));
            }
        }
        for (auto& [vao, ebo] : current.vao_element_buffers) {
            if (ebo == id) ebo = 0;
        }
        evict_buffer_mapping(*this, id);
    }
    sync_legacy_binding_fields(current);
    if (multi_thread_gl_active()) runtime_.bump_gl_delete_epoch();
}

void HostGl::invalidate_state_filter() {
    GlThreadState& current = state(*this);
    current.filter_egl_generation = UINT64_MAX;
}

void HostGl::note_bind_vertex_array(GLuint array) {
    GlThreadState& current = state(*this);
    current.switch_vao_attributes(array);
    const auto it = current.vao_element_buffers.find(array);
    if (it != current.vao_element_buffers.end()) {
        current.bound_buffers[kElementArrayBufferIndex] = it->second;
    } else {
        // A freshly bound unseen VAO starts with EBO 0, never the previous VAO's EBO.
        current.bound_buffers[kElementArrayBufferIndex] = 0;
    }
    current.known_bindings |= static_cast<std::uint8_t>(1u << kElementArrayBufferIndex);
    sync_legacy_binding_fields(current);
}

void HostGl::note_delete_vertex_arrays(GLsizei n, const GLuint* arrays) {
    if (n <= 0 || arrays == nullptr) return;
    GlThreadState& current = state(*this);
    for (GLsizei i = 0; i < n; ++i) {
        const GLuint id = arrays[i];
        if (id == 0) continue;
        current.vao_element_buffers.erase(id);
        current.vao_attributes.erase(id);
        if (current.current_vao == id) {
            current.current_vao_known = true;
            note_bind_vertex_array(0);
        }
    }
    if (multi_thread_gl_active()) runtime_.bump_gl_delete_epoch();
}

bool HostGl::buffer_binding(GLenum target, GLuint& buffer) const {
    const GlThreadState& current = state(*this);
    const int idx = buffer_target_index(target);
    if (idx < 0) return false;
    if ((current.known_bindings & (1u << idx)) == 0) return false;
    buffer = current.bound_buffers[static_cast<std::size_t>(idx)];
    return true;
}

GLuint HostGl::pixel_buffer(bool pack) const {
    const GlThreadState& current = state(*this);
    return pack ? current.pixel_pack_buffer : current.pixel_unpack_buffer;
}

void HostGl::note_vertex_attrib_enabled(GLuint index, bool enabled) {
    GlThreadState& current = state(*this);
    GlThreadState::Attribute& attribute = current.attribute_mut(index);
    attribute.enabled = enabled;
    current.refresh_client_flag(index, attribute);
}

GLint HostGl::pixel_alignment(bool pack) const {
    const GlThreadState& current = state(*this);
    return pack ? current.pack_alignment : current.unpack_alignment;
}

void HostGl::invalidate_uniforms(GLuint program) {
    state(*this).uniforms.erase(program);
}

namespace {

int texture_target_slot(GLenum target) {
    switch (target) {
    case 0x0DE1: return 0;  // GL_TEXTURE_2D
    case 0x8513: return 1;  // GL_TEXTURE_CUBE_MAP
    case 0x8C1A: return 2;  // GL_TEXTURE_2D_ARRAY
    case 0x806F: return 3;  // GL_TEXTURE_3D
    default: return -1;
    }
}

int gl_cap_bit(GLenum cap) {
    switch (cap) {
    case 0x0BE2: return 0;  // GL_BLEND
    case 0x0B44: return 1;  // GL_CULL_FACE
    case 0x0B71: return 2;  // GL_DEPTH_TEST
    case 0x0C11: return 3;  // GL_SCISSOR_TEST
    case 0x0B90: return 4;  // GL_STENCIL_TEST
    case 0x0BD0: return 5;  // GL_DITHER
    case 0x8037: return 6;  // GL_POLYGON_OFFSET_FILL
    case 0x809E: return 7;  // GL_SAMPLE_ALPHA_TO_COVERAGE
    case 0x80A0: return 8;  // GL_SAMPLE_COVERAGE
    default: return -1;
    }
}

bool normalize_scalar_state(std::uint32_t index, const std::uint32_t (&in)[4], int& slot,
                            std::array<std::uint32_t, 4>& out) {
    out = {};
    switch (index) {
    case ZB_GL_HC_glDepthMask: slot = 0; out[0] = (in[0] != 0) ? 1u : 0u; return true;
    case ZB_GL_HC_glDepthFunc: slot = 1; out[0] = in[0]; return true;
    case ZB_GL_HC_glCullFace: slot = 2; out[0] = in[0]; return true;
    case ZB_GL_HC_glFrontFace: slot = 3; out[0] = in[0]; return true;
    case ZB_GL_HC_glBlendEquation:
        slot = 4;
        out[0] = in[0];
        out[1] = in[0];
        return true;
    case ZB_GL_HC_glBlendEquationSeparate:
        slot = 4;
        out[0] = in[0];
        out[1] = in[1];
        return true;
    case ZB_GL_HC_glBlendFunc:
        slot = 5;
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[0];
        out[3] = in[1];
        return true;
    case ZB_GL_HC_glBlendFuncSeparate:
        slot = 5;
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
        out[3] = in[3];
        return true;
    case ZB_GL_HC_glColorMask:
        slot = 6;
        for (int i = 0; i < 4; ++i) out[i] = (in[i] != 0) ? 1u : 0u;
        return true;
    case ZB_GL_HC_glViewport:
        slot = 7;
        for (int i = 0; i < 4; ++i) out[i] = in[i];
        return true;
    case ZB_GL_HC_glScissor:
        slot = 8;
        for (int i = 0; i < 4; ++i) out[i] = in[i];
        return true;
    case ZB_GL_HC_glDepthRangef:
        slot = 9;
        out[0] = in[0];
        out[1] = in[1];
        return true;
    case ZB_GL_HC_glClearColor:
        slot = 10;
        for (int i = 0; i < 4; ++i) out[i] = in[i];
        return true;
    default:
        return false;
    }
}

void ensure_filter_epoch(GlThreadState& current, std::uint64_t egl_generation) {
    if (current.filter_egl_generation != egl_generation) {
        current.reset_dynamic_state_filter(egl_generation);
    }
}

}  // namespace

bool HostGl::try_skip_redundant_call(std::uint32_t index, GuestThread& thread, std::uint64_t egl_generation) {
    switch (index) {
    case ZB_GL_HC_glActiveTexture: {
        GlThreadState& current = state(*this);
        ensure_filter_epoch(current, egl_generation);
        const GLenum tex = thread.regs()[0];
        if (current.active_texture_known && current.active_texture == tex) return true;
        break;
    }
    case ZB_GL_HC_glUseProgram: {
        GlThreadState& current = state(*this);
        ensure_filter_epoch(current, egl_generation);
        const GLuint program = thread.regs()[0];
        if (current.current_program_known && current.current_program == program) return true;
        break;
    }
    case ZB_GL_HC_glBindVertexArray:
    case ZB_GL_HC_glBindVertexArrayOES: {
        GlThreadState& current = state(*this);
        ensure_filter_epoch(current, egl_generation);
        if (current.current_vao_known && current.current_vao == thread.regs()[0]) return true;
        break;
    }
    case ZB_GL_HC_glBindBuffer: {
        GlThreadState& current = state(*this);
        ensure_filter_epoch(current, egl_generation);
        const int idx = buffer_target_index(thread.regs()[0]);
        if (idx >= 0 && (current.known_bindings & (1u << idx)) != 0 &&
            current.bound_buffers[static_cast<std::size_t>(idx)] == thread.regs()[1]) {
            return true;
        }
        break;
    }
    case ZB_GL_HC_glBindTexture: {
        GlThreadState& current = state(*this);
        ensure_filter_epoch(current, egl_generation);
        if (!current.active_texture_known) break;
        const std::uint32_t unit = current.active_texture - 0x84C0u;
        const int slot = texture_target_slot(thread.regs()[0]);
        if (unit < GlThreadState::kCachedTextureUnits && slot >= 0 &&
            (current.bound_texture_known[unit] & (1u << slot)) != 0 &&
            current.bound_textures[unit][static_cast<std::size_t>(slot)] == thread.regs()[1]) {
            return true;
        }
        break;
    }
    case ZB_GL_HC_glEnable:
    case ZB_GL_HC_glDisable: {
        const int bit = gl_cap_bit(thread.regs()[0]);
        if (bit < 0) break;
        GlThreadState& current = state(*this);
        ensure_filter_epoch(current, egl_generation);
        const std::uint32_t mask = 1u << static_cast<unsigned>(bit);
        const bool want = (index == ZB_GL_HC_glEnable);
        if ((current.cap_known & mask) != 0 && ((current.cap_value & mask) != 0) == want) {
            return true;
        }
        break;
    }
    default: {
        int slot = -1;
        std::array<std::uint32_t, 4> norm{};
        const std::uint32_t args[4] = {thread.regs()[0], thread.regs()[1], thread.regs()[2],
                                       thread.regs()[3]};
        if (normalize_scalar_state(index, args, slot, norm)) {
            GlThreadState& current = state(*this);
            ensure_filter_epoch(current, egl_generation);
            const auto& entry = current.scalar_states[static_cast<std::size_t>(slot)];
            if (entry.known && entry.words == norm) return true;
        }
        break;
    }
    }
    return false;
}

void HostGl::note_after_dispatch(std::uint32_t index, Call& call, std::uint64_t egl_generation) {
    if (!call.valid()) return;
    switch (index) {
    case ZB_GL_HC_glActiveTexture: {
        GlThreadState& current = state(*this);
        ensure_filter_epoch(current, egl_generation);
        const GLenum tex = call.scalar<GLenum>(0);
        if (tex >= 0x84C0u && tex < 0x84C0u + GlThreadState::kCachedTextureUnits) {
            current.active_texture = tex;
            current.active_texture_known = true;
        } else {
            current.active_texture_known = false;
        }
        break;
    }
    case ZB_GL_HC_glUseProgram: {
        GlThreadState& current = state(*this);
        ensure_filter_epoch(current, egl_generation);
        current.current_program = call.scalar<GLuint>(0);
        current.current_program_known = true;
        break;
    }
    case ZB_GL_HC_glDeleteProgram: {
        GlThreadState& current = state(*this);
        if (current.current_program_known && current.current_program == call.scalar<GLuint>(0)) {
            current.current_program_known = false;
        }
        if (multi_thread_gl_active()) runtime_.bump_gl_delete_epoch();
        break;
    }
    case ZB_GL_HC_glBindVertexArray:
    case ZB_GL_HC_glBindVertexArrayOES: {
        GlThreadState& current = state(*this);
        ensure_filter_epoch(current, egl_generation);
        current.current_vao_known = true;
        break;
    }
    case ZB_GL_HC_glBindTexture: {
        GlThreadState& current = state(*this);
        ensure_filter_epoch(current, egl_generation);
        if (!current.active_texture_known) break;
        const std::uint32_t unit = current.active_texture - 0x84C0u;
        const int slot = texture_target_slot(call.scalar<GLenum>(0));
        if (unit < GlThreadState::kCachedTextureUnits && slot >= 0) {
            current.bound_textures[unit][static_cast<std::size_t>(slot)] = call.scalar<GLuint>(1);
            current.bound_texture_known[unit] |= static_cast<std::uint8_t>(1u << slot);
        }
        break;
    }
    case ZB_GL_HC_glDeleteTextures: {
        const GLsizei n = call.scalar<GLsizei>(0);
        if (n <= 0) break;
        const GLuint* textures = call.pointer<const GLuint>(1, call.length(n, 1u), kPageRead);
        if (textures == nullptr) break;
        GlThreadState& current = state(*this);
        for (GLsizei i = 0; i < n; ++i) {
            const GLuint id = textures[i];
            if (id == 0) continue;
            for (std::size_t unit = 0; unit < GlThreadState::kCachedTextureUnits; ++unit) {
                std::uint8_t mask = current.bound_texture_known[unit];
                if (mask == 0) continue;
                for (std::size_t slot = 0; slot < GlThreadState::kCachedTextureTargets; ++slot) {
                    if ((mask & (1u << slot)) != 0 && current.bound_textures[unit][slot] == id) {
                        mask &= static_cast<std::uint8_t>(~(1u << slot));
                    }
                }
                current.bound_texture_known[unit] = mask;
            }
        }
        if (multi_thread_gl_active()) runtime_.bump_gl_delete_epoch();
        break;
    }
    case ZB_GL_HC_glEnable:
    case ZB_GL_HC_glDisable: {
        const int bit = gl_cap_bit(call.arg(0));
        if (bit < 0) break;
        GlThreadState& current = state(*this);
        ensure_filter_epoch(current, egl_generation);
        const std::uint32_t mask = 1u << static_cast<unsigned>(bit);
        current.cap_known |= mask;
        if (index == ZB_GL_HC_glEnable) {
            current.cap_value |= mask;
        } else {
            current.cap_value &= ~mask;
        }
        break;
    }
    default: {
        int slot = -1;
        std::array<std::uint32_t, 4> norm{};
        const std::uint32_t args[4] = {call.arg(0), call.arg(1), call.arg(2), call.arg(3)};
        if (normalize_scalar_state(index, args, slot, norm)) {
            GlThreadState& current = state(*this);
            ensure_filter_epoch(current, egl_generation);
            auto& entry = current.scalar_states[static_cast<std::size_t>(slot)];
            entry.words = norm;
            entry.known = true;
        }
        break;
    }
    }
}

std::optional<std::uint32_t> HostGl::allocate_guest(std::size_t size) {
    if (size == 0 || size > UINT32_MAX) return std::nullopt;
    if (allocator_) return allocator_(size);
    GuestCall args;
    args.regs = {static_cast<std::uint32_t>(size), 0, 0, 0};
    const auto result = runtime_.call_on_current(runtime_.service_api().malloc_fn, args);
    if (!result || result->r0 == 0) return std::nullopt;
    return result->r0;
}

namespace {

constexpr std::size_t kMaxPooledMirrorBlock = 4u << 20;

void compute_mirror_window(std::size_t size, std::size_t& request_size, std::size_t& max_acceptable) {
    request_size = size;
    if (size <= (1u << 20)) {
        request_size = std::bit_ceil(std::max<std::size_t>(size, 256));
    }
    max_acceptable = std::max<std::size_t>(request_size * 2, 4096);
}

std::size_t find_local_mirror_index(const GlThreadState& current, std::size_t size,
                                    std::size_t max_acceptable) {
    std::size_t best_idx = current.local_mirror_pool.size();
    for (std::size_t i = 0; i < current.local_mirror_pool.size(); ++i) {
        const auto& b = current.local_mirror_pool[i];
        if (b.capacity >= size && b.capacity <= max_acceptable) {
            if (best_idx == current.local_mirror_pool.size() ||
                b.capacity < current.local_mirror_pool[best_idx].capacity) {
                best_idx = i;
            }
        }
    }
    return best_idx;
}

bool can_push_local_mirror(const GlThreadState& current, std::size_t capacity) {
    return capacity > 0 && capacity <= kMaxPooledMirrorBlock &&
           current.local_mirror_pool.size() < HostGl::kMaxMirrorPoolEntries &&
           current.local_mirror_pool_bytes + capacity <= HostGl::kMaxMirrorPoolBytes;
}

}  // namespace

std::optional<std::uint32_t> HostGl::acquire_mirror(std::size_t size, std::size_t& capacity_out) {
    capacity_out = size;
    if (size == 0 || size > UINT32_MAX) return std::nullopt;
    if (allocator_) return allocator_(size);

    // Bucket allocations <= 1 MiB to power-of-two sizes so repeated map/unmap cycles
    // reuse pooled guest allocations without re-entering guest malloc/free.
    std::size_t request_size = 0;
    std::size_t max_acceptable = 0;
    compute_mirror_window(size, request_size, max_acceptable);
    {
        GlThreadState& current = state(*this);
        const std::size_t best_idx = find_local_mirror_index(current, size, max_acceptable);
        if (best_idx < current.local_mirror_pool.size()) {
            const auto chosen = current.local_mirror_pool[best_idx];
            current.local_mirror_pool[best_idx] = current.local_mirror_pool.back();
            current.local_mirror_pool.pop_back();
            current.local_mirror_pool_bytes -= chosen.capacity;
            capacity_out = chosen.capacity;
            return chosen.address;
        }
    }
    const auto& pool = mirror_pool_state_;
    if (pool->bytes_approx.load(std::memory_order_relaxed) > 0) {
        std::lock_guard<std::mutex> lock(pool->mutex);
        std::size_t best_idx = pool->blocks.size();
        for (std::size_t i = 0; i < pool->blocks.size(); ++i) {
            const MirrorBlock& b = pool->blocks[i];
            if (b.capacity >= size && b.capacity <= max_acceptable) {
                if (best_idx == pool->blocks.size() || b.capacity < pool->blocks[best_idx].capacity) {
                    best_idx = i;
                }
            }
        }
        if (best_idx < pool->blocks.size()) {
            const MirrorBlock chosen = pool->blocks[best_idx];
            pool->blocks[best_idx] = pool->blocks.back();
            pool->blocks.pop_back();
            pool->bytes -= chosen.capacity;
            pool->bytes_approx.store(pool->bytes, std::memory_order_relaxed);
            capacity_out = chosen.capacity;
            return chosen.address;
        }
    }
    capacity_out = request_size;
    return allocate_guest(request_size);
}

void HostGl::release_mirror(std::uint32_t address, std::size_t capacity) {
    if (address == 0 || allocator_) return;
    if (capacity > 0 && capacity <= kMaxPooledMirrorBlock) {
        GlThreadState& current = state(*this);
        if (can_push_local_mirror(current, capacity)) {
            current.local_mirror_pool.push_back(GlThreadState::ThreadMirrorBlock{address, capacity});
            current.local_mirror_pool_bytes += capacity;
            return;
        }
        const auto& pool = mirror_pool_state_;
        std::lock_guard<std::mutex> lock(pool->mutex);
        if (pool->blocks.size() < kMaxMirrorPoolEntries && pool->bytes + capacity <= kMaxMirrorPoolBytes) {
            pool->blocks.push_back(MirrorBlock{address, capacity});
            pool->bytes += capacity;
            pool->bytes_approx.store(pool->bytes, std::memory_order_relaxed);
            return;
        }
    }
    free_guest(address);
}

bool HostGl::can_fast_map_buffer_range(GuestThread& thread) const {
    if (allocator_) return false;
    const auto offset = static_cast<GLintptr>(static_cast<std::int32_t>(thread.regs()[1]));
    const auto length = static_cast<GLsizeiptr>(static_cast<std::int32_t>(thread.regs()[2]));
    if (offset < 0 || length <= 0) return false;
    const auto size = static_cast<std::size_t>(length);
    std::size_t request_size = 0;
    std::size_t max_acceptable = 0;
    compute_mirror_window(size, request_size, max_acceptable);
    const GlThreadState& current = state(*this);
    return find_local_mirror_index(current, size, max_acceptable) < current.local_mirror_pool.size();
}

bool HostGl::can_fast_unmap_buffer(GuestThread& thread) const {
    if (allocator_) return false;
    const GLenum target = static_cast<GLenum>(thread.regs()[0]);
    GLuint bound = 0;
    if (!buffer_binding(target, bound)) return false;
    const std::uint64_t key =
        bound > 0 ? ((std::uint64_t{1} << 32) | static_cast<std::uint64_t>(bound))
                  : static_cast<std::uint64_t>(target);
    const GlThreadState& current = state(*this);
    for (std::size_t i = 0; i < current.local_mapping_count; ++i) {
        if (current.local_mappings[i].key == key) {
            return can_push_local_mirror(current, current.local_mappings[i].mapping.capacity);
        }
    }
    return false;
}

void HostGl::free_guest(std::uint32_t address) {
    if (address == 0 || allocator_) return;
    GuestCall args;
    args.regs = {address, 0, 0, 0};
    (void)runtime_.call_on_current(runtime_.service_api().free_fn, args);
}

std::optional<std::uint64_t> HostGl::uniform_elements(GLuint program, GLint location) {
    GlThreadState& current = state(*this);
    auto cached = current.uniforms.find(program);
    if (cached == current.uniforms.end()) {
        std::unordered_map<GLint, std::uint64_t> locations;
        GLint count = 0;
        GLint max_length = 0;
        backend_.glGetProgramiv(program, kGlActiveUniforms, &count);
        backend_.glGetProgramiv(program, kGlActiveUniformMaxLength, &max_length);
        if (count < 0) count = 0;
        max_length = std::clamp(max_length, 1, 1 << 20);
        std::vector<GLchar> name(static_cast<std::size_t>(max_length));
        for (GLint index = 0; index < count; ++index) {
            GLsizei written = 0;
            GLint size = 0;
            GLenum type = 0;
            backend_.glGetActiveUniform(program, static_cast<GLuint>(index), max_length,
                                        &written, &size, &type, name.data());
            if (written < 0 || written >= max_length) continue;
            name[static_cast<std::size_t>(written)] = '\0';
            std::string uniform(name.data(), static_cast<std::size_t>(written));
            if (uniform.ends_with("[0]")) uniform.resize(uniform.size() - 3);
            const GLint uniform_location = backend_.glGetUniformLocation(program, uniform.c_str());
            const std::uint64_t elements = uniform_components(type);
            if (uniform_location >= 0 && elements != 0) {
                locations[uniform_location] = elements;
                if (size > 1 && size <= 256) {
                    for (GLint elem = 1; elem < size; ++elem) {
                        const std::string indexed = uniform + "[" + std::to_string(elem) + "]";
                        const GLint elem_loc = backend_.glGetUniformLocation(program, indexed.c_str());
                        if (elem_loc >= 0) locations[elem_loc] = elements;
                    }
                }
            }
        }
        cached = current.uniforms.emplace(program, std::move(locations)).first;
    }
    const auto found = cached->second.find(location);
    if (found == cached->second.end()) return std::nullopt;
    return found->second;
}

bool zbgl_manual_glGetBooleanv(HostGl& host, HostGl::Call& call) {
    return serve_pname(host, call, &GlBackend::glGetBooleanv);
}

bool zbgl_manual_glGetFloatv(HostGl& host, HostGl::Call& call) {
    return serve_pname(host, call, &GlBackend::glGetFloatv);
}

bool zbgl_manual_glGetIntegerv(HostGl& host, HostGl::Call& call) {
    return serve_pname(host, call, &GlBackend::glGetIntegerv);
}

bool zbgl_manual_glTexImage2D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLint internalformat = call.scalar<GLint>(2);
    const GLsizei width = call.scalar<GLsizei>(3);
    const GLsizei height = call.scalar<GLsizei>(4);
    const GLint border = call.scalar<GLint>(5);
    const GLenum format = call.scalar<GLenum>(6);
    const GLenum type = call.scalar<GLenum>(7);
    void* pixels = nullptr;
    if (!pixel_pointer(host, call, false, format, type, width, height, 1,
                       host.pixel_alignment(false), 8, kPageRead, pixels)) return true;
    host.backend().glTexImage2D(target, level, internalformat, width, height, border,
                                format, type, pixels);
    return true;
}

bool zbgl_manual_glTexSubImage2D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLint xoffset = call.scalar<GLint>(2);
    const GLint yoffset = call.scalar<GLint>(3);
    const GLsizei width = call.scalar<GLsizei>(4);
    const GLsizei height = call.scalar<GLsizei>(5);
    const GLenum format = call.scalar<GLenum>(6);
    const GLenum type = call.scalar<GLenum>(7);
    void* pixels = nullptr;
    if (!pixel_pointer(host, call, false, format, type, width, height, 1,
                       host.pixel_alignment(false), 8, kPageRead, pixels)) return true;
    host.backend().glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                                   format, type, pixels);
    return true;
}

bool zbgl_manual_glReadPixels(HostGl& host, HostGl::Call& call) {
    const GLint x = call.scalar<GLint>(0);
    const GLint y = call.scalar<GLint>(1);
    const GLsizei width = call.scalar<GLsizei>(2);
    const GLsizei height = call.scalar<GLsizei>(3);
    const GLenum format = call.scalar<GLenum>(4);
    const GLenum type = call.scalar<GLenum>(5);
    void* pixels = nullptr;
    if (!pixel_pointer(host, call, true, format, type, width, height, 1,
                       host.pixel_alignment(true), 6, kPageRead | kPageWrite, pixels)) return true;
    host.backend().glReadPixels(x, y, width, height, format, type, pixels);
    return true;
}

bool zbgl_manual_glGetUniformfv(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLint location = call.scalar<GLint>(1);
    const auto elements = host.uniform_elements(program, location);
    if (!elements) {
        host.reject(call, kGlInvalidOperation, "uniform location is not active in the program");
        return true;
    }
    GLfloat* params = call.pointer<GLfloat>(2, *elements, kPageRead | kPageWrite);
    if (call.valid()) host.backend().glGetUniformfv(program, location, params);
    return true;
}

bool zbgl_manual_glGetUniformiv(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLint location = call.scalar<GLint>(1);
    const auto elements = host.uniform_elements(program, location);
    if (!elements) {
        host.reject(call, kGlInvalidOperation, "uniform location is not active in the program");
        return true;
    }
    GLint* params = call.pointer<GLint>(2, *elements, kPageRead | kPageWrite);
    if (call.valid()) host.backend().glGetUniformiv(program, location, params);
    return true;
}

bool zbgl_manual_glBindAttribLocation(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLuint index = call.scalar<GLuint>(1);
    const GLchar* name = guest_string(host, call, call.arg(2));
    if (call.valid()) host.backend().glBindAttribLocation(program, index, name);
    return true;
}

bool zbgl_manual_glGetAttribLocation(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLchar* name = guest_string(host, call, call.arg(1));
    if (call.valid()) call.set_result(host.backend().glGetAttribLocation(program, name));
    return true;
}

bool zbgl_manual_glGetUniformLocation(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLchar* name = guest_string(host, call, call.arg(1));
    if (call.valid()) call.set_result(host.backend().glGetUniformLocation(program, name));
    return true;
}

bool zbgl_manual_glGetString(HostGl& host, HostGl::Call& call) {
    const GLenum name = call.scalar<GLenum>(0);
    GlThreadState& current = state(host);
    const std::uint64_t egl_gen = runtime_report().egl_current_generation();
    current.ensure_strings_epoch(egl_gen);
    const auto cached = current.strings.find(name);
    if (cached != current.strings.end()) {
        call.set_result(cached->second);
        return true;
    }
    if (name == kGlExtensions) {
        std::string served;
        for (const std::string& extension : served_extensions(host)) {
            if (!served.empty()) served.push_back(' ');
            served += extension;
        }
        if (const std::uint32_t address = answer_with_string(host, call, served)) current.strings[name] = address;
        return true;
    }
    const GLubyte* source = host.backend().glGetString(name);
    if (source == nullptr) return true;
    constexpr std::size_t kMaxDriverString = 64u << 20;
    const std::size_t length = strnlen(reinterpret_cast<const char*>(source), kMaxDriverString);
    if (length == kMaxDriverString) {
        host.reject(call, kGlInvalidOperation, "driver string exceeds 64 MiB");
        return true;
    }
    const std::string_view raw(reinterpret_cast<const char*>(source), length);
    const std::optional<std::string> clamped = clamp_gl_version_string(name, raw);
    if (const std::uint32_t address = answer_with_string(host, call, clamped ? *clamped : raw)) {
        current.strings[name] = address;
    }
    return true;
}

bool zbgl_manual_glShaderSource(HostGl& host, HostGl::Call& call) {
    const GLuint shader = call.scalar<GLuint>(0);
    const GLsizei count = call.scalar<GLsizei>(1);
    if (count < 0) {
        call.fail(kGlInvalidValue, "shader source count is negative");
        return true;
    }
    const std::uint64_t items = call.length(count);
    const std::uint32_t* guest_sources =
        call.pointer<const std::uint32_t>(2, items, kPageRead);
    const GLint* lengths = call.pointer<const GLint>(3, items, kPageRead);
    if (!call.valid()) return true;

    std::vector<const GLchar*> sources(static_cast<std::size_t>(items));
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const std::uint32_t address = guest_sources[i];
        if (lengths == nullptr || lengths[i] < 0) {
            sources[i] = guest_string(host, call, address);
        } else if (address == 0) {
            call.fail(kGlInvalidValue, "shader source pointer is null");
        } else {
            const std::uint8_t* source = host.runtime().memory().host_ptr(
                address, static_cast<std::uint64_t>(lengths[i]), kPageRead);
            if (source == nullptr) {
                call.fail(kGlInvalidValue, "shader source range is unreadable");
            } else {
                sources[i] = reinterpret_cast<const GLchar*>(source);
            }
        }
        if (!call.valid()) return true;
    }
    host.backend().glShaderSource(shader, count,
                                  sources.empty() ? nullptr : sources.data(), lengths);
    return true;
}

bool zbgl_manual_glVertexAttribPointer(HostGl& host, HostGl::Call& call) {
    const GLuint index = call.scalar<GLuint>(0);
    GlThreadState& current = state(host);
    GlThreadState::Attribute& attribute = current.attribute_mut(index);
    attribute.defined = true;
    attribute.size = call.scalar<GLint>(1);
    attribute.type = call.scalar<GLenum>(2);
    attribute.normalized = call.scalar<GLboolean>(3);
    attribute.stride = call.scalar<GLsizei>(4);
    attribute.guest_pointer = call.arg(5);
    attribute.integer = false;
    attribute.buffer = current.array_buffer;
    current.refresh_client_flag(index, attribute);
    if (!call.valid()) return true;
    if (attribute.buffer != 0) {
        host.backend().glVertexAttribPointer(
            index, attribute.size, attribute.type, attribute.normalized, attribute.stride,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(attribute.guest_pointer)));
    }
    return true;
}

void HostGl::apply_batched_vertex_attrib_pointer(GLuint index, GLint size, GLenum type,
                                                 GLboolean normalized, GLsizei stride,
                                                 std::uint32_t pointer) {
    GlThreadState& current = state(*this);
    GlThreadState::Attribute& attribute = current.attribute_mut(index);
    attribute.defined = true;
    attribute.size = size;
    attribute.type = type;
    attribute.normalized = normalized;
    attribute.stride = stride;
    attribute.guest_pointer = pointer;
    attribute.integer = false;
    attribute.buffer = current.array_buffer;
    current.refresh_client_flag(index, attribute);
    if (attribute.buffer != 0) {
        backend_.glVertexAttribPointer(
            index, size, type, normalized, stride,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(pointer)));
    }
}

bool zbgl_manual_glDrawArrays(HostGl& host, HostGl::Call& call) {
    const GLenum mode = call.scalar<GLenum>(0);
    const GLint first = call.scalar<GLint>(1);
    const GLsizei count = call.scalar<GLsizei>(2);
    if (!call.valid()) return true;
    if (count > 0) {
        if (first < 0 || !materialize_client_arrays(
                             host, call, static_cast<std::uint64_t>(first),
                             static_cast<std::uint64_t>(first) + static_cast<std::uint64_t>(count) - 1)) {
            return true;
        }
    }
    host.backend().glDrawArrays(mode, first, count);
    return true;
}

// Shared by glDrawElements, glDrawElementsInstanced and glDrawRangeElements: the indices
// argument is a guest pointer when no element array buffer is bound and a buffer offset when one
// is, and any enabled client array is materialized for the index range that will be read.
// Returns false when the call was rejected.
bool prepare_elements(HostGl& host, HostGl::Call& call, GLsizei count, GLenum type,
                      std::uint32_t guest_indices, const void*& driver_indices) {
    if (count < 0) {
        call.fail(kGlInvalidOperation, "element count is negative");
        return false;
    }
    GlThreadState& current = state(host);
    const bool has_client_arrays =
        current.fast_client_mask != 0 || current.overflow_client_count != 0;
    driver_indices = nullptr;
    std::uint64_t max_index = 0;
    if (current.element_array_buffer == 0) {
        const std::uint64_t index_size = element_index_bytes(type);
        if (index_size == 0) {
            call.fail(kGlInvalidOperation, "element index type is invalid");
            return false;
        }
        const std::uint64_t bytes = static_cast<std::uint64_t>(count) * index_size;
        const std::uint8_t* indices = count == 0 && guest_indices == 0
                                          ? nullptr
                                          : host.runtime().memory().host_ptr(guest_indices, bytes, kPageRead);
        if (count != 0 && indices == nullptr) {
            call.fail(kGlInvalidOperation, "client element indices are unreadable");
            return false;
        }
        driver_indices = indices;
        if (!has_client_arrays) return true;
        if (index_size == 1) {
            std::uint8_t max8 = 0;
            for (GLsizei i = 0; i < count; ++i) max8 = std::max(max8, indices[i]);
            max_index = max8;
        } else if (index_size == 2) {
            std::uint16_t max16 = 0;
            for (GLsizei i = 0; i < count; ++i) {
                std::uint16_t value16;
                std::memcpy(&value16, indices + static_cast<std::size_t>(i) * 2, sizeof(value16));
                max16 = std::max(max16, value16);
            }
            max_index = max16;
        } else {
            std::uint32_t max32 = 0;
            for (GLsizei i = 0; i < count; ++i) {
                std::uint32_t value32;
                std::memcpy(&value32, indices + static_cast<std::size_t>(i) * 4, sizeof(value32));
                max32 = std::max(max32, value32);
            }
            max_index = max32;
        }
    } else {
        driver_indices = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(guest_indices));
        if (!has_client_arrays) return true;
        if (count > 0) max_index = static_cast<std::uint64_t>(count - 1);
    }
    if (count > 0 && !materialize_client_arrays(host, call, 0, max_index)) return false;
    return true;
}

bool zbgl_manual_glDrawElements(HostGl& host, HostGl::Call& call) {
    const GLenum mode = call.scalar<GLenum>(0);
    const GLsizei count = call.scalar<GLsizei>(1);
    const GLenum type = call.scalar<GLenum>(2);
    const std::uint32_t guest_indices = call.arg(3);
    if (!call.valid()) return true;
    const void* driver_indices = nullptr;
    if (!prepare_elements(host, call, count, type, guest_indices, driver_indices)) return true;
    host.backend().glDrawElements(mode, count, type, driver_indices);
    return true;
}

void HostGl::apply_batched_draw_arrays(GuestThread& thread, GLenum mode, GLint first,
                                       GLsizei count) {
    const GlThreadState& current = state(*this);
    if (__builtin_expect(first >= 0 && count >= 0 && current.fast_client_mask == 0 &&
                             current.overflow_client_count == 0,
                         1)) {
        backend_.glDrawArrays(mode, first, count);
        return;
    }
    auto& regs = thread.regs();
    const std::uint32_t saved[4] = {regs[0], regs[1], regs[2], regs[3]};
    regs[0] = static_cast<std::uint32_t>(mode);
    regs[1] = static_cast<std::uint32_t>(first);
    regs[2] = static_cast<std::uint32_t>(count);
    regs[3] = 0;
    Call call(*this, thread, ZB_GL_HC_glDrawArrays);
    zbgl_manual_glDrawArrays(*this, call);
    regs[0] = saved[0];
    regs[1] = saved[1];
    regs[2] = saved[2];
    regs[3] = saved[3];
}

void HostGl::apply_batched_draw_elements(GuestThread& thread, GLenum mode, GLsizei count,
                                         GLenum type, std::uint32_t guest_indices) {
    const GlThreadState& current = state(*this);
    if (__builtin_expect(count >= 0 && current.element_array_buffer != 0 &&
                             current.fast_client_mask == 0 &&
                             current.overflow_client_count == 0,
                         1)) {
        backend_.glDrawElements(
            mode, count, type,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(guest_indices)));
        return;
    }
    auto& regs = thread.regs();
    const std::uint32_t saved[4] = {regs[0], regs[1], regs[2], regs[3]};
    regs[0] = static_cast<std::uint32_t>(mode);
    regs[1] = static_cast<std::uint32_t>(count);
    regs[2] = static_cast<std::uint32_t>(type);
    regs[3] = guest_indices;
    Call call(*this, thread, ZB_GL_HC_glDrawElements);
    zbgl_manual_glDrawElements(*this, call);
    regs[0] = saved[0];
    regs[1] = saved[1];
    regs[2] = saved[2];
    regs[3] = saved[3];
}

bool zbgl_manual_glGetVertexAttribPointerv(HostGl& host, HostGl::Call& call) {
    const GLuint index = call.scalar<GLuint>(0);
    (void)call.scalar<GLenum>(1);
    std::uint32_t* pointer = call.pointer<std::uint32_t>(2, 1, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    if (pointer == nullptr) {
        call.fail(kGlInvalidValue, "vertex attribute pointer output is null");
        return true;
    }
    const GlThreadState::Attribute* found = state(host).find_attribute(index);
    *pointer = found == nullptr ? 0 : found->guest_pointer;
    return true;
}

namespace {

constexpr GLenum kGlInvalidEnum = 0x0500;
constexpr GLenum kGlColorBuffer = 0x1800;

// GLES 3.0 sync objects. A GLsync is a driver pointer, so the guest only ever sees a 32-bit
// handle; the table is process-wide because sync objects are shared between contexts.
std::mutex sync_mutex;
std::vector<GLsync> sync_objects;  // handle - 1 indexes this; a null slot is a deleted object

std::uint32_t register_sync(GLsync sync) {
    if (sync == nullptr) return 0;
    std::lock_guard<std::mutex> lock(sync_mutex);
    for (std::size_t i = 0; i < sync_objects.size(); ++i) {
        if (sync_objects[i] == nullptr) {
            sync_objects[i] = sync;
            return static_cast<std::uint32_t>(i + 1);
        }
    }
    sync_objects.push_back(sync);
    return static_cast<std::uint32_t>(sync_objects.size());
}

GLsync lookup_sync(std::uint32_t handle) {
    std::lock_guard<std::mutex> lock(sync_mutex);
    if (handle == 0 || handle > sync_objects.size()) return nullptr;
    return sync_objects[handle - 1];
}

GLsync release_sync(std::uint32_t handle) {
    std::lock_guard<std::mutex> lock(sync_mutex);
    if (handle == 0 || handle > sync_objects.size()) return nullptr;
    const GLsync sync = sync_objects[handle - 1];
    sync_objects[handle - 1] = nullptr;
    return sync;
}

bool resolve_sync(HostGl& host, HostGl::Call& call, unsigned position, GLsync& sync) {
    const std::uint32_t handle = call.arg(position);
    if (!call.valid()) return false;
    sync = lookup_sync(handle);
    if (sync == nullptr) {
        host.reject(call, kGlInvalidValue, "sync handle is not a live sync object");
        return false;
    }
    return true;
}

// AAPCS32 puts a 64-bit argument in an even register pair (or an 8-byte aligned stack slot),
// low word first.
std::uint64_t argument64(HostGl::Call& call, unsigned low) {
    return static_cast<std::uint64_t>(call.arg(low)) |
           (static_cast<std::uint64_t>(call.arg(low + 1)) << 32);
}

// A guest mirror of a host-mapped buffer range: the driver's mapped memory lives outside the
// guest address space, so glMapBufferRange hands the guest a copy of the range and
// glUnmapBuffer writes it back. Keyed by the bound GL buffer object ID when non-zero (so two
// buffers bound in turn to e.g. GL_COPY_WRITE_BUFFER or unmapped via another target do not
// collide), falling back to the target enum only when no binding ID is reported.
std::mutex mapping_mutex;
std::unordered_map<std::uint64_t, BufferMapping> mappings;
std::atomic<std::size_t> global_mapping_count{0};

GLenum buffer_binding_enum(GLenum target) {
    switch (target) {
    case kGlArrayBuffer: return 0x8894;         // GL_ARRAY_BUFFER_BINDING
    case kGlElementArrayBuffer: return 0x8895;  // GL_ELEMENT_ARRAY_BUFFER_BINDING
    case kGlPixelPackBuffer: return 0x88ED;     // GL_PIXEL_PACK_BUFFER_BINDING
    case kGlPixelUnpackBuffer: return 0x88EF;   // GL_PIXEL_UNPACK_BUFFER_BINDING
    case 0x8A11: return 0x8A28;                 // GL_UNIFORM_BUFFER -> BINDING
    case 0x8C8E: return 0x8C8F;                 // GL_TRANSFORM_FEEDBACK_BUFFER -> BINDING
    case 0x8F36: return 0x8F36;                 // GL_COPY_READ_BUFFER (_BINDING)
    case 0x8F37: return 0x8F37;                 // GL_COPY_WRITE_BUFFER (_BINDING)
    default: return 0;
    }
}

std::uint64_t mapping_key(HostGl& host, GLenum target) {
    const std::uint64_t ctx_hash = (static_cast<std::uint64_t>(host.egl_context()) >> 4) & 0x7FFFFFFFULL;
    GLuint bound = 0;
    if (host.buffer_binding(target, bound)) {
        if (bound > 0) return (1ULL << 63) | (ctx_hash << 32) | static_cast<std::uint64_t>(bound);
        return (ctx_hash << 32) | static_cast<std::uint64_t>(target);
    }
    const GLenum binding = buffer_binding_enum(target);
    if (binding != 0) {
        GLint buffer = 0;
        frame_probe_add_driver_query();
        host.backend().glGetIntegerv(binding, &buffer);
        if (buffer > 0) {
            return (1ULL << 63) | (ctx_hash << 32) | static_cast<std::uint64_t>(static_cast<GLuint>(buffer));
        }
    }
    return (ctx_hash << 32) | static_cast<std::uint64_t>(target);
}

bool find_mapping(HostGl& host, std::uint64_t key, BufferMapping& mapping) {
    GlThreadState& current = state(host);
    for (std::size_t i = 0; i < current.local_mapping_count; ++i) {
        if (current.local_mappings[i].key == key) {
            mapping = current.local_mappings[i].mapping;
            return true;
        }
    }
    if (global_mapping_count.load(std::memory_order_relaxed) == 0) return false;
    std::lock_guard<std::mutex> lock(mapping_mutex);
    const auto found = mappings.find(key);
    if (found == mappings.end()) return false;
    mapping = found->second;
    return true;
}

void insert_mapping(HostGl& host, std::uint64_t key, const BufferMapping& mapping) {
    GlThreadState& current = state(host);
    if (!host.multi_thread_gl_active()) {
        for (std::size_t i = 0; i < current.local_mapping_count; ++i) {
            if (current.local_mappings[i].key == key) {
                current.local_mappings[i].mapping = mapping;
                return;
            }
        }
        if (current.local_mapping_count < current.local_mappings.size()) {
            current.local_mappings[current.local_mapping_count++] =
                GlThreadState::LocalMapping{key, mapping};
            return;
        }
    } else {
        for (std::size_t i = 0; i < current.local_mapping_count; ++i) {
            if (current.local_mappings[i].key == key) {
                current.local_mappings[i] = current.local_mappings[--current.local_mapping_count];
                break;
            }
        }
    }
    std::lock_guard<std::mutex> lock(mapping_mutex);
    mappings[key] = mapping;
    global_mapping_count.store(mappings.size(), std::memory_order_relaxed);
}

bool take_mapping(HostGl& host, std::uint64_t key, BufferMapping& mapping) {
    GlThreadState& current = state(host);
    for (std::size_t i = 0; i < current.local_mapping_count; ++i) {
        if (current.local_mappings[i].key == key) {
            mapping = current.local_mappings[i].mapping;
            current.local_mappings[i] = current.local_mappings[--current.local_mapping_count];
            return true;
        }
    }
    if (global_mapping_count.load(std::memory_order_relaxed) == 0) return false;
    std::lock_guard<std::mutex> lock(mapping_mutex);
    const auto found = mappings.find(key);
    if (found == mappings.end()) return false;
    mapping = found->second;
    mappings.erase(found);
    global_mapping_count.store(mappings.size(), std::memory_order_relaxed);
    return true;
}

void evict_buffer_mapping(HostGl& host, GLuint id) {
    if (id == 0) return;
    const std::uint64_t ctx_hash = (static_cast<std::uint64_t>(host.egl_context()) >> 4) & 0x7FFFFFFFULL;
    const std::uint64_t key = (1ULL << 63) | (ctx_hash << 32) | static_cast<std::uint64_t>(id);
    BufferMapping orphaned;
    if (take_mapping(host, key, orphaned)) {
        host.release_mirror(orphaned.guest, orphaned.capacity);
    }
}

template <typename T>
bool serve_uniform(HostGl& host, HostGl::Call& call,
                   void (GlBackend::*function)(GLuint, GLint, T*)) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLint location = call.scalar<GLint>(1);
    const auto elements = host.uniform_elements(program, location);
    if (!elements) {
        host.reject(call, kGlInvalidOperation, "uniform location is not active in the program");
        return true;
    }
    T* params = call.pointer<T>(2, *elements, kPageRead | kPageWrite);
    if (call.valid()) (host.backend().*function)(program, location, params);
    return true;
}

// GL_OES_mapbuffer maps a whole data store rather than a range, so it needs the buffer's size
// and translates its access enum to the glMapBufferRange bits the mirror logic is written in.
constexpr GLenum kGlBufferSize = 0x8764;
constexpr GLenum kGlReadOnlyOes = 0x88B8;
constexpr GLenum kGlWriteOnlyOes = 0x88B9;
constexpr GLenum kGlReadWriteOes = 0x88BA;

// glUnmapBuffer (GLES 3.0) and glUnmapBufferOES (GL_OES_mapbuffer) differ only in which driver
// entry point ends the mapping: both write the guest mirror back first.
bool unmap_mirrored(HostGl& host, HostGl::Call& call, bool oes) {
    const GLenum target = call.scalar<GLenum>(0);
    if (!call.valid()) return true;
    const std::uint64_t key = mapping_key(host, target);
    BufferMapping mapping;
    const bool mirrored = take_mapping(host, key, mapping);
    if (mirrored && mapping.shadow_only) {
        const bool flush_explicit = (mapping.access & kGlMapFlushExplicit) != 0;
        GLboolean result = 1;
        if ((mapping.access & kGlMapWrite) != 0 && !flush_explicit && mapping.length > 0) {
            const std::uint8_t* mirror =
                host.runtime().memory().host_ptr(mapping.guest, mapping.length, kPageRead);
            if (mirror == nullptr) {
                host.reject(call, kGlInvalidOperation, "mapped buffer mirror is unreadable");
                result = 0;
            } else {
                frame_probe_add_map_copy(mapping.length);
                host.backend().glBufferSubData(
                    target, mapping.offset, static_cast<GLsizeiptr>(mapping.length), mirror);
            }
        }
        host.release_mirror(mapping.guest, mapping.capacity);
        call.set_result(result);
        return true;
    }
    if (mirrored && (mapping.access & kGlMapWrite) != 0) {
        const bool flush_explicit = (mapping.access & kGlMapFlushExplicit) != 0;
        const std::uint8_t* mirror =
            host.runtime().memory().host_ptr(mapping.guest, mapping.length, kPageRead);
        if (mirror == nullptr) {
            host.reject(call, kGlInvalidOperation, "mapped buffer mirror is unreadable");
        } else {
            // With GL_MAP_FLUSH_EXPLICIT_BIT, glFlushMappedBufferRange already copied every
            // written subrange to the driver pointer; avoid copying the entire range again on unmap.
            if (!flush_explicit) {
                std::memcpy(mapping.data, mirror, static_cast<std::size_t>(mapping.length));
                frame_probe_add_map_copy(mapping.length);
            }
            gl_diagnose_map_unmap(mapping.diagnostic, mapping.length, mirror, mapping.data);
        }
    }
    const GLboolean result =
        oes ? host.backend().glUnmapBufferOES(target) : host.backend().glUnmapBuffer(target);
    if (mirrored) host.release_mirror(mapping.guest, mapping.capacity);
    call.set_result(result);
    return true;
}

// glGetBufferPointerv / glGetBufferPointervOES. void** in the guest is one 32-bit slot, and what
// belongs in it is the mirror's guest address, never the driver's mapped pointer.
bool report_buffer_pointer(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLenum pname = call.scalar<GLenum>(1);
    std::uint32_t* params = call.pointer<std::uint32_t>(2, 1, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    if (params == nullptr) {
        host.reject(call, kGlInvalidValue, "buffer pointer output is null");
        return true;
    }
    if (pname != kGlBufferMapPointer) {
        host.reject(call, kGlInvalidEnum, "buffer pointer pname is not GL_BUFFER_MAP_POINTER");
        return true;
    }
    BufferMapping mapping;
    *params = find_mapping(host, mapping_key(host, target), mapping) ? mapping.guest : 0;
    return true;
}

// The guest passes an array of 32-bit pointers to strings; the driver needs host-width pointers.
bool guest_string_array(HostGl& host, HostGl::Call& call, unsigned position, std::uint64_t items,
                        std::vector<const GLchar*>& strings) {
    const std::uint32_t* guest_strings =
        call.pointer<const std::uint32_t>(position, items, kPageRead);
    if (!call.valid()) return false;
    if (items != 0 && guest_strings == nullptr) {
        call.fail(kGlInvalidValue, "string array pointer is null");
        return false;
    }
    strings.resize(static_cast<std::size_t>(items));
    for (std::size_t i = 0; i < strings.size(); ++i) {
        strings[i] = guest_string(host, call, guest_strings[i]);
        if (!call.valid()) return false;
    }
    return true;
}

}  // namespace

bool zbgl_manual_glGetStringi(HostGl& host, HostGl::Call& call) {
    const GLenum name = call.scalar<GLenum>(0);
    const GLuint index = call.scalar<GLuint>(1);
    if (!call.valid()) return true;
    GlThreadState& current = state(host);
    const std::uint64_t egl_gen = runtime_report().egl_current_generation();
    current.ensure_strings_epoch(egl_gen);
    const std::uint64_t key = (static_cast<std::uint64_t>(name) << 32) | index;
    const auto cached = current.indexed_strings.find(key);
    if (cached != current.indexed_strings.end()) {
        call.set_result(cached->second);
        return true;
    }
    if (name == kGlExtensions) {
        const std::vector<std::string>& served = served_extensions(host);
        if (index >= served.size()) {
            host.reject(call, kGlInvalidValue, "extension index is out of range");
            return true;
        }
        if (const std::uint32_t address = answer_with_string(host, call, served[index])) {
            current.indexed_strings[key] = address;
        }
        return true;
    }
    const GLubyte* source = host.backend().glGetStringi(name, index);
    if (source == nullptr) return true;
    constexpr std::size_t kMaxDriverString = 64u << 20;
    const std::size_t length = strnlen(reinterpret_cast<const char*>(source), kMaxDriverString);
    if (length == kMaxDriverString) {
        host.reject(call, kGlInvalidOperation, "driver string exceeds 64 MiB");
        return true;
    }
    const auto address = host.allocate_guest(length + 1);
    std::uint8_t* destination =
        address ? host.runtime().memory().host_ptr(*address, length + 1, kPageRead | kPageWrite)
                : nullptr;
    if (destination == nullptr) {
        host.reject(call, kGlOutOfMemory, "guest allocation for driver string failed");
        return true;
    }
    std::memcpy(destination, source, length + 1);
    current.indexed_strings[key] = *address;
    call.set_result(*address);
    return true;
}

bool zbgl_manual_glMapBufferRange(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLintptr offset = call.scalar<GLintptr>(1);
    const GLsizeiptr length = call.scalar<GLsizeiptr>(2);
    const GLbitfield access = call.scalar<GLbitfield>(3);
    if (!call.valid()) return true;
    if (offset < 0 || length <= 0) {
        host.reject(call, kGlInvalidValue, "mapped range offset or length is invalid");
        return true;
    }
    const std::uint64_t key = mapping_key(host, target);
    BufferMapping existing;
    if (find_mapping(host, key, existing)) {
        gl_diagnose_map_collision(host, target, existing.diagnostic);
        host.reject(call, kGlInvalidOperation, "buffer target is already mapped");
        return true;
    }
    constexpr GLbitfield kGlMapUnsynchronized = 0x0020;
    const GLbitfield kFastShadowAllowedBits = kGlMapWrite | kGlMapInvalidateRange |
                                              kGlMapInvalidateBuffer | kGlMapFlushExplicit |
                                              kGlMapUnsynchronized;
    GLuint bound_buffer = 0;
    const bool can_shadow =
        !gl_diagnostics_enabled() &&
        host.buffer_binding(target, bound_buffer) && bound_buffer != 0 &&
        (access & kGlMapRead) == 0 && (access & kGlMapWrite) != 0 &&
        (access & (kGlMapInvalidateRange | kGlMapInvalidateBuffer)) != 0 &&
        (access & ~kFastShadowAllowedBits) == 0;
    if (can_shadow) {
        std::size_t mirror_capacity = 0;
        const auto address = host.acquire_mirror(static_cast<std::size_t>(length), mirror_capacity);
        std::uint8_t* mirror =
            address ? host.runtime().memory().host_ptr(*address, static_cast<std::uint64_t>(length),
                                                       kPageRead | kPageWrite)
                    : nullptr;
        if (mirror == nullptr) {
            if (address) host.release_mirror(*address, mirror_capacity);
            host.reject(call, kGlOutOfMemory, "guest mirror for the mapped buffer range failed");
            return true;
        }
        insert_mapping(host, key,
                       BufferMapping{*address, nullptr, static_cast<std::uint64_t>(length),
                                     mirror_capacity, access, offset, true, {}});
        call.set_result(*address);
        return true;
    }
    void* mapped = host.backend().glMapBufferRange(target, offset, length, access);
    if (mapped == nullptr) return true;  // The driver queued its own error; the guest gets NULL.
    std::size_t mirror_capacity = 0;
    const auto address = host.acquire_mirror(static_cast<std::size_t>(length), mirror_capacity);
    std::uint8_t* mirror =
        address ? host.runtime().memory().host_ptr(*address, static_cast<std::uint64_t>(length),
                                                   kPageRead | kPageWrite)
                : nullptr;
    if (mirror == nullptr) {
        if (address) host.release_mirror(*address, mirror_capacity);
        host.backend().glUnmapBuffer(target);
        host.reject(call, kGlOutOfMemory, "guest mirror for the mapped buffer range failed");
        return true;
    }
    // GL_MAP_INVALIDATE_RANGE_BIT / GL_MAP_INVALIDATE_BUFFER_BIT say the previous contents are
    // undefined, so they are never copied in; otherwise a reader and a partial writer both need
    // to see what the buffer holds now.
    const bool invalidated = (access & (kGlMapInvalidateRange | kGlMapInvalidateBuffer)) != 0;
    if (!invalidated && (access & (kGlMapRead | kGlMapWrite)) != 0) {
        std::memcpy(mirror, mapped, static_cast<std::size_t>(length));
    }
    const GlMapDiagnostic diagnostic =
        gl_diagnose_map(host, target, offset, length, access, *address, mirror,
                        static_cast<const std::uint8_t*>(mapped));
    insert_mapping(host, key,
                   BufferMapping{*address, static_cast<std::uint8_t*>(mapped),
                                 static_cast<std::uint64_t>(length), mirror_capacity, access,
                                 offset, false, diagnostic});
    call.set_result(*address);
    return true;
}

bool zbgl_manual_glUnmapBuffer(HostGl& host, HostGl::Call& call) {
    return unmap_mirrored(host, call, false);
}

bool zbgl_manual_glFlushMappedBufferRange(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLintptr offset = call.scalar<GLintptr>(1);
    const GLsizeiptr length = call.scalar<GLsizeiptr>(2);
    if (!call.valid()) return true;
    BufferMapping mapping;
    if (find_mapping(host, mapping_key(host, target), mapping) && (mapping.access & kGlMapWrite) != 0) {
        // The flushed range is relative to the start of the mapped range.
        if (offset < 0 || length < 0 ||
            static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(length) >
                mapping.length) {
            host.reject(call, kGlInvalidValue, "flushed range is outside the mapped range");
            return true;
        }
        const std::uint8_t* mirror = host.runtime().memory().host_ptr(
            mapping.guest + static_cast<std::uint32_t>(offset),
            static_cast<std::uint64_t>(length), kPageRead);
        if (mirror == nullptr) {
            host.reject(call, kGlInvalidOperation, "mapped buffer mirror is unreadable");
            return true;
        }
        if (mapping.shadow_only) {
            if (length > 0) {
                frame_probe_add_map_copy(static_cast<std::uint64_t>(length));
                host.backend().glBufferSubData(target, mapping.offset + offset, length, mirror);
            }
            return true;
        }
        std::memcpy(mapping.data + offset, mirror, static_cast<std::size_t>(length));
        frame_probe_add_map_copy(static_cast<std::uint64_t>(length));
        gl_diagnose_map_flush(mapping.diagnostic, offset, length, mirror,
                              mapping.data + offset);
    }
    host.backend().glFlushMappedBufferRange(target, offset, length);
    return true;
}

bool zbgl_manual_glGetBufferPointerv(HostGl& host, HostGl::Call& call) {
    return report_buffer_pointer(host, call);
}

// GL_OES_mapbuffer: the GLES 2.0 spelling of the same three entry points. glMapBufferOES maps
// the whole data store, so the mirror is the buffer's GL_BUFFER_SIZE bytes.
bool zbgl_manual_glMapBufferOES(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLenum access = call.scalar<GLenum>(1);
    if (!call.valid()) return true;
    GLbitfield bits = 0;
    switch (access) {
    case kGlReadOnlyOes: bits = kGlMapRead; break;
    case kGlWriteOnlyOes: bits = kGlMapWrite; break;
    case kGlReadWriteOes: bits = kGlMapRead | kGlMapWrite; break;
    default:
        host.reject(call, kGlInvalidEnum, "glMapBufferOES access is not a GL_*_ONLY enum");
        return true;
    }
    const std::uint64_t key = mapping_key(host, target);
    BufferMapping existing;
    if (find_mapping(host, key, existing)) {
        host.reject(call, kGlInvalidOperation, "buffer target is already mapped");
        return true;
    }
    GLint size = 0;
    host.backend().glGetBufferParameteriv(target, kGlBufferSize, &size);
    if (size <= 0) {
        host.reject(call, kGlInvalidOperation, "no buffer with a data store is bound to the target");
        return true;
    }
    void* mapped = host.backend().glMapBufferOES(target, access);
    if (mapped == nullptr) return true;  // The driver queued its own error; the guest gets NULL.
    std::size_t mirror_capacity = 0;
    const auto address = host.acquire_mirror(static_cast<std::size_t>(size), mirror_capacity);
    std::uint8_t* mirror =
        address ? host.runtime().memory().host_ptr(*address, static_cast<std::uint64_t>(size),
                                                   kPageRead | kPageWrite)
                : nullptr;
    if (mirror == nullptr) {
        if (address) host.release_mirror(*address, mirror_capacity);
        host.backend().glUnmapBufferOES(target);
        host.reject(call, kGlOutOfMemory, "guest mirror for the mapped buffer failed");
        return true;
    }
    // Unlike glMapBufferRange there is no invalidate bit: the data store keeps its contents, so
    // a partial writer must see them.
    std::memcpy(mirror, mapped, static_cast<std::size_t>(size));
    const GlMapDiagnostic diagnostic =
        gl_diagnose_map(host, target, 0, size, bits, *address, mirror,
                        static_cast<const std::uint8_t*>(mapped));
    insert_mapping(host, key,
                   BufferMapping{*address, static_cast<std::uint8_t*>(mapped),
                                 static_cast<std::uint64_t>(size), mirror_capacity, bits, 0,
                                 false, diagnostic});
    call.set_result(*address);
    return true;
}

bool zbgl_manual_glUnmapBufferOES(HostGl& host, HostGl::Call& call) {
    return unmap_mirrored(host, call, true);
}

bool zbgl_manual_glGetBufferPointervOES(HostGl& host, HostGl::Call& call) {
    return report_buffer_pointer(host, call);
}

bool zbgl_manual_glTexImage3D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLint internalformat = call.scalar<GLint>(2);
    const GLsizei width = call.scalar<GLsizei>(3);
    const GLsizei height = call.scalar<GLsizei>(4);
    const GLsizei depth = call.scalar<GLsizei>(5);
    const GLint border = call.scalar<GLint>(6);
    const GLenum format = call.scalar<GLenum>(7);
    const GLenum type = call.scalar<GLenum>(8);
    void* pixels = nullptr;
    if (!pixel_pointer(host, call, false, format, type, width, height, depth,
                       host.pixel_alignment(false), 9, kPageRead, pixels)) return true;
    host.backend().glTexImage3D(target, level, internalformat, width, height, depth, border,
                                format, type, pixels);
    return true;
}

bool zbgl_manual_glTexSubImage3D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLint xoffset = call.scalar<GLint>(2);
    const GLint yoffset = call.scalar<GLint>(3);
    const GLint zoffset = call.scalar<GLint>(4);
    const GLsizei width = call.scalar<GLsizei>(5);
    const GLsizei height = call.scalar<GLsizei>(6);
    const GLsizei depth = call.scalar<GLsizei>(7);
    const GLenum format = call.scalar<GLenum>(8);
    const GLenum type = call.scalar<GLenum>(9);
    void* pixels = nullptr;
    if (!pixel_pointer(host, call, false, format, type, width, height, depth,
                       host.pixel_alignment(false), 10, kPageRead, pixels)) return true;
    host.backend().glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth,
                                   format, type, pixels);
    return true;
}

namespace {

bool compressed_pixel_pointer(HostGl& host, HostGl::Call& call, GLsizei imageSize,
                              unsigned position, const void*& out) {
    if (!call.valid()) return false;
    if (imageSize < 0) {
        host.reject(call, kGlInvalidValue, "compressed image size is negative");
        return false;
    }
    const std::uint32_t guest_address = call.arg(position);
    if (host.pixel_buffer(false) != 0) {
        out = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(guest_address));
        return true;
    }
    if (guest_address == 0) {
        out = nullptr;
        return true;
    }
    const std::uint8_t* ptr = host.runtime().memory().host_ptr(
        guest_address, static_cast<std::uint64_t>(imageSize), kPageRead);
    if (imageSize != 0 && ptr == nullptr) {
        call.fail(kGlInvalidValue, "compressed texture data is unreadable");
        return false;
    }
    out = ptr;
    return true;
}

}  // namespace

bool zbgl_manual_glCompressedTexImage2D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLenum internalformat = call.scalar<GLenum>(2);
    const GLsizei width = call.scalar<GLsizei>(3);
    const GLsizei height = call.scalar<GLsizei>(4);
    const GLint border = call.scalar<GLint>(5);
    const GLsizei imageSize = call.scalar<GLsizei>(6);
    const void* data = nullptr;
    if (!compressed_pixel_pointer(host, call, imageSize, 7, data)) return true;
    host.backend().glCompressedTexImage2D(target, level, internalformat, width, height, border,
                                          imageSize, data);
    return true;
}

bool zbgl_manual_glCompressedTexSubImage2D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLint xoffset = call.scalar<GLint>(2);
    const GLint yoffset = call.scalar<GLint>(3);
    const GLsizei width = call.scalar<GLsizei>(4);
    const GLsizei height = call.scalar<GLsizei>(5);
    const GLenum format = call.scalar<GLenum>(6);
    const GLsizei imageSize = call.scalar<GLsizei>(7);
    const void* data = nullptr;
    if (!compressed_pixel_pointer(host, call, imageSize, 8, data)) return true;
    host.backend().glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format,
                                             imageSize, data);
    return true;
}

bool zbgl_manual_glCompressedTexImage3D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLenum internalformat = call.scalar<GLenum>(2);
    const GLsizei width = call.scalar<GLsizei>(3);
    const GLsizei height = call.scalar<GLsizei>(4);
    const GLsizei depth = call.scalar<GLsizei>(5);
    const GLint border = call.scalar<GLint>(6);
    const GLsizei imageSize = call.scalar<GLsizei>(7);
    const void* data = nullptr;
    if (!compressed_pixel_pointer(host, call, imageSize, 8, data)) return true;
    host.backend().glCompressedTexImage3D(target, level, internalformat, width, height, depth,
                                          border, imageSize, data);
    return true;
}

bool zbgl_manual_glCompressedTexSubImage3D(HostGl& host, HostGl::Call& call) {
    const GLenum target = call.scalar<GLenum>(0);
    const GLint level = call.scalar<GLint>(1);
    const GLint xoffset = call.scalar<GLint>(2);
    const GLint yoffset = call.scalar<GLint>(3);
    const GLint zoffset = call.scalar<GLint>(4);
    const GLsizei width = call.scalar<GLsizei>(5);
    const GLsizei height = call.scalar<GLsizei>(6);
    const GLsizei depth = call.scalar<GLsizei>(7);
    const GLenum format = call.scalar<GLenum>(8);
    const GLsizei imageSize = call.scalar<GLsizei>(9);
    const void* data = nullptr;
    if (!compressed_pixel_pointer(host, call, imageSize, 10, data)) return true;
    host.backend().glCompressedTexSubImage3D(target, level, xoffset, yoffset, zoffset, width,
                                             height, depth, format, imageSize, data);
    return true;
}

bool zbgl_manual_glDrawArraysInstanced(HostGl& host, HostGl::Call& call) {
    const GLenum mode = call.scalar<GLenum>(0);
    const GLint first = call.scalar<GLint>(1);
    const GLsizei count = call.scalar<GLsizei>(2);
    const GLsizei instancecount = call.scalar<GLsizei>(3);
    if (!call.valid()) return true;
    if (count > 0) {
        if (first < 0 || !materialize_client_arrays(
                             host, call, static_cast<std::uint64_t>(first),
                             static_cast<std::uint64_t>(first) + static_cast<std::uint64_t>(count) - 1)) {
            return true;
        }
    }
    host.backend().glDrawArraysInstanced(mode, first, count, instancecount);
    return true;
}

bool zbgl_manual_glDrawElementsInstanced(HostGl& host, HostGl::Call& call) {
    const GLenum mode = call.scalar<GLenum>(0);
    const GLsizei count = call.scalar<GLsizei>(1);
    const GLenum type = call.scalar<GLenum>(2);
    const std::uint32_t guest_indices = call.arg(3);
    const GLsizei instancecount = call.scalar<GLsizei>(4);
    if (!call.valid()) return true;
    const void* driver_indices = nullptr;
    if (!prepare_elements(host, call, count, type, guest_indices, driver_indices)) return true;
    host.backend().glDrawElementsInstanced(mode, count, type, driver_indices, instancecount);
    return true;
}

bool zbgl_manual_glDrawRangeElements(HostGl& host, HostGl::Call& call) {
    const GLenum mode = call.scalar<GLenum>(0);
    const GLuint start = call.scalar<GLuint>(1);
    const GLuint end = call.scalar<GLuint>(2);
    const GLsizei count = call.scalar<GLsizei>(3);
    const GLenum type = call.scalar<GLenum>(4);
    const std::uint32_t guest_indices = call.arg(5);
    if (!call.valid()) return true;
    const void* driver_indices = nullptr;
    if (!prepare_elements(host, call, count, type, guest_indices, driver_indices)) return true;
    host.backend().glDrawRangeElements(mode, start, end, count, type, driver_indices);
    return true;
}

bool zbgl_manual_glVertexAttribIPointer(HostGl& host, HostGl::Call& call) {
    const GLuint index = call.scalar<GLuint>(0);
    GlThreadState& current = state(host);
    GlThreadState::Attribute& attribute = current.attribute_mut(index);
    attribute.defined = true;
    attribute.size = call.scalar<GLint>(1);
    attribute.type = call.scalar<GLenum>(2);
    attribute.normalized = 0;
    attribute.stride = call.scalar<GLsizei>(3);
    attribute.guest_pointer = call.arg(4);
    attribute.integer = true;
    attribute.buffer = current.array_buffer;
    current.refresh_client_flag(index, attribute);
    if (!call.valid()) return true;
    if (attribute.buffer != 0) {
        host.backend().glVertexAttribIPointer(
            index, attribute.size, attribute.type, attribute.stride,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(attribute.guest_pointer)));
    }
    return true;
}

bool zbgl_manual_glGetUniformuiv(HostGl& host, HostGl::Call& call) {
    return serve_uniform(host, call, &GlBackend::glGetUniformuiv);
}

bool zbgl_manual_glGetInteger64v(HostGl& host, HostGl::Call& call) {
    return serve_pname(host, call, &GlBackend::glGetInteger64v);
}

bool zbgl_manual_glGetFragDataLocation(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLchar* name = guest_string(host, call, call.arg(1));
    if (call.valid()) call.set_result(host.backend().glGetFragDataLocation(program, name));
    return true;
}

bool zbgl_manual_glGetUniformBlockIndex(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLchar* name = guest_string(host, call, call.arg(1));
    if (call.valid()) call.set_result(host.backend().glGetUniformBlockIndex(program, name));
    return true;
}

namespace {

// GL_COLOR takes four components; GL_DEPTH and GL_STENCIL take one.
std::uint64_t clear_buffer_elements(GLenum buffer) { return buffer == kGlColorBuffer ? 4 : 1; }

}  // namespace

bool zbgl_manual_glClearBufferiv(HostGl& host, HostGl::Call& call) {
    const GLenum buffer = call.scalar<GLenum>(0);
    const GLint drawbuffer = call.scalar<GLint>(1);
    const GLint* value = call.pointer<const GLint>(2, clear_buffer_elements(buffer), kPageRead);
    if (call.valid()) host.backend().glClearBufferiv(buffer, drawbuffer, value);
    return true;
}

bool zbgl_manual_glClearBufferuiv(HostGl& host, HostGl::Call& call) {
    const GLenum buffer = call.scalar<GLenum>(0);
    const GLint drawbuffer = call.scalar<GLint>(1);
    const GLuint* value = call.pointer<const GLuint>(2, clear_buffer_elements(buffer), kPageRead);
    if (call.valid()) host.backend().glClearBufferuiv(buffer, drawbuffer, value);
    return true;
}

bool zbgl_manual_glClearBufferfv(HostGl& host, HostGl::Call& call) {
    const GLenum buffer = call.scalar<GLenum>(0);
    const GLint drawbuffer = call.scalar<GLint>(1);
    const GLfloat* value = call.pointer<const GLfloat>(2, clear_buffer_elements(buffer), kPageRead);
    if (call.valid()) host.backend().glClearBufferfv(buffer, drawbuffer, value);
    return true;
}

bool zbgl_manual_glGetUniformIndices(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLsizei count = call.scalar<GLsizei>(1);
    if (!call.valid()) return true;
    if (count < 0) {
        call.fail(kGlInvalidValue, "uniform name count is negative");
        return true;
    }
    const std::uint64_t items = call.length(count);
    std::vector<const GLchar*> names;
    if (!guest_string_array(host, call, 2, items, names)) return true;
    GLuint* indices = call.pointer<GLuint>(3, items, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    if (items != 0 && indices == nullptr) {
        host.reject(call, kGlInvalidValue, "uniform index output is null");
        return true;
    }
    host.backend().glGetUniformIndices(program, count, names.empty() ? nullptr : names.data(),
                                       indices);
    return true;
}

bool zbgl_manual_glTransformFeedbackVaryings(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLsizei count = call.scalar<GLsizei>(1);
    const GLenum bufferMode = call.scalar<GLenum>(3);
    if (!call.valid()) return true;
    if (count < 0) {
        call.fail(kGlInvalidValue, "varying count is negative");
        return true;
    }
    const std::uint64_t items = call.length(count);
    std::vector<const GLchar*> varyings;
    if (!guest_string_array(host, call, 2, items, varyings)) return true;
    host.backend().glTransformFeedbackVaryings(
        program, count, varyings.empty() ? nullptr : varyings.data(), bufferMode);
    return true;
}

bool zbgl_manual_glGetActiveUniformsiv(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLsizei uniformCount = call.scalar<GLsizei>(1);
    if (!call.valid()) return true;
    if (uniformCount < 0) {
        call.fail(kGlInvalidValue, "uniform count is negative");
        return true;
    }
    const std::uint64_t items = call.length(uniformCount);
    const GLuint* indices = call.pointer<const GLuint>(2, items, kPageRead);
    const GLenum pname = call.scalar<GLenum>(3);
    // Every GLES 3.0 pname of this query returns one value per named uniform.
    GLint* params = call.pointer<GLint>(4, items, kPageRead | kPageWrite);
    if (!call.valid()) return true;
    host.backend().glGetActiveUniformsiv(program, uniformCount, indices, pname, params);
    return true;
}

bool zbgl_manual_glGetActiveUniformBlockiv(HostGl& host, HostGl::Call& call) {
    const GLuint program = call.scalar<GLuint>(0);
    const GLuint uniformBlockIndex = call.scalar<GLuint>(1);
    const GLenum pname = call.scalar<GLenum>(2);
    if (!call.valid()) return true;
    std::uint64_t elements = 1;
    if (pname == kGlUniformBlockActiveUniformIndices) {
        GLint active = 0;
        host.backend().glGetActiveUniformBlockiv(program, uniformBlockIndex,
                                                 kGlUniformBlockActiveUniforms, &active);
        elements = active > 0 ? static_cast<std::uint64_t>(active) : 0;
    }
    GLint* params = call.pointer<GLint>(3, elements, kPageRead | kPageWrite);
    if (call.valid()) {
        host.backend().glGetActiveUniformBlockiv(program, uniformBlockIndex, pname, params);
    }
    return true;
}

bool zbgl_manual_glFenceSync(HostGl& host, HostGl::Call& call) {
    const GLenum condition = call.scalar<GLenum>(0);
    const GLbitfield flags = call.scalar<GLbitfield>(1);
    if (!call.valid()) return true;
    call.set_result(register_sync(host.backend().glFenceSync(condition, flags)));
    return true;
}

bool zbgl_manual_glIsSync(HostGl& host, HostGl::Call& call) {
    const std::uint32_t handle = call.arg(0);
    if (!call.valid()) return true;
    const GLsync sync = lookup_sync(handle);
    // An unknown handle is GL_FALSE, not an error.
    call.set_result(sync == nullptr ? GLboolean{0} : host.backend().glIsSync(sync));
    return true;
}

bool zbgl_manual_glDeleteSync(HostGl& host, HostGl::Call& call) {
    const std::uint32_t handle = call.arg(0);
    if (!call.valid()) return true;
    if (handle == 0) return true;  // Deleting the zero sync is a no-op, like the real entry point.
    const GLsync sync = release_sync(handle);
    if (sync == nullptr) {
        host.reject(call, kGlInvalidValue, "sync handle is not a live sync object");
        return true;
    }
    host.backend().glDeleteSync(sync);
    return true;
}

bool zbgl_manual_glClientWaitSync(HostGl& host, HostGl::Call& call) {
    GLsync sync = nullptr;
    if (!resolve_sync(host, call, 0, sync)) return true;
    const GLbitfield flags = call.scalar<GLbitfield>(1);
    const GLuint64 timeout = argument64(call, 2);
    if (!call.valid()) return true;
    call.set_result(host.backend().glClientWaitSync(sync, flags, timeout));
    return true;
}

bool zbgl_manual_glWaitSync(HostGl& host, HostGl::Call& call) {
    GLsync sync = nullptr;
    if (!resolve_sync(host, call, 0, sync)) return true;
    const GLbitfield flags = call.scalar<GLbitfield>(1);
    const GLuint64 timeout = argument64(call, 2);
    if (!call.valid()) return true;
    host.backend().glWaitSync(sync, flags, timeout);
    return true;
}

bool zbgl_manual_glGetSynciv(HostGl& host, HostGl::Call& call) {
    GLsync sync = nullptr;
    if (!resolve_sync(host, call, 0, sync)) return true;
    const GLenum pname = call.scalar<GLenum>(1);
    const GLsizei count = call.scalar<GLsizei>(2);
    GLsizei* length = call.pointer<GLsizei>(3, 1, kPageRead | kPageWrite);
    GLint* values = call.pointer<GLint>(4, call.length(count), kPageRead | kPageWrite);
    if (!call.valid()) return true;
    host.backend().glGetSynciv(sync, pname, count, length, values);
    return true;
}

#define ZB_GL_STUB(name)                                                            \
    bool zbgl_manual_##name(HostGl& host, HostGl::Call& call) {                     \
        host.reject(call, kGlInvalidOperation, #name " is not implemented yet");   \
        return true;                                                                \
    }

#undef ZB_GL_STUB

}  // namespace zb
