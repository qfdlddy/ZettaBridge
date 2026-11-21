// Compatibility hooks for Java APIs that changed after the guest was built. Each hook runs inside
// one host call and only ever makes an old guest work on a current platform: a missing method, a
// failed lookup or a pending exception leaves the call exactly as the guest made it.
#include "host_jni_internal.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

// PendingIntent mutability, API 31+ (android.content.Intent).
constexpr std::int32_t kFlagMutable = 0x02000000;
constexpr std::int32_t kFlagImmutable = 0x04000000;

// The relay receiver the launcher declares in its :guest process
// (android/launcher/.../PluginRelayReceiver.java). A guest creates PendingIntent broadcasts with
// a component whose package is the plugin's context package - which is the launcher's, because
// PluginContext keeps the launcher's identity - and whose class is the plugin's receiver. No such
// receiver exists in this APK, so the alarm's broadcast used to be dropped. The relay runs the
// named plugin receiver instead.
constexpr const char* kRelayPackage = "com.zettabridge.launcher";
constexpr const char* kRelayReceiver = "com.zettabridge.launcher.PluginRelayReceiver";
constexpr const char* kRelayClassExtra = "zb.relay.class";

// Notifications need a channel from API 26 on; a guest that predates channels never sets one and
// its notification is dropped. One bridge channel serves every translated guest.
constexpr const char* kChannelId = "zb.plugin";
constexpr const char* kChannelName = "ZettaBridge";
constexpr std::int32_t kImportanceDefault = 3;

// Local references created by a hook. They belong to the guest call's JNI frame, which lives far
// longer than this call, so every one of them is released on the way out.
class LocalRefs {
public:
    LocalRefs(JniBackend& backend, JniBackend::Env env) : backend_(backend), env_(env) {}
    LocalRefs(const LocalRefs&) = delete;
    LocalRefs& operator=(const LocalRefs&) = delete;
    ~LocalRefs() {
        for (auto it = refs_.rbegin(); it != refs_.rend(); ++it) {
            if (*it != 0) backend_.delete_local_ref(env_, *it);
        }
    }

    // Keeps a reference for release at scope exit; returns it unchanged.
    JniBackend::Ref keep(JniBackend::Ref ref) {
        if (ref != 0) refs_.push_back(ref);
        return ref;
    }

    // Hands a reference to the caller: it is no longer released here.
    JniBackend::Ref release(JniBackend::Ref ref) {
        for (auto it = refs_.begin(); it != refs_.end(); ++it) {
            if (*it == ref) {
                refs_.erase(it);
                break;
            }
        }
        return ref;
    }

private:
    JniBackend& backend_;
    JniBackend::Env env_;
    std::vector<JniBackend::Ref> refs_;
};

std::string hex_flags(std::int32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "0x%08x", static_cast<unsigned>(value));
    return buffer;
}

// Reads a java.lang.String through the string functions of the backend.
bool read_java_string(JniBackend& backend, JniBackend::Env env, JniBackend::Ref str, std::string& out) {
    if (str == 0) return false;
    const std::int32_t length = backend.get_string_utf_length(env, str);
    if (length < 0) return false;
    if (!backend.get_string_utf_region(env, str, 0, length, out)) return false;
    return !backend.exception_check(env);
}

bool find_class(JniBackend& backend, JniBackend::Env env, const char* name, JniBackend::Ref& cls) {
    cls = backend.find_class(env, name);
    if (cls == 0 || backend.exception_check(env)) {
        if (backend.exception_check(env)) backend.exception_clear(env);
        cls = 0;
        return false;
    }
    return true;
}

bool method_of(JniBackend& backend, JniBackend::Env env, JniBackend::Ref cls, const char* name,
               const char* signature, bool is_static, JniBackend::Id& id) {
    id = backend.get_method_id(env, cls, name, signature, is_static);
    if (id == 0 || backend.exception_check(env)) {
        if (backend.exception_check(env)) backend.exception_clear(env);
        id = 0;
        return false;
    }
    return true;
}

// True when the receiver's class is the one this hook was written for. Name and signature alone
// are not proof: build() and notify() exist on other classes with the same shape.
bool receiver_is(JniBackend& backend, JniBackend::Env env, JniBackend::Ref obj, const char* expected) {
    std::string name;
    if (!backend.object_class_name(env, obj, name)) return false;
    // Real JNI reports the binary name ("android.app.Notification$Builder"); the host mock reports
    // the internal one. Both are the same class for this purpose.
    for (char& c : name) {
        if (c == '.') c = '/';
    }
    return name == expected;
}

}  // namespace

void HostJni::Impl::fix_pending_intent_flags(JniBackend::Env, const MethodRecord& record, JValue* values) {
    static constexpr char kTail[] = "I)Landroid/app/PendingIntent;";
    if (record.signature.size() < sizeof kTail - 1 ||
        record.signature.compare(record.signature.size() - (sizeof kTail - 1), sizeof kTail - 1, kTail) != 0) {
        return;
    }
    // Context, int, Intent (or Intent[]), int flags: the flags are the fourth argument.
    const std::int32_t flags = values[3].i;
    if ((flags & (kFlagMutable | kFlagImmutable)) != 0) return;
    values[3].i = flags | kFlagImmutable;
    static std::atomic<unsigned> notes{0};
    if (notes.fetch_add(1) < 8) {
        log("JNI %s%s: no PendingIntent mutability flag; calling it with FLAG_IMMUTABLE",
            record.name.c_str(), record.signature.c_str());
    }
    runtime_report().note_jni_detail(
        "pending-intent",
        record.name + " " + hex_flags(flags) + " -> " + hex_flags(flags | kFlagImmutable) +
            " (FLAG_IMMUTABLE)",
        true);
}

JniBackend::Ref HostJni::Impl::relay_broadcast_intent(JniBackend::Env env, JniBackend::Ref intent) {
    if (intent == 0) return intent;
    backend.ensure_local_capacity(env, 24);
    LocalRefs locals(backend, env);

    JniBackend::Ref intent_cls = 0;
    if (!find_class(backend, env, "android/content/Intent", intent_cls)) return intent;
    locals.keep(intent_cls);

    JniBackend::Id get_component = 0;
    JniBackend::Id get_class_name = 0;
    JniBackend::Id get_package = 0;
    if (!method_of(backend, env, intent_cls, "getComponent", "()Landroid/content/ComponentName;", false,
                   get_component)) {
        return intent;
    }
    const JniBackend::Ref component =
        locals.keep(backend.call_method(env, JniCallKind::Virtual, 'L', intent, 0, get_component, nullptr).l);
    if (component == 0) {
        backend.exception_clear(env);
        return intent;
    }
    const JniBackend::Ref component_cls = locals.keep(backend.get_object_class(env, component));
    if (component_cls == 0) {
        backend.exception_clear(env);
        return intent;
    }
    if (!method_of(backend, env, component_cls, "getClassName", "()Ljava/lang/String;", false, get_class_name) ||
        !method_of(backend, env, component_cls, "getPackageName", "()Ljava/lang/String;", false, get_package)) {
        return intent;
    }
    const JniBackend::Ref class_name =
        locals.keep(backend.call_method(env, JniCallKind::Virtual, 'L', component, 0, get_class_name, nullptr).l);
    const JniBackend::Ref package_name =
        locals.keep(backend.call_method(env, JniCallKind::Virtual, 'L', component, 0, get_package, nullptr).l);
    std::string target_class;
    std::string target_package;
    if (!read_java_string(backend, env, class_name, target_class) ||
        !read_java_string(backend, env, package_name, target_package)) {
        if (backend.exception_check(env)) backend.exception_clear(env);
        return intent;
    }
    // Only a component of this package is rewritten: it can only name plugin classes, and a
    // broadcast to any other package may be a working one this hook must not touch.
    if (target_package != kRelayPackage || target_class.empty()) return intent;

    JniBackend::Id copy_ctor = 0;
    JniBackend::Id put_extra = 0;
    JniBackend::Id set_class_name = 0;
    if (!method_of(backend, env, intent_cls, "<init>", "(Landroid/content/Intent;)V", false, copy_ctor) ||
        !method_of(backend, env, intent_cls, "putExtra",
                   "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;", false, put_extra) ||
        !method_of(backend, env, intent_cls, "setClassName",
                   "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;", false, set_class_name)) {
        return intent;
    }
    // A copy: the guest's own intent object is guest state and must not change under it.
    JValue argument{};
    argument.l = intent;
    const JniBackend::Ref relay =
        locals.keep(backend.call_method(env, JniCallKind::NewObject, 'L', 0, intent_cls, copy_ctor, &argument).l);
    if (relay == 0) {
        backend.exception_clear(env);
        return intent;
    }
    const JniBackend::Ref key = locals.keep(backend.new_string_utf(env, kRelayClassExtra));
    const JniBackend::Ref target = locals.keep(backend.new_string_utf(env, target_class.c_str()));
    const JniBackend::Ref relay_package = locals.keep(backend.new_string_utf(env, kRelayPackage));
    const JniBackend::Ref relay_receiver = locals.keep(backend.new_string_utf(env, kRelayReceiver));
    if (key == 0 || target == 0 || relay_package == 0 || relay_receiver == 0) {
        backend.exception_clear(env);
        return intent;
    }
    JValue arguments[2]{};
    arguments[0].l = key;
    arguments[1].l = target;
    // Both setters return the intent itself; the reference is already owned by `relay`, so the
    // result is dropped rather than tracked twice.
    backend.call_method(env, JniCallKind::Virtual, 'L', relay, 0, put_extra, arguments);
    arguments[0].l = relay_package;
    arguments[1].l = relay_receiver;
    backend.call_method(env, JniCallKind::Virtual, 'L', relay, 0, set_class_name, arguments);
    if (backend.exception_check(env)) {
        backend.exception_clear(env);
        return intent;
    }
    static std::atomic<unsigned> notes{0};
    if (notes.fetch_add(1) < 8) log("JNI PendingIntent.getBroadcast: relaying %s", target_class.c_str());
    runtime_report().note_jni_detail("pending-intent-relay", target_class, true);
    // The caller owns this reference for the duration of the real call.
    return locals.release(relay);
}

void HostJni::Impl::prepare_notification(JniBackend::Env env, JniBackend::Ref builder) {
    if (builder == 0) return;
    if (!receiver_is(backend, env, builder, "android/app/Notification$Builder")) return;
    backend.ensure_local_capacity(env, 8);
    LocalRefs locals(backend, env);
    JniBackend::Ref builder_cls = 0;
    if (!find_class(backend, env, "android/app/Notification$Builder", builder_cls)) return;
    locals.keep(builder_cls);
    JniBackend::Id get_channel = 0;
    if (!method_of(backend, env, builder_cls, "getChannelId", "()Ljava/lang/String;", false, get_channel)) return;
    const JniBackend::Ref existing =
        locals.keep(backend.call_method(env, JniCallKind::Virtual, 'L', builder, 0, get_channel, nullptr).l);
    if (backend.exception_check(env)) {
        backend.exception_clear(env);
        return;
    }
    // A guest that knows about channels keeps its own.
    if (existing != 0) return;
    JniBackend::Id set_channel = 0;
    JniBackend::Ref channel = 0;
    if (!method_of(backend, env, builder_cls, "setChannelId",
                   "(Ljava/lang/String;)Landroid/app/Notification$Builder;", false, set_channel) ||
        (channel = locals.keep(backend.new_string_utf(env, kChannelId))) == 0) {
        if (backend.exception_check(env)) backend.exception_clear(env);
        return;
    }
    JValue argument{};
    argument.l = channel;
    backend.call_method(env, JniCallKind::Virtual, 'L', builder, 0, set_channel, &argument);
    if (backend.exception_check(env)) {
        backend.exception_clear(env);
        return;
    }
    static std::atomic<unsigned> notes{0};
    if (notes.fetch_add(1) < 4) log("JNI Notification.Builder.build(): assigned channel %s", kChannelId);
    runtime_report().note_jni_detail("notification-channel", "assigned " + std::string(kChannelId), true);
}

void HostJni::Impl::ensure_notification_channel(JniBackend::Env env, JniBackend::Ref manager) {
    if (manager == 0) return;
    if (!receiver_is(backend, env, manager, "android/app/NotificationManager")) return;
    static std::atomic<bool> attempted{false};
    if (attempted.exchange(true)) return;
    backend.ensure_local_capacity(env, 16);
    LocalRefs locals(backend, env);
    JniBackend::Ref channel_cls = 0;
    JniBackend::Ref manager_cls = 0;
    if (!find_class(backend, env, "android/app/NotificationChannel", channel_cls) ||
        !find_class(backend, env, "android/app/NotificationManager", manager_cls)) {
        runtime_report().note_jni_detail("notification-channel", "unavailable", true);
        return;
    }
    locals.keep(channel_cls);
    locals.keep(manager_cls);
    JniBackend::Id constructor = 0;
    JniBackend::Id create = 0;
    if (!method_of(backend, env, channel_cls, "<init>",
                   "(Ljava/lang/String;Ljava/lang/CharSequence;I)V", false, constructor) ||
        !method_of(backend, env, manager_cls, "createNotificationChannel",
                   "(Landroid/app/NotificationChannel;)V", false, create)) {
        runtime_report().note_jni_detail("notification-channel", "unavailable", true);
        return;
    }
    const JniBackend::Ref id = locals.keep(backend.new_string_utf(env, kChannelId));
    const JniBackend::Ref name = locals.keep(backend.new_string_utf(env, kChannelName));
    if (id == 0 || name == 0) {
        backend.exception_clear(env);
        return;
    }
    JValue arguments[3]{};
    arguments[0].l = id;
    arguments[1].l = name;
    arguments[2].i = kImportanceDefault;
    const JniBackend::Ref channel =
        locals.keep(backend.call_method(env, JniCallKind::NewObject, 'L', 0, channel_cls, constructor, arguments).l);
    if (channel == 0) {
        backend.exception_clear(env);
        return;
    }
    JValue argument{};
    argument.l = channel;
    backend.call_method(env, JniCallKind::Virtual, 'V', manager, 0, create, &argument);
    if (backend.exception_check(env)) {
        backend.exception_clear(env);
        runtime_report().note_jni_detail("notification-channel", "create failed", true);
        return;
    }
    log("JNI NotificationManager.notify(): created channel %s", kChannelId);
    runtime_report().note_jni_detail("notification-channel", "created " + std::string(kChannelId), true);
}

bool HostJni::Impl::post_legacy_notification(JniBackend::Env env, JniBackend::Ref activity,
                                            const JValue* values) {
    if (activity == 0) return false;
    backend.ensure_local_capacity(env, 8);
    LocalRefs locals(backend, env);
    // Only a real Activity owns a context a notification can be posted from. Any other receiver
    // keeps its own method.
    JniBackend::Ref activity_cls = 0;
    if (!find_class(backend, env, "android/app/Activity", activity_cls)) return false;
    locals.keep(activity_cls);
    if (!backend.is_instance_of(env, activity, activity_cls)) {
        if (backend.exception_check(env)) backend.exception_clear(env);
        return false;
    }
    JniBackend::Ref bridge_cls = 0;
    if (!find_class(backend, env, "com/zettabridge/core/ZBridge", bridge_cls)) return false;
    locals.keep(bridge_cls);
    JniBackend::Id notify_guest = 0;
    if (!method_of(backend, env, bridge_cls, "notifyGuest",
                   "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V",
                   true, notify_guest)) {
        return false;
    }
    JValue arguments[5]{};
    arguments[0].l = activity;
    for (int i = 0; i < 4; ++i) arguments[i + 1] = values[i];
    backend.call_method(env, JniCallKind::Static, 'V', 0, bridge_cls, notify_guest, arguments);
    if (backend.exception_check(env)) {
        // Nothing was posted. Let the guest's own method run, so it sees the same failure it saw
        // before this hook existed rather than a silent no-op.
        backend.exception_clear(env);
        runtime_report().note_jni_detail("notification-relay", "failed", true);
        return false;
    }
    std::string id_text;
    if (!read_java_string(backend, env, values[0].l, id_text)) {
        if (backend.exception_check(env)) backend.exception_clear(env);
    }
    static std::atomic<unsigned> notes{0};
    if (notes.fetch_add(1) < 8) {
        log("JNI setNotification: posted notification %s (%d s) through ZBridge.notifyGuest",
            id_text.c_str(), values[3].i);
    }
    runtime_report().note_jni_detail(
        "notification-relay", "id " + id_text + " delay " + std::to_string(values[3].i) + "s", true);
    return true;
}

}  // namespace zb
