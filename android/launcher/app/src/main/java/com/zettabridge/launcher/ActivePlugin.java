package com.zettabridge.launcher;

import android.app.ActivityManager;
import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * Which plugin the :guest process currently holds, readable from the launcher process.
 *
 * The guest runtime is process-lifetime, so one :guest process serves one plugin, and the
 * launcher must know which one before it routes a launch. The guest writes the name here as it
 * loads; the record counts only while that process is alive, because a dead process leaves it
 * behind.
 */
final class ActivePlugin {
    private static final String TAG = "zb-launcher";
    private static final String FILE = "active-plugin";

    private ActivePlugin() {}

    /** Called in :guest once a plugin is loaded. */
    static void record(Context context, String packageName) {
        File file = new File(context.getFilesDir(), FILE);
        String payload = android.os.Process.myPid() + ":" + packageName;
        try {
            PluginFiles.copyAtomic(
                    new java.io.ByteArrayInputStream(payload.getBytes(StandardCharsets.UTF_8)), file);
        } catch (IOException e) {
            Log.w(TAG, "cannot record the active plugin: " + e);
        }
    }

    /**
     * The plugin the live :guest process holds, or null if no guest process is running. A guest
     * process with no record counts as holding an unknown plugin, which is never a match.
     */
    static String of(Context context) {
        int livePid = guestPid(context);
        if (livePid < 0) return null;
        File file = new File(context.getFilesDir(), FILE);
        try (RandomAccessFile in = new RandomAccessFile(file, "r")) {
            byte[] bytes = new byte[(int) Math.min(in.length(), 512)];
            in.readFully(bytes);
            String raw = new String(bytes, StandardCharsets.UTF_8).trim();
            if (raw.isEmpty()) return "";
            int colon = raw.indexOf(':');
            if (colon > 0) {
                try {
                    int recordedPid = Integer.parseInt(raw.substring(0, colon));
                    if (recordedPid != livePid) return "";
                    return raw.substring(colon + 1);
                } catch (NumberFormatException ignored) {
                    return "";
                }
            }
            return raw;
        } catch (IOException e) {
            return "";  // running, but unknown: not equal to any package name
        }
    }

    private static int guestPid(Context context) {
        try {
            ActivityManager manager = context.getSystemService(ActivityManager.class);
            List<ActivityManager.RunningAppProcessInfo> running =
                    manager != null ? manager.getRunningAppProcesses() : null;
            if (running == null) return -1;
            for (ActivityManager.RunningAppProcessInfo info : running) {
                if (info.processName != null && info.processName.endsWith(ZbApplication.GUEST_SUFFIX)) {
                    return info.pid;
                }
            }
            return -1;
        } catch (RuntimeException e) {
            return -1;
        }
    }

    static boolean guestIsRunning(Context context) {
        return guestPid(context) >= 0;
    }

    /** Ends the :guest process, whatever plugin it holds. */
    static void endGuestProcess(Context context) {
        try {
            ActivityManager manager = context.getSystemService(ActivityManager.class);
            List<ActivityManager.RunningAppProcessInfo> running =
                    manager != null ? manager.getRunningAppProcesses() : null;
            if (running == null) return;
            for (ActivityManager.RunningAppProcessInfo info : running) {
                if (info.processName != null && info.processName.endsWith(ZbApplication.GUEST_SUFFIX)) {
                    if (!BuildConfig.DISABLE_DIAGNOSTICS) {
                        Log.i(TAG, "ending the guest process " + info.pid + " to switch plugins");
                    }
                    android.os.Process.killProcess(info.pid);
                }
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot end the guest process: " + e);
        }
    }
}
