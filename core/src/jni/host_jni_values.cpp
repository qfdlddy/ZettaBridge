// JNI host calls: method calls and fields.
#include "host_jni_internal.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>

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

// A guest word pair (low, high) as a backend value of the given type.
JValue value_from_bits(char type, std::uint64_t bits) {
    JValue value{};
    switch (type) {
    case 'Z': value.z = static_cast<std::uint8_t>(bits); break;
    case 'B': value.b = static_cast<std::int8_t>(bits); break;
    case 'C': value.c = static_cast<std::uint16_t>(bits); break;
    case 'S': value.s = static_cast<std::int16_t>(bits); break;
    case 'I': value.i = static_cast<std::int32_t>(bits); break;
    case 'F': {
        const auto low = static_cast<std::uint32_t>(bits);
        std::memcpy(&value.f, &low, sizeof value.f);
        break;
    }
    case 'J': value.j = static_cast<std::int64_t>(bits); break;
    case 'D': std::memcpy(&value.d, &bits, sizeof value.d); break;
    default: value.l = bits; break;
    }
    return value;
}

// A backend value as the guest's 64-bit result (r0:r1). References are converted by the caller.
std::uint64_t bits_from_value(char type, const JValue& value) {
    switch (type) {
    case 'Z': return value.z;
    case 'B': return static_cast<std::uint8_t>(value.b);
    case 'C': return value.c;
    case 'S': return static_cast<std::uint16_t>(value.s);
    case 'I': return static_cast<std::uint32_t>(value.i);
    case 'F': {
        std::uint32_t bits;
        std::memcpy(&bits, &value.f, sizeof bits);
        return bits;
    }
    case 'J': return static_cast<std::uint64_t>(value.j);
    case 'D': {
        std::uint64_t bits;
        std::memcpy(&bits, &value.d, sizeof bits);
        return bits;
    }
    default: return 0;
    }
}

bool is_value_type(char type) {
    return element_size(type) != 0 || type == 'L';
}

// Emulated guest audio threads cannot reliably meet 3ms (144-frame) HAL callback deadlines,
// whereas flooring to 1024 shrinks FMOD's OpenSL buffer queue down to 4 chunks. Floor small
// OUTPUT_FRAMES_PER_BUFFER values to 512 frames (~10.67ms at 48kHz) so FMOD keeps an 8-buffer
// (~85ms) queue that aligns evenly with FastTrack (3 x 512 = 1536) while cutting callbacks 3.55x.
JniBackend::Ref maybe_adjust_audio_property(JniBackend& backend, JniBackend::Env env,
                                            JniBackend::Ref key_ref, JniBackend::Ref value_ref) {
    if (key_ref == 0 || value_ref == 0 || backend.exception_check(env)) return value_ref;
    const std::int32_t key_len = backend.get_string_length(env, key_ref);
    if (backend.exception_check(env)) {
        backend.exception_clear(env);
        return value_ref;
    }
    if (key_len <= 0 || key_len > 128) return value_ref;
    std::string key;
    if (!backend.get_string_utf_region(env, key_ref, 0, key_len, key)) {
        backend.exception_clear(env);
        return value_ref;
    }
    if (key != "android.media.property.OUTPUT_FRAMES_PER_BUFFER") return value_ref;

    const std::int32_t val_len = backend.get_string_length(env, value_ref);
    if (backend.exception_check(env)) {
        backend.exception_clear(env);
        return value_ref;
    }
    if (val_len <= 0 || val_len > 32) return value_ref;
    std::string raw;
    if (!backend.get_string_utf_region(env, value_ref, 0, val_len, raw)) {
        backend.exception_clear(env);
        return value_ref;
    }
    char* end = nullptr;
    const long frames = std::strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0' || frames <= 0) return value_ref;
    if (frames >= 512) {
        runtime_report().note_audio_detail("prop-frames", raw + "->keep", false);
        return value_ref;
    }
    const JniBackend::Ref replacement = backend.new_string_utf(env, "512");
    if (replacement == 0 || backend.exception_check(env)) {
        if (backend.exception_check(env)) backend.exception_clear(env);
        return value_ref;
    }
    backend.delete_local_ref(env, value_ref);
    runtime_report().note_audio_detail("prop-frames", raw + "->512", false);
    return replacement;
}

}  // namespace

bool HostJni::Impl::serve_values(JniCall& call) {
    JniThread& state = call.state();
    const char* name = jni_host_call_name(call.index());
    const auto ref = [&](unsigned position) { return resolve(state, call.arg(position), name); };
    switch (call.index()) {
    case ZB_JNI_HC_CallMethodA: {
        const JniBackend::Env env = call.env();
        const std::uint32_t kind = call.arg(0);
        const char type = static_cast<char>(call.arg(1));
        const std::uint32_t method = call.arg(4);
        if (kind > ZB_JNI_CALL_NEW_OBJECT || (!is_value_type(type) && type != 'V')) {
            fatal(env, "CallMethodA: bad call kind %u or type 0x%x", kind, call.arg(1));
        }
        // Fast lock-free path avoids two mutex locks and a string copy per JNI call.
        std::optional<MethodRecord> owned_record;
        const MethodRecord* record = method_record_fast(method);
        if (record == nullptr) {
            const JniBackend::Id checked = method_id(env, method, name);
            owned_record = method_record(method);
            if (!owned_record) fatal(env, "CallMethodA: jmethodID 0x%08x has no shorty", method);
            owned_record->id = checked;
            record = &*owned_record;
        }
        const JniBackend::Id id = record->id;
        const std::string& shorty = record->shorty;
        const AudioMethodHook audio_hook = record->audio_hook;
        if (!record->name.empty()) jni_note_call_name(record->name);
        // A constructor returns V; NewObject asks for the new object ('L').
        const bool matches = kind == ZB_JNI_CALL_NEW_OBJECT ? shorty[0] == 'V' && type == 'L' : shorty[0] == type;
        if (!matches) {
            fatal(env, "CallMethodA: result type %c does not match the method shorty %s", type, shorty.c_str());
        }
        const JniBackend::Ref obj = ref(2);
        const JniBackend::Ref cls = ref(3);
        const std::size_t count = shorty.size() - 1;
        std::array<JValue, ZB_JNI_SHORTY_SIZE> values{};
        if (count != 0) {
            const std::uint8_t* raw = readable(env, call.arg(5), 8 * count, name);
            for (std::size_t i = 0; i < count; ++i) {
                std::uint64_t bits;
                std::memcpy(&bits, raw + 8 * i, sizeof bits);
                const char letter = shorty[i + 1];
                values[i] = letter == 'L'
                                ? value_from_bits('L', resolve(state, static_cast<std::uint32_t>(bits), name))
                                : value_from_bits(letter, bits);
            }
        }
        if (audio_hook != AudioMethodHook::None && kind != ZB_JNI_CALL_STATIC && obj != 0) {
            if (!audio_bt_shield_noted.exchange(true, std::memory_order_relaxed)) {
                runtime_report().note_audio_detail("bt-shield", "active", false);
            }
            if (audio_hook == AudioMethodHook::IsBluetoothOn) {
                call.set64(0);
                return true;
            }
            if (audio_hook == AudioMethodHook::DeviceCallbackNoop) {
                return true;
            }
        }
        JValue result = backend.call_method(env, static_cast<JniCallKind>(kind), type, obj, cls, id, values.data());
        if (type == 'L' && result.l == 0 && !backend.exception_check(env)) {
            // A null object where the guest asked for one. The guest usually dereferences it much
            // later and in a different library, so name the method and its signature here (the
            // shorty erases both the return type and every reference argument) while we still can.
            std::string what = record->name.empty() ? std::string("(unknown method)") : record->name;
            what += record->signature.empty() ? std::string("(shorty ") + shorty + ")" : record->signature;
            std::string receiver;
            if (kind != ZB_JNI_CALL_STATIC && obj != 0 && backend.object_class_name(env, obj, receiver)) {
                what += " on " + receiver;
            }
            // A getter that answers null is usually keyed (Bundle.getString, SharedPreferences.getString):
            // the argument is the whole difference between "absent by design" and "the bridge lost it".
            if (count == 1 && shorty.size() > 1 && shorty[1] == 'L' && values[0].l != 0 &&
                record->signature.rfind("(Ljava/lang/String;)", 0) == 0) {
                std::string argument;
                const std::int32_t chars = backend.get_string_length(env, values[0].l);
                if (!backend.exception_check(env) && chars >= 0) {
                    const std::int32_t take = std::min<std::int32_t>(chars, 256);
                    if (backend.get_string_utf_region(env, values[0].l, 0, take, argument)) {
                        what += " key=\"" + argument + "\"";
                    }
                }
                if (backend.exception_check(env)) backend.exception_clear(env);
            }
            note_null_result(what);
        }
        if (audio_hook == AudioMethodHook::GetDevices && type == 'L' && result.l != 0) {
            result.l = backend.filter_bluetooth_audio_devices(env, result.l);
        } else if (audio_hook == AudioMethodHook::GetProperty && kind != ZB_JNI_CALL_STATIC && obj != 0 &&
                   type == 'L' && count == 1 && result.l != 0) {
            result.l = maybe_adjust_audio_property(backend, env, values[0].l, result.l);
        }
        if (type == 'L') {
            // Record the actual string a getter returned. A guest that later builds a path from
            // these (getPackageName, File.getAbsolutePath, toString, ...) can fault far away, so
            // the value has to be captured where both the method name and the jstring are known.
            const std::string_view kStringRet = ")Ljava/lang/String;";
            const bool returns_string =
                record->signature.size() >= kStringRet.size() &&
                record->signature.compare(record->signature.size() - kStringRet.size(), kStringRet.size(),
                                          kStringRet) == 0;
            if (result.l != 0 && !record->name.empty() && returns_string && !backend.exception_check(env)) {
                static const char* const kStringGetters[] = {"getPackageName", "getAbsolutePath",
                                                             "getPath",        "getParent",
                                                             "toString",       "getCanonicalPath"};
                bool interesting = false;
                for (const char* getter : kStringGetters) {
                    if (record->name == getter) { interesting = true; break; }
                }
                if (interesting) {
                    std::string text;
                    const std::int32_t chars = backend.get_string_length(env, result.l);
                    if (!backend.exception_check(env) && chars >= 0) {
                        const std::int32_t take = std::min<std::int32_t>(chars, 512);
                        if (backend.get_string_utf_region(env, result.l, 0, take, text)) {
                            jni_note_string_result(record->name, text);
                        }
                    }
                    if (backend.exception_check(env)) backend.exception_clear(env);
                }
            }
            call.set(local(state, result.l));
        } else if (type != 'V') {
            call.set64(bits_from_value(type, result));
        }
        return true;
    }
    case ZB_JNI_HC_GetField: {
        const JniBackend::Env env = call.env();
        const char type = static_cast<char>(call.arg(1));
        if (!is_value_type(type)) fatal(env, "GetField: bad type 0x%x", call.arg(1));
        const JniBackend::Ref obj = ref(2);
        const JniBackend::Id id = field_id(env, call.arg(3), name);
        const JValue value = backend.get_field(env, call.arg(0) != 0, type, obj, id);
        if (type == 'L') {
            call.set(local(state, value.l));
        } else {
            call.set64(bits_from_value(type, value));
        }
        return true;
    }
    case ZB_JNI_HC_SetField: {
        const JniBackend::Env env = call.env();
        const char type = static_cast<char>(call.arg(1));
        if (!is_value_type(type)) fatal(env, "SetField: bad type 0x%x", call.arg(1));
        const JniBackend::Ref obj = ref(2);
        const JniBackend::Id id = field_id(env, call.arg(3), name);
        const std::uint64_t bits = type == 'L' ? ref(4) : (call.arg(4) | (static_cast<std::uint64_t>(call.arg(5)) << 32));
        backend.set_field(env, call.arg(0) != 0, type, obj, id, value_from_bits(type, bits));
        return true;
    }
    default:
        return false;
    }
}

}  // namespace zb
