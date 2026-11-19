// JNI entry points of libzbridge.so for com.zettabridge.core.ZBridge.
#include <jni.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "guest_jni_runtime.h"
#include "zb/elf_fixups.h"
#include "zb/guest_thread.h"
#include "zb/log.h"
#include "zb/process.h"
#include "zb/runtime_report.h"
#include "zb/zbridge.h"

namespace {

// Converts arbitrary UTF-8 (which may contain 4-byte supplementary code points or invalid bytes)
// into valid Modified UTF-8 (CESU-8) accepted by ART CheckJNI's NewStringUTF / ThrowNew.
std::string to_mutf8(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    const auto* s = reinterpret_cast<const unsigned char*>(input.data());
    const std::size_t n = input.size();
    std::size_t i = 0;
    auto push_utf16 = [&](std::uint16_t u) {
        if (u != 0 && u <= 0x7Fu) {
            out.push_back(static_cast<char>(u));
        } else if (u <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | ((u >> 6) & 0x1Fu)));
            out.push_back(static_cast<char>(0x80u | (u & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xE0u | ((u >> 12) & 0x0Fu)));
            out.push_back(static_cast<char>(0x80u | ((u >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (u & 0x3Fu)));
        }
    };
    while (i < n) {
        const unsigned char c = s[i];
        if (c == 0) {
            out.push_back(static_cast<char>(0xC0));
            out.push_back(static_cast<char>(0x80));
            ++i;
        } else if (c < 0x80u) {
            out.push_back(static_cast<char>(c));
            ++i;
        } else if ((c & 0xE0u) == 0xC0u && i + 1 < n && (s[i + 1] & 0xC0u) == 0x80u) {
            const std::uint32_t cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
            if (cp >= 0x80u) {
                push_utf16(static_cast<std::uint16_t>(cp));
                i += 2;
            } else {
                out.push_back('?');
                ++i;
            }
        } else if ((c & 0xF0u) == 0xE0u && i + 2 < n && (s[i + 1] & 0xC0u) == 0x80u &&
                   (s[i + 2] & 0xC0u) == 0x80u) {
            const std::uint32_t cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
            if (cp >= 0x800u && !(cp >= 0xD800u && cp <= 0xDFFFu)) {
                push_utf16(static_cast<std::uint16_t>(cp));
                i += 3;
            } else {
                out.push_back('?');
                ++i;
            }
        } else if ((c & 0xF8u) == 0xF0u && i + 3 < n && (s[i + 1] & 0xC0u) == 0x80u &&
                   (s[i + 2] & 0xC0u) == 0x80u && (s[i + 3] & 0xC0u) == 0x80u) {
            const std::uint32_t cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) |
                                     ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
            if (cp >= 0x10000u && cp <= 0x10FFFFu) {
                const std::uint32_t v = cp - 0x10000u;
                push_utf16(static_cast<std::uint16_t>(0xD800u + (v >> 10)));
                push_utf16(static_cast<std::uint16_t>(0xDC00u + (v & 0x3FFu)));
                i += 4;
            } else {
                out.push_back('?');
                ++i;
            }
        } else {
            out.push_back('?');
            ++i;
        }
    }
    return out;
}

std::vector<std::string> to_strings(JNIEnv* env, jobjectArray array) {
    std::vector<std::string> out;
    if (array == nullptr || env->ExceptionCheck()) return out;
    const jsize count = env->GetArrayLength(array);
    out.reserve(static_cast<std::size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        auto value = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        if (env->ExceptionCheck()) {
            if (value != nullptr) env->DeleteLocalRef(value);
            return {};
        }
        if (value == nullptr) {
            out.emplace_back();
            continue;
        }
        const char* chars = env->GetStringUTFChars(value, nullptr);
        if (chars == nullptr) {
            env->DeleteLocalRef(value);
            return {};
        }
        out.emplace_back(chars);
        env->ReleaseStringUTFChars(value, chars);
        env->DeleteLocalRef(value);
    }
    return out;
}

std::vector<const char*> to_pointers(const std::vector<std::string>& strings) {
    std::vector<const char*> pointers;
    pointers.reserve(strings.size() + 1);
    for (const auto& s : strings) pointers.push_back(s.c_str());
    pointers.push_back(nullptr);
    return pointers;
}

// nullopt for a null string or when GetStringUTFChars failed (OutOfMemoryError pending).
std::optional<std::string> to_string(JNIEnv* env, jstring value) {
    if (value == nullptr) return std::nullopt;
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) return std::nullopt;
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

void throw_new(JNIEnv* env, const char* class_name, const std::string& message) {
    if (env->ExceptionCheck()) return;
    jclass cls = env->FindClass(class_name);
    if (cls == nullptr) return;  // NoClassDefFoundError pending
    const std::string safe = to_mutf8(message);
    env->ThrowNew(cls, safe.c_str());
    env->DeleteLocalRef(cls);
}

zb::JniBackend::Env to_env(JNIEnv* env) {
    return static_cast<zb::JniBackend::Env>(reinterpret_cast<std::uintptr_t>(env));
}

jstring new_safe_string(JNIEnv* env, const std::string& value) {
    const std::string safe = to_mutf8(value);
    return env->NewStringUTF(safe.c_str());
}

jstring optional_string(JNIEnv* env, const std::optional<std::string>& value) {
    return value ? new_safe_string(env, *value) : nullptr;
}

}  // namespace

extern "C" {

// static native int runExecutable(String sysroot, String[] argv, String[] envp)
// Blocks until the guest exits. envp == null passes the app process environment.
JNIEXPORT jint JNICALL Java_com_zettabridge_core_ZBridge_runExecutable(JNIEnv* env, jclass, jstring sysroot,
                                                                      jobjectArray argv, jobjectArray envp) {
    std::string sysroot_path;
    if (sysroot != nullptr) {
        const char* chars = env->GetStringUTFChars(sysroot, nullptr);
        if (chars == nullptr) return 2;
        sysroot_path = chars;
        env->ReleaseStringUTFChars(sysroot, chars);
    }

    const std::vector<std::string> args = to_strings(env, argv);
    if (env->ExceptionCheck() || args.empty()) return 2;
    const std::vector<const char*> arg_pointers = to_pointers(args);

    std::vector<std::string> env_strings;
    std::vector<const char*> env_pointers;
    if (envp != nullptr) {
        env_strings = to_strings(env, envp);
        if (env->ExceptionCheck()) return 2;
        env_pointers = to_pointers(env_strings);
    }

    return zb_run_executable(sysroot != nullptr ? sysroot_path.c_str() : nullptr, static_cast<int>(args.size()),
                             arg_pointers.data(), envp != nullptr ? env_pointers.data() : nullptr);
}

// static native void activatePlugin(String pluginRoot, int targetSdk, ClassLoader classLoader)
// Throws IllegalStateException with the reason.
JNIEXPORT void JNICALL Java_com_zettabridge_core_ZBridge_activatePlugin(JNIEnv* env, jclass, jstring plugin_root,
                                                                       jint target_sdk, jobject class_loader) {
    const std::optional<std::string> root = to_string(env, plugin_root);
    if (!root) {
        throw_new(env, "java/lang/NullPointerException", "pluginRoot");
        return;
    }
    if (target_sdk <= 0) {
        throw_new(env, "java/lang/IllegalStateException", "invalid plugin targetSdk " + std::to_string(target_sdk));
        return;
    }
    std::string error;
    if (!zb::GuestJniRuntime::get(env).proxies().activate_plugin(
            to_env(env), *root, static_cast<std::uint32_t>(target_sdk),
            static_cast<zb::JniBackend::Ref>(reinterpret_cast<std::uintptr_t>(class_loader)), error)) {
        zb::log("ZBridge.activatePlugin: %s", error.c_str());
        if (env->ExceptionCheck()) env->ExceptionClear();
        throw_new(env, "java/lang/IllegalStateException", "ZettaBridge cannot activate plugin: " + error);
    }
}

// static native int onProxyLoaded(String proxyPath), called by libzbproxy.so's JNI_OnLoad.
// Returns the guest JNI version or throws UnsatisfiedLinkError with the full detail, which also
// stays available through loadError/lastLoadError because ART replaces it.
JNIEXPORT jint JNICALL Java_com_zettabridge_core_ZBridge_onProxyLoaded(JNIEnv* env, jclass, jstring proxy_path) {
    const std::optional<std::string> path = to_string(env, proxy_path);
    if (!path) {
        throw_new(env, "java/lang/UnsatisfiedLinkError", "ZettaBridge: null proxy path");
        return JNI_ERR;
    }
    const zb::ProxyLoadResult result = zb::GuestJniRuntime::get(env).proxies().on_proxy_loaded(to_env(env), *path);
    if (result.ok) return result.jni_version;
    if (env->ExceptionCheck()) env->ExceptionClear();
    throw_new(env, "java/lang/UnsatisfiedLinkError", result.error);
    return JNI_ERR;
}

// static native boolean onNativeActivityCreated(long activity, long savedState, long size, String proxy)
// Called by libzbproxy.so from ANativeActivity_onCreate. Never throws: the framework is in the
// middle of creating an activity, and an exception here would surface somewhere unrelated. A
// refusal is false, with the reason in the runtime report.
JNIEXPORT jboolean JNICALL Java_com_zettabridge_core_ZBridge_onNativeActivityCreated(
    JNIEnv* env, jclass, jlong activity, jlong saved_state, jlong saved_state_size, jstring proxy_path) {
    const std::optional<std::string> path = to_string(env, proxy_path);
    if (!path) return JNI_FALSE;
    const bool ok = zb::GuestJniRuntime::get(env).proxies().on_native_activity_created(
        to_env(env), static_cast<std::uint64_t>(activity), static_cast<std::uint64_t>(saved_state),
        static_cast<std::uint64_t>(saved_state_size), *path);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return ok ? JNI_TRUE : JNI_FALSE;
}

// static native String loadError(String proxyPath): the stored failure of that proxy, or null.
JNIEXPORT jstring JNICALL Java_com_zettabridge_core_ZBridge_loadError(JNIEnv* env, jclass, jstring proxy_path) {
    const std::optional<std::string> path = to_string(env, proxy_path);
    zb::GuestJniRuntime* runtime = zb::GuestJniRuntime::peek();
    if (!path || runtime == nullptr) return nullptr;
    return optional_string(env, runtime->proxies().load_error(*path));
}

// static native String lastLoadError(): the most recent proxy load failure, or null.
JNIEXPORT jstring JNICALL Java_com_zettabridge_core_ZBridge_lastLoadError(JNIEnv* env, jclass) {
    zb::GuestJniRuntime* runtime = zb::GuestJniRuntime::peek();
    return runtime != nullptr ? optional_string(env, runtime->proxies().last_load_error()) : nullptr;
}

// static native boolean setReportFile(String path)
// Starts persisting the runtime report to `path`, rewriting it atomically whenever the report
// changes. Called once per :guest process, before plugin code runs, so a process that dies
// silently still leaves its last state on disk (OxygenOS drops our logcat output).
JNIEXPORT jboolean JNICALL Java_com_zettabridge_core_ZBridge_setReportFile(JNIEnv* env, jclass, jstring path) {
    const std::optional<std::string> file = to_string(env, path);
    if (!file) {
        throw_new(env, "java/lang/NullPointerException", "path");
        return JNI_FALSE;
    }
    return zb::write_runtime_report_to(zb::runtime_report(), *file) ? JNI_TRUE : JNI_FALSE;
}

// static native String runtimeReport(): the same text that setReportFile persists.
JNIEXPORT jstring JNICALL Java_com_zettabridge_core_ZBridge_runtimeReport(JNIEnv* env, jclass) {
    return new_safe_string(env, zb::runtime_report().text());
}

// static native String fixGuestLibrary(String path) throws IOException
// Applies the shared import fixups in place. Returns "unchanged", "changed: <c1>; <c2>" or
// "skipped: <reason>" (not an ARM ELF32 shared library); throws IOException on a malformed file.
JNIEXPORT jstring JNICALL Java_com_zettabridge_core_ZBridge_fixGuestLibrary(JNIEnv* env, jclass, jstring path) {
    const std::optional<std::string> file = to_string(env, path);
    if (!file) {
        throw_new(env, "java/lang/NullPointerException", "path");
        return nullptr;
    }
    const zb::ElfFixupReport report = zb::fix_guest_library(*file);
    std::string summary;
    switch (report.status) {
    case zb::ElfFixupStatus::Unchanged:
        summary = "unchanged";
        break;
    case zb::ElfFixupStatus::Changed:
        summary = "changed: ";
        for (std::size_t i = 0; i < report.changes.size(); ++i) {
            if (i != 0) summary += "; ";
            summary += report.changes[i];
        }
        break;
    case zb::ElfFixupStatus::Skipped:
        summary = "skipped: " + report.message;
        break;
    case zb::ElfFixupStatus::Error:
        throw_new(env, "java/io/IOException", *file + ": " + report.message);
        return nullptr;
    }
    return new_safe_string(env, summary);
}

// static native void setPreciseFaults(boolean enabled)
// Must be called before the guest runtime/Process is constructed: LibraryRuntime is
// process-lifetime and reads the override only at Process construction.
JNIEXPORT void JNICALL Java_com_zettabridge_core_ZBridge_setPreciseFaults(JNIEnv*, jclass, jboolean enabled) {
    zb::set_precise_faults_override(enabled != JNI_FALSE);
}

// static native void setGlDiagnostics(boolean enabled)
// Must be called before the guest JNI runtime is constructed: it reads the override once.
JNIEXPORT void JNICALL Java_com_zettabridge_core_ZBridge_setGlDiagnostics(JNIEnv*, jclass, jboolean enabled) {
    zb::GuestJniRuntime::set_gl_diagnostics_override(enabled != JNI_FALSE);
}

// static native void setUnsafeFp(boolean enabled)
// Must be called before the guest runtime/Process is constructed: GuestThread reads the override once.
JNIEXPORT void JNICALL Java_com_zettabridge_core_ZBridge_setUnsafeFp(JNIEnv*, jclass, jboolean enabled) {
    zb::set_unsafe_fp_override(enabled != JNI_FALSE);
}

}  // extern "C"
