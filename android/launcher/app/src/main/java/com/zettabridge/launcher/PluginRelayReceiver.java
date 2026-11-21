package com.zettabridge.launcher;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/**
 * Delivers a guest's PendingIntent broadcast to its own receiver.
 *
 * A translated guest creates the PendingIntent with {@code new ComponentName(context, ItsReceiver)}
 * while its context carries the launcher package (it is not an installed package), so the alarm's
 * intent names this APK and a class that does not exist here. The JNI bridge rewrites such an
 * intent to this receiver, with the original class in {@link #EXTRA_CLASS} (see
 * {@code core/src/jni/host_jni_compat.cpp}). This receiver runs in :guest, loads the plugin that
 * declares the class and calls it with a plugin context, which is what the guest would have got
 * had the framework delivered the broadcast to it directly.
 */
public final class PluginRelayReceiver extends BroadcastReceiver {
    /** The plugin receiver class named by the rewritten intent. Not the launcher's own name: the
     * bridge writes the literal string, so a rename there has to be matched here. */
    static final String EXTRA_CLASS = "zb.relay.class";

    @Override
    public void onReceive(Context context, Intent intent) {
        String className = intent == null ? null : intent.getStringExtra(EXTRA_CLASS);
        if (className == null) return;
        if (!GuestRuntime.get().isInstalled()) {
            Diagnostics.report(context, "relay: the guest runtime is not installed", null, false);
            return;
        }
        try {
            LoadedPlugin plugin = GuestRuntime.get().pluginForReceiver(className);
            if (plugin == null) {
                Diagnostics.report(context, "relay: no imported plugin declares " + className, null, false);
                return;
            }
            // A copy without our routing extra: the plugin receiver must see the intent the guest
            // built, and nothing else.
            Intent delivered = new Intent(intent);
            delivered.removeExtra(EXTRA_CLASS);
            delivered.setExtrasClassLoader(plugin.classLoader);
            BroadcastReceiver receiver = (BroadcastReceiver) plugin.classLoader.loadClass(className)
                    .getDeclaredConstructor().newInstance();
            receiver.onReceive(new PluginContext(context, plugin), delivered);
        } catch (Throwable t) {
            Diagnostics.report(context, "relay: cannot deliver to " + className, t, true);
        }
    }
}
