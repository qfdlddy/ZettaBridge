#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "zb/guest_thread.h"
#include "zb/jni_backend.h"
#include "zb/library_runtime.h"
#include "zb/native_call.h"
#include "zb/native_thunks.h"

namespace zb {

struct NativeCallCounter;  // core/include/zb/runtime_report.h

// The host side of the guest JNIEnv (guest/zbjni/zbjni.c -> libzbjni.so). It serves the flat JNI
// host calls against a JniBackend, owns the 32-bit handle and id tables and the thunk slots, and
// runs guest native code on behalf of Java callers.
//
// One instance per process: the constructor installs the process-wide native dispatcher. Like
// the LibraryRuntime it is process-lifetime and must outlive every guest thread.
// The last JNI host calls this process served, newest last, for a crash report.
std::string jni_recent_calls();
// The Java method names of the last CallMethodA calls, newest last. The host-call index alone
// cannot tell getObbDir from Bundle.getString, and a crash caused by one of them has to.
std::string jni_recent_call_names();
void jni_note_call_name(const std::string& name);
// `name=value` pairs of the last string results returned to the guest by CallMethodA, newest
// last. Bounded and truncated; used to see the actual strings a guest like Unity used to build a
// path, which the call names alone cannot show.
std::string jni_recent_string_results();
void jni_note_string_result(const std::string& name, const std::string& value);
// Name of one flat JNI host call (index 0xFB00+), or "?" when the table has no entry. The
// watchdog and the slow-call report name a stuck call through this, so it must stay readable
// without the generated tables at hand.
const char* jni_host_call_name(std::uint32_t index);

class HostJni {
public:
    // Builds the arguments of a native call. guest_env is the guest JNIEnv* of the calling host
    // thread; to_handle turns a host reference into a local handle of the call's frame.
    using BuildCall = std::function<GuestCall(std::uint32_t guest_env, const RefToHandle& to_handle)>;

    struct NativeResult {
        GuestResult guest;
        // Return type 'L': the returned object as a host local reference in the caller's frame.
        JniBackend::Ref ref = 0;
    };

    HostJni(LibraryRuntime& runtime, JniBackend& backend, std::size_t slot_capacity = kNativeThunkCount);
    ~HostJni();
    HostJni(const HostJni&) = delete;
    HostJni& operator=(const HostJni&) = delete;

    // Serves JNI host calls (0xFB00-0xFCFF). Chain it into LibraryRuntime::set_host_call_handler
    // before start(); returns false for other indices.
    bool handle_host_call(std::uint32_t index, GuestThread& thread);
    // Synchronous in-JIT fast path for pure-leaf JNI calls that never invoke Java code, trigger
    // class initializers, block, or re-enter the guest via call_on_current.
    bool handle_fast_host_call(std::uint32_t index, GuestThread& thread);
    // True once libzbjni.so has registered its API (while zbhost preloads it).
    bool ready() const;
    // The guest JavaVM*, valid when ready().
    std::uint32_t guest_java_vm() const;

    // The host JNIEnv of the calling thread's current JNI transition (call_native or a guest JNI
    // host call reached from one), or 0 outside one. Lets another host-call dispatcher (HostAssets)
    // call back into Java on the same thread.
    JniBackend::Env current_env();
    // Resolves a guest JNI local/global/weak-global reference handle to a host reference on the
    // calling thread; 0 for the null handle. Fatal for an invalid handle, like every JNI reference
    // operation. function names the caller for the error message.
    JniBackend::Ref resolve_ref(std::uint32_t handle, const char* function);
    // The reverse of resolve_ref: wraps a host reference as a guest JNI local handle of the
    // calling thread's current frame. 0 for a null ref. Lets another host-call dispatcher
    // (HostNativeWindow) hand a Java object back to the guest as a jobject.
    std::uint32_t new_local_handle(JniBackend::Ref ref);
    // The same for a reference that must outlive the call and cross threads: a NativeActivity
    // keeps the activity object in its own structure and uses it from its own threads, where a
    // local handle is long gone (ART ends the process for using one: "invalid JNI reference").
    // The caller owns the global reference behind it.
    std::uint32_t new_global_handle(JniBackend::Ref ref);
    // Removes a guest global handle created by new_global_handle and returns its underlying host ref (or 0).
    JniBackend::Ref remove_global_handle(std::uint32_t handle);
    // Wraps JniBackend::new_global_ref / delete_global_ref for companion NDK units (e.g. HostNativeWindow).
    JniBackend::Ref retain_global_ref(JniBackend::Env env, JniBackend::Ref ref);
    void release_global_ref(JniBackend::Env env, JniBackend::Ref ref);
    // Ensures the calling host thread has a guest JNIEnv* allocated and returns its guest address (or 0).
    std::uint32_t ensure_guest_env_for_current(JniBackend::Env env);

    // Runs a guest function as native code called from Java on the calling host thread, whose
    // host JNIEnv is env. Uses the guest thread this host thread already runs, or else this host
    // thread's cached carrier. Opens a local frame for the call; return_type 'L' converts the
    // returned handle before the frame closes. nullopt if the frame could not be opened (a Java
    // exception is pending) or the guest call failed.
    // call_counter, when non-null, is incremented with one relaxed atomic add: the native-call
    // census (runtime_report.h) for a bound Java native method. Callers that are not dispatching
    // a bound native method (the loader's own JNI_OnLoad call) pass nullptr and are not counted.
    std::optional<NativeResult> call_native(JniBackend::Env env, char return_type, std::uint32_t function,
                                            const BuildCall& build, NativeCallCounter* call_counter = nullptr);

    // Binds one Java native method to a guest function through a thunk slot: strips one leading
    // '!' from the signature, allocates a slot, and calls the backend once. Returns 0, or a
    // negative JNI error with the slot released. class_name, when known to the caller (the
    // loader has it from the export's decoded name), labels the method's native-call counter
    // "class.name"; nullptr labels it just "name".
    std::int32_t register_native(JniBackend::Env env, JniBackend::Ref cls, const char* name, const char* signature,
                                 std::uint32_t guest_function, bool is_static = false,
                                 const char* class_name = nullptr);

    // Runs a guest function on the calling host thread with no JNI transition: the guest thread
    // this host thread already runs, or else this host thread's cached carrier, borrowed on first
    // use. Lets a host callback the platform delivers on a Java thread (an Android looper
    // callback) re-enter the guest with the same guest identity that thread used before.
    // nullopt when no guest thread could be obtained or the call failed.
    std::optional<GuestResult> call_on_host_thread(std::uint32_t function, const GuestCall& args);

    // Guest loader operations bound to the calling host thread. All calls use its cached carrier,
    // or the currently running guest thread for a nested Java -> load transition. A failed loader
    // lookup reads dlerror on that same guest thread.
    // Host paths of the files the guest has mapped (LibraryRuntime::mapped_file_paths).
    std::vector<std::string> mapped_file_paths() const;

    std::uint32_t load_library_on_current(JniBackend::Env env, const std::string& path,
                                          std::uint32_t guest_flags, std::string& error);
    std::uint32_t find_symbol_on_current(JniBackend::Env env, std::uint32_t handle,
                                         const std::string& name, std::string& error);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace zb
