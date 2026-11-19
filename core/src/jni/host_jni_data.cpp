// JNI host calls: strings, arrays, direct buffers.
#include "host_jni_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>

#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

// Bytes of one element of a primitive type letter; 0 for anything else.
std::size_t element_size(char type) {
    switch (type) {
    case 'Z':
    case 'B':
        return 1;
    case 'C':
    case 'S':
        return 2;
    case 'I':
    case 'F':
        return 4;
    case 'J':
    case 'D':
        return 8;
    default:
        return 0;
    }
}

}  // namespace

std::uint32_t HostJni::Impl::mirror_direct_buffer(JniBackend::Env env, JniBackend::Ref buffer,
                                                  const void* host, std::int64_t capacity,
                                                  const char*& failure) {
    if (host == nullptr || capacity <= 0) {
        failure = "empty";
        return 0;
    }
    if (static_cast<std::uint64_t>(capacity) > kMirrorCapBytes) {
        failure = "over-cap";
        return 0;
    }
    const auto size = static_cast<std::uint64_t>(capacity);
    std::uint32_t existing_guest = 0;
    std::uint64_t existing_size = 0;
    {
        std::lock_guard<std::mutex> lock(mirror_mutex);
        auto found = mirrors.find(host);
        if (found != mirrors.end() && found->second.size >= size) {
            existing_guest = found->second.guest;
            existing_size = found->second.size;
        } else {
            const std::uint64_t existing_bytes = found != mirrors.end() ? found->second.size : 0;
            if (mirrored_bytes - existing_bytes + size > kMirrorCapBytes) {
                ++mirror_failures;
                failure = "over-cap";
                return 0;
            }
        }
    }
    if (existing_guest != 0) {
        std::uint8_t* dst = runtime.memory().host_ptr(existing_guest, size, kPageWrite);
        if (dst == nullptr) {
            failure = "unwritable-mirror";
            return 0;
        }
        std::memcpy(dst, host, size);
        (void)existing_size;
        return existing_guest;
    }

    // Guest malloc runs guest code on this thread, so it must never run under mirror_mutex:
    // another thread holding the guest allocator's own lock and waiting here would deadlock.
    runtime_report().note_jni_detail("direct-buffer-allocating", "size=" + std::to_string(size), true);
    GuestCall args;
    args.regs = {static_cast<std::uint32_t>(size), 0, 0, 0};
    const auto allocated = runtime.call_on_current(runtime.service_api().malloc_fn, args);
    if (!allocated || allocated->r0 == 0) {
        std::lock_guard<std::mutex> lock(mirror_mutex);
        ++mirror_failures;
        failure = "no-guest-memory";
        return 0;
    }
    const std::uint32_t guest = allocated->r0;
    const JniBackend::Ref new_global = backend.new_global_ref(env, buffer);

    std::uint32_t unused_guest = 0;
    JniBackend::Ref unused_global = 0;
    std::uint32_t result_guest = 0;
    {
        std::lock_guard<std::mutex> lock(mirror_mutex);
        auto found = mirrors.find(host);
        if (found != mirrors.end() && found->second.size >= size) {
            // A concurrent call already installed a big-enough mirror; release ours outside the lock.
            unused_guest = guest;
            unused_global = new_global;
            result_guest = found->second.guest;
        } else {
            const std::uint64_t old_size = found != mirrors.end() ? found->second.size : 0;
            if (mirrored_bytes - old_size + size > kMirrorCapBytes) {
                ++mirror_failures;
                failure = "over-cap";
                unused_guest = guest;
                unused_global = new_global;
            } else {
                if (found != mirrors.end()) {
                    unused_guest = found->second.guest;
                    unused_global = found->second.global;
                    mirrored_bytes -= found->second.size;
                    found->second = BufferMirror{guest, size, new_global};
                } else {
                    mirrors.emplace(host, BufferMirror{guest, size, new_global});
                }
                mirrored_bytes += size;
                has_mirrors.store(true, std::memory_order_release);
                result_guest = guest;
            }
        }
    }
    if (unused_global != 0) backend.delete_global_ref(env, unused_global);
    if (unused_guest != 0) {
        GuestCall free_args;
        free_args.regs = {unused_guest, 0, 0, 0};
        (void)runtime.call_on_current(runtime.service_api().free_fn, free_args);
    }
    if (result_guest == 0) return 0;
    std::uint8_t* dst = runtime.memory().host_ptr(result_guest, size, kPageWrite);
    if (dst == nullptr) {
        failure = "unwritable-mirror";
        return 0;
    }
    std::memcpy(dst, host, size);
    return result_guest;
}

void HostJni::Impl::flush_buffer_mirrors(JniBackend::Env env) {
    if (!has_mirrors.load(std::memory_order_acquire)) return;
    if (backend.exception_check(env)) return;
    struct Snapshot {
        const void* host = nullptr;
        std::uint32_t guest = 0;
        std::uint64_t size = 0;
        JniBackend::Ref global = 0;
    };
    std::vector<Snapshot> snapshot;
    {
        std::lock_guard<std::mutex> lock(mirror_mutex);
        if (mirrors.empty()) {
            has_mirrors.store(false, std::memory_order_release);
            return;
        }
        snapshot.reserve(mirrors.size());
        for (const auto& [h, m] : mirrors) snapshot.push_back({h, m.guest, m.size, m.global});
    }
    std::vector<std::uint32_t> stale_guests;
    std::vector<JniBackend::Ref> stale_globals;
    for (const Snapshot& s : snapshot) {
        // Ask Java outside mirror_mutex so JNI calls never nest inside mirror_mutex.
        const void* host = s.global != 0 ? backend.get_direct_buffer_address(env, s.global) : nullptr;
        const std::int64_t capacity =
            host != nullptr ? backend.get_direct_buffer_capacity(env, s.global) : 0;
        if (backend.exception_check(env)) {
            backend.exception_clear(env);
            continue;
        }
        if (host != s.host || capacity < 0 || static_cast<std::uint64_t>(capacity) < s.size) {
            log("direct buffer mirror dropped: the Java buffer changed or went away");
            std::lock_guard<std::mutex> lock(mirror_mutex);
            auto it = mirrors.find(s.host);
            if (it != mirrors.end() && it->second.guest == s.guest && it->second.global == s.global) {
                mirrored_bytes -= it->second.size;
                stale_guests.push_back(it->second.guest);
                if (it->second.global != 0) stale_globals.push_back(it->second.global);
                mirrors.erase(it);
                if (mirrors.empty()) has_mirrors.store(false, std::memory_order_release);
            }
            continue;
        }
        const std::uint8_t* src = runtime.memory().host_ptr(s.guest, s.size, kPageRead);
        if (src != nullptr) std::memcpy(const_cast<void*>(host), src, s.size);
    }
    for (JniBackend::Ref g : stale_globals) backend.delete_global_ref(env, g);
    for (std::uint32_t g : stale_guests) {
        GuestCall free_args;
        free_args.regs = {g, 0, 0, 0};
        (void)runtime.call_on_current(runtime.service_api().free_fn, free_args);
    }
}

bool HostJni::Impl::serve_data(JniCall& call) {
    JniThread& state = call.state();
    const char* name = jni_host_call_name(call.index());
    const auto ref = [&](unsigned position) { return resolve(state, call.arg(position), name); };
    const auto sint = [&](unsigned position) { return static_cast<std::int32_t>(call.arg(position)); };
    switch (call.index()) {
    case ZB_JNI_HC_NewString: {
        const JniBackend::Env env = call.env();
        const std::int32_t length = sint(1);
        const void* chars = length > 0 ? readable(env, call.arg(0), 2 * static_cast<std::uint64_t>(length), name) : nullptr;
        call.set(local(state, backend.new_string(env, static_cast<const std::uint16_t*>(chars), length)));
        return true;
    }
    case ZB_JNI_HC_NewStringUTF: {
        const JniBackend::Env env = call.env();
        if (call.arg(0) == 0) return true;  // ART returns null for a null string
        const std::string utf = read_string(env, call.arg(0), name);
        call.set(local(state, backend.new_string_utf(env, utf.c_str())));
        return true;
    }
    case ZB_JNI_HC_GetStringLength:
        call.set(static_cast<std::uint32_t>(backend.get_string_length(call.env(), ref(0))));
        return true;
    case ZB_JNI_HC_GetStringUTFLength:
        call.set(static_cast<std::uint32_t>(backend.get_string_utf_length(call.env(), ref(0))));
        return true;
    case ZB_JNI_HC_GetStringRegion: {
        const JniBackend::Env env = call.env();
        const JniBackend::Ref str = ref(0);
        const std::int32_t length = sint(2);
        void* out = length > 0 ? writable(env, call.arg(3), 2 * static_cast<std::uint64_t>(length), name) : nullptr;
        backend.get_string_region(env, str, sint(1), length, out);
        return true;
    }
    case ZB_JNI_HC_GetStringUTFRegion: {
        const JniBackend::Env env = call.env();
        const JniBackend::Ref str = ref(0);
        std::string utf;
        if (!backend.get_string_utf_region(env, str, sint(1), sint(2), utf)) return true;
        if (call.arg(3) == 0 && utf.empty()) return true;
        // ART writes the modified UTF-8 bytes and a NUL.
        std::memcpy(writable(env, call.arg(3), utf.size() + 1, name), utf.c_str(), utf.size() + 1);
        return true;
    }
    case ZB_JNI_HC_GetArrayLength:
        call.set(static_cast<std::uint32_t>(backend.get_array_length(call.env(), ref(0))));
        return true;
    case ZB_JNI_HC_GetArrayElementType:
        call.set(static_cast<std::uint8_t>(backend.get_array_element_type(call.env(), ref(0))));
        return true;
    case ZB_JNI_HC_NewObjectArray:
        call.set(local(state, backend.new_object_array(call.env(), sint(0), ref(1), ref(2))));
        return true;
    case ZB_JNI_HC_GetObjectArrayElement:
        call.set(local(state, backend.get_object_array_element(call.env(), ref(0), sint(1))));
        return true;
    case ZB_JNI_HC_SetObjectArrayElement:
        backend.set_object_array_element(call.env(), ref(0), sint(1), ref(2));
        return true;
    case ZB_JNI_HC_NewPrimitiveArray: {
        const JniBackend::Env env = call.env();
        const char type = static_cast<char>(call.arg(0));
        if (element_size(type) == 0) fatal(env, "NewPrimitiveArray: bad type 0x%x", call.arg(0));
        call.set(local(state, backend.new_primitive_array(env, type, sint(1))));
        return true;
    }
    case ZB_JNI_HC_GetPrimitiveArrayRegion:
    case ZB_JNI_HC_SetPrimitiveArrayRegion: {
        const JniBackend::Env env = call.env();
        const char type = static_cast<char>(call.arg(0));
        const std::size_t size = element_size(type);
        if (size == 0) fatal(env, "%s: bad type 0x%x", name, call.arg(0));
        const JniBackend::Ref array = ref(1);
        const std::int32_t length = sint(3);
        const std::uint64_t bytes = length > 0 ? static_cast<std::uint64_t>(length) * size : 0;
        if (call.index() == ZB_JNI_HC_GetPrimitiveArrayRegion) {
            void* out = bytes != 0 ? writable(env, call.arg(4), bytes, name) : nullptr;
            backend.get_primitive_array_region(env, type, array, sint(2), length, out);
        } else {
            const void* in = bytes != 0 ? readable(env, call.arg(4), bytes, name) : nullptr;
            backend.set_primitive_array_region(env, type, array, sint(2), length, in);
        }
        return true;
    }
    case ZB_JNI_HC_NewDirectByteBuffer: {
        const JniBackend::Env env = call.env();
        const std::uint32_t address = call.arg(0);
        const auto capacity = static_cast<std::int64_t>(call.arg(1) | (static_cast<std::uint64_t>(call.arg(2)) << 32));
        if (capacity < 0) {
            // Delegate negative capacity to ART so it throws IllegalArgumentException cleanly.
            call.set(local(state, backend.new_direct_byte_buffer(env, nullptr, capacity)));
            return true;
        }
        if (capacity > std::numeric_limits<std::int32_t>::max()) {
            fatal(env, "NewDirectByteBuffer: capacity %lld is outside the 32-bit guest range",
                  static_cast<long long>(capacity));
        }
        if (capacity > 0 && static_cast<std::uint64_t>(address) + static_cast<std::uint64_t>(capacity) > kGuestSpaceSize) {
            fatal(env, "NewDirectByteBuffer: 0x%08x + %lld leaves the guest address space", address,
                  static_cast<long long>(capacity));
        }
        void* host = address != 0 ? runtime.memory().base() + address : nullptr;
        call.set(local(state, backend.new_direct_byte_buffer(env, host, capacity)));
        return true;
    }
    case ZB_JNI_HC_GetDirectBufferAddress: {
        const JniBackend::Env env = call.env();
        const JniBackend::Ref buffer = ref(0);
        const auto* host = static_cast<const std::uint8_t*>(backend.get_direct_buffer_address(env, buffer));
        const std::uint8_t* base = runtime.memory().base();
        const bool inside = host != nullptr && host >= base &&
                            static_cast<std::uint64_t>(host - base) < kGuestSpaceSize;
        std::uint32_t answer = 0;
        const char* failure = "";
        bool mirrored = false;
        if (inside) {
            // A buffer the guest itself made with NewDirectByteBuffer: hand over its own address.
            answer = static_cast<std::uint32_t>(host - base);
        } else if (host != nullptr) {
            // A direct buffer Java allocated lives outside the guest's 4 GiB space, so its host
            // address cannot be handed over. Mirror it into guest memory instead; the guest reads
            // a real copy rather than NULL (Flutter copies from this pointer at once).
            // Recorded before mirroring: if the guest allocation below never returns, the report
            // still shows how far this call got.
            runtime_report().note_jni_detail("direct-buffer-last-request",
                                             "host=" + std::to_string(reinterpret_cast<std::uintptr_t>(host)), true);
            answer = mirror_direct_buffer(env, buffer, host,
                                          backend.get_direct_buffer_capacity(env, buffer), failure);
            mirrored = answer != 0;
        }
        call.set(answer);
        // Record every answer: a guest that trusts this pointer crashes far away from here.
        static std::atomic<unsigned> answered{0};
        const unsigned seen = answered.fetch_add(1) + 1;
        if (seen <= 4) {
            char text[200];
            std::snprintf(text, sizeof text, "buffer=0x%08x host=%p %s guest=0x%08x mirrored=%s%s",
                          call.arg(0), static_cast<const void*>(host), inside ? "inside" : "outside",
                          answer, mirrored ? "yes" : "no", mirrored || inside ? "" : failure);
            runtime_report().note_jni_detail("direct-buffer-" + std::to_string(seen), text, false);
        }
        if (seen <= 4 || (seen % 128) == 0 || (!inside && host != nullptr && !mirrored)) {
            runtime_report().note_jni_detail("direct-buffer-calls", std::to_string(seen), true);
            std::lock_guard<std::mutex> lock(mirror_mutex);
            runtime_report().note_jni_detail("direct-buffer-mirrored-bytes", std::to_string(mirrored_bytes), true);
            if (mirror_failures != 0) {
                runtime_report().note_jni_detail("direct-buffer-mirror-failures",
                                                 std::to_string(mirror_failures), true);
            }
        }
        if (!inside && host != nullptr && !mirrored && !logged_foreign_buffer.exchange(true)) {
            log("GetDirectBufferAddress: the buffer lies outside guest memory and could not be "
                "mirrored (%s); returning NULL (logged once)", failure);
        }
        return true;
    }
    case ZB_JNI_HC_GetDirectBufferCapacity:
        call.set64(static_cast<std::uint64_t>(backend.get_direct_buffer_capacity(call.env(), ref(0))));
        return true;
    default:
        return false;
    }
}

}  // namespace zb
