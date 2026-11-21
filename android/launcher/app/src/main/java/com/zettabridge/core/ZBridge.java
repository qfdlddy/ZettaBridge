package com.zettabridge.core;

import android.app.Activity;
import android.app.AlarmManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;

import java.io.IOException;

/**
 * JNI surface of libzbridge.so in the launcher's :guest process.
 *
 * <p>Order for an arm32 plugin, all in one :guest process:
 * <ol>
 *   <li>Import: {@link #fixGuestLibrary} on every extracted arm32 library before the import is
 *       marked complete.</li>
 *   <li>Launch, before plugin code runs (before the plugin Application is created):
 *       {@link #activatePlugin} with the plugin root, its targetSdk and its class loader.</li>
 *   <li>Plugin code calls System.loadLibrary; the plugin class loader returns
 *       filesDir/plugins/&lt;pkg&gt;/proxy/lib&lt;name&gt;.so, and that proxy's JNI_OnLoad calls
 *       {@link #onProxyLoaded}.</li>
 * </ol>
 *
 * <p>Runtime layout (fixed): filesDir/zb/sysroot/system/..., filesDir/zb/guest/zbhost,
 * filesDir/zb/guest/lib/{libzbcompat.so,libzbjni.so,...}, filesDir/plugins/&lt;pkg&gt;/lib/lib&lt;name&gt;.so.
 *
 * <p>Failures are final for the process. ART replaces the UnsatisfiedLinkError thrown here by a
 * generic "JNI_ERR returned from JNI_OnLoad" and never calls JNI_OnLoad again for the same path;
 * the guest runtime is started once and never restarted; only one plugin can be active. Show
 * {@link #lastLoadError} (or {@link #loadError}) on screen, and restart the :guest process before
 * retrying or switching plugins.
 */
public final class ZBridge {
    static {
        System.loadLibrary("zbridge");
    }

    private ZBridge() {}

    /**
     * Blocks until the guest exits.
     *
     * @param sysroot directory holding system/bin/linker and system/lib (arm32 bionic)
     * @param argv argv[0] is the absolute path of the guest executable
     * @param envp guest environment ("NAME=VALUE"), or null to pass this process's environment
     * @return the guest exit status, or 128 + signal for a guest crash
     */
    public static native int runExecutable(String sysroot, String[] argv, String[] envp);

    /**
     * Binds this process's guest JNI runtime to one plugin. Calling it again with the same plugin
     * root, targetSdk and class loader is a no-op.
     *
     * @param pluginRoot filesDir/plugins/&lt;pkg&gt; (symlinks are resolved)
     * @param targetSdk the plugin's targetSdkVersion, passed to the guest linker
     * @param classLoader the plugin class loader; retained for the life of the process
     * @throws IllegalStateException when the layout is incomplete, or another plugin, targetSdk
     *     or class loader is already active in this process
     */
    public static native void activatePlugin(String pluginRoot, int targetSdk, ClassLoader classLoader);

    /**
     * Called by libzbproxy.so from JNI_OnLoad. Loads the arm32 library that belongs to the proxy,
     * binds its Java_* exports and runs its JNI_OnLoad on the calling thread. Results are memoized
     * by canonical proxy path.
     *
     * @return the guest JNI version (JNI_VERSION_1_2, 1_4 or 1_6)
     * @throws UnsatisfiedLinkError with the detailed reason
     */
    public static native int onProxyLoaded(String proxyPath);

    /**
     * Called by libzbproxy.so from ANativeActivity_onCreate, which is what the framework's
     * android.app.NativeActivity looks up in the library the manifest names. Builds the 32-bit
     * activity the guest sees and calls the guest's own ANativeActivity_onCreate.
     *
     * @param activity the host ANativeActivity* the framework passed, as a pointer value
     * @param savedState the saved-state block, or 0
     * @param savedStateSize its size in bytes
     * @param proxyPath the proxy library the framework loaded, which names the arm32 library
     * @return true when the guest activity is built and its callbacks are installed; false leaves
     *     the framework's activity untouched, and the reason is in the runtime report
     */
    public static native boolean onNativeActivityCreated(long activity, long savedState, long savedStateSize,
                                                         String proxyPath);

    /** The stored failure message for a proxy path, or null if it did not fail. */
    public static native String loadError(String proxyPath);

    /** The most recent proxy load failure message, or null. */
    public static native String lastLoadError();

    /**
     * Starts persisting the runtime report (see {@link #runtimeReport}) to a file, rewritten
     * atomically whenever the report changes. Call once per :guest process, before plugin code
     * runs: the report then survives a :guest process that dies without a Java or JNI error.
     *
     * @param path absolute file path; its directory must already exist
     * @return true when the first write succeeded and persisting is on
     */
    public static native boolean setReportFile(String path);

    /**
     * What this process recorded about the guest run, as short plain text: the active plugin,
     * proxy loads, guest JNI_OnLoad calls, registered natives, unimplemented host calls (with the
     * first one by name, for example "libGLESv2.so glCreateProgram") and how the guest ended.
     *
     * <p>ROMs that drop third-party logcat output make this the only way to see what happened.
     */
    public static native String runtimeReport();

    /**
     * Applies the arm32 import fixups (absolute DT_NEEDED to basename, DT_TEXTREL marker) in place.
     *
     * @return "unchanged", "changed: ..." or "skipped: reason" for a file that is not an ARM ELF32
     *     shared library
     * @throws IOException for a malformed ELF file or an I/O error
     */
    public static native String fixGuestLibrary(String path) throws IOException;

    /**
     * Strong undefined symbols of an imported plugin's arm32 libraries that neither the plugin
     * itself nor the bundled guest runtime provides.
     *
     * <p>The guest links a library with {@code BIND_NOW}, so a single unresolved strong name stops
     * the whole library from loading and the only error the launcher sees is
     * {@code JNI_ERR returned from JNI_OnLoad}. Running this at import turns that into a list of
     * names before the plugin is launched. The result is conservative: every plugin library and
     * every bundled runtime library counts as a provider, so a reported name is a real gap while an
     * empty result is not a promise that the load will succeed.
     *
     * @param libDir the plugin's extracted lib directory
     * @param guestLibDir filesDir/zb/guest/lib
     * @param sysrootLibDir filesDir/zb/sysroot/system/lib
     * @return {@code "libgame.so: symbol"} entries, at most 32, sorted; empty when nothing is missing
     */
    public static native String[] missingGuestSymbols(String libDir, String guestLibDir, String sysrootLibDir);

    /**
     * Sets ZB_PRECISE_FAULTS in this process's environment (setenv), so a memory fault stops at
     * the exact faulting instruction with registers committed instead of at the end of the
     * translated block. Diagnostic only: roughly 2x slower on integer code. Must be called before
     * the guest runtime/Process for this plugin is constructed, since it is process-lifetime and
     * reads the variable only once.
     */
    public static native void setPreciseFaults(boolean enabled);

    /**
     * Sets ZB_GL_DIAGNOSTICS in this process's environment (setenv), which turns on the GL
     * instrumentation: whole-framebuffer readbacks, per-draw pixel diffs and ASCII frame maps.
     * Debugging only and very slow; off in a normal run. Must be called before the guest JNI
     * runtime of this plugin is constructed, since it reads the variable only once.
     */
    public static native void setGlDiagnostics(boolean enabled);

    /**
     * Sets ZB_UNSAFE_FP in this process's environment (setenv), enabling relaxed IEEE-754
     * floating-point optimizations in the translator for higher game performance. Must be called
     * before the guest runtime/Process is constructed.
     */
    public static native void setUnsafeFp(boolean enabled);

    /** The channel every translated guest posts into. One channel serves every plugin. */
    public static final String CHANNEL_ID = "zb.plugin";
    private static final String CHANNEL_NAME = "ZettaBridge";
    /** Posts a guest notification when the alarm it was given fires (main process). */
    private static final String NOTIFICATION_RELAY = "com.zettabridge.launcher.NotificationRelayReceiver";
    /** Extras of an alarm this class schedules; the receiver reads them back. */
    public static final String EXTRA_ID = "zb.relay.id";
    public static final String EXTRA_TITLE = "zb.relay.title";
    public static final String EXTRA_TEXT = "zb.relay.text";

    /**
     * Posts a notification for a translated guest.
     *
     * <p>Called by libzbridge.so in place of a guest's own notification helper
     * ({@code setNotification(String id, String title, String text, int seconds)}), which runs as
     * plugin Java code on the platform and cannot work there: it builds a {@link PendingIntent}
     * with no mutability flag (rejected by API 31+), posts with no channel (dropped by API 26+) and
     * names a plugin drawable for its icon. The guest's arguments match its own method's, so the
     * notification appears where the guest meant it to.
     *
     * @param activity the guest's activity, the context the notification belongs to
     * @param id the guest's notification id, as text; a malformed one becomes 0
     * @param seconds delay before the notification appears, in seconds; 0 or less posts at once
     */
    public static void notifyGuest(Activity activity, String id, String title, String text, int seconds) {
        if (activity == null) return;
        final int notificationId = parseId(id);
        if (seconds > 0) {
            Intent relay = new Intent();
            relay.setClassName(activity.getPackageName(), NOTIFICATION_RELAY);
            relay.putExtra(EXTRA_ID, notificationId);
            relay.putExtra(EXTRA_TITLE, textOf(title));
            relay.putExtra(EXTRA_TEXT, textOf(text));
            PendingIntent alarm = PendingIntent.getBroadcast(activity, notificationId, relay,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            AlarmManager alarms = (AlarmManager) activity.getSystemService(Context.ALARM_SERVICE);
            if (alarms != null) {
                alarms.set(AlarmManager.ELAPSED_REALTIME_WAKEUP,
                        SystemClock.elapsedRealtime() + seconds * 1000L, alarm);
                return;
            }
        }
        postGuestNotification(activity, notificationId, title, text);
    }

    /** Builds and posts a guest notification now; also the relay receiver's whole job. */
    public static void postGuestNotification(Context context, int id, String title, String text) {
        if (context == null) return;
        NotificationManager manager =
                (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager == null) return;
        manager.createNotificationChannel(
                new NotificationChannel(CHANNEL_ID, CHANNEL_NAME, NotificationManager.IMPORTANCE_HIGH));
        Notification.Builder builder = new Notification.Builder(context, CHANNEL_ID)
                .setContentTitle(textOf(title))
                .setContentText(textOf(text))
                // The guest's own icon is a drawable in its APK, which this package cannot resolve.
                .setSmallIcon(android.R.drawable.ic_popup_reminder)
                .setAutoCancel(true);
        Intent open = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
        if (open != null) {
            open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            builder.setContentIntent(PendingIntent.getActivity(context, id, open,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE));
        }
        manager.notify(id, builder.build());
    }

    private static int parseId(String id) {
        if (id == null) return 0;
        try {
            return Integer.parseInt(id.trim());
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static String textOf(String text) {
        return text == null ? "" : text;
    }
}
