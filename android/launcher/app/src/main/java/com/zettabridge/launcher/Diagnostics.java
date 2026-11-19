package com.zettabridge.launcher;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.util.Log;

import com.zettabridge.core.ZBridge;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Error reports that do not depend on logcat: some ROMs (OxygenOS) drop third-party app logs.
 * The full stack trace goes to the clipboard and to
 * /sdcard/Android/data/com.zettabridge.launcher/files/zb-errors.txt (appended).
 *
 * <p>The translator's own runtime report lands next to it as zb-runtime-report.txt, rewritten by
 * the :guest process whenever it changes, so it survives a :guest process that dies silently.
 */
final class Diagnostics {
    private static final String TAG = "zb-launcher";
    private static final String REPORT_NAME = "zb-runtime-report.txt";
    private static final String ERROR_NAME = "zb-errors.txt";
    private static final int MAX_ERROR_TAIL_CHARS = 8192;
    private static volatile Boolean cachedDisabled;

    private Diagnostics() {}

    static boolean isDisabled(Context context) {
        if (BuildConfig.DISABLE_DIAGNOSTICS) return true;
        Boolean cached = cachedDisabled;
        if (cached != null) return cached;
        boolean bundled = false;
        if (context != null) {
            try (java.io.InputStream in = context.getAssets().open("bundled/bundle-info.properties")) {
                bundled = in != null;
            } catch (IOException ignored) {
                bundled = false;
            }
        }
        cachedDisabled = bundled;
        return bundled;
    }

    /**
     * Where the diagnostic files live.
     *
     * <p>Release builds use the app's external files directory, which is what a user can reach
     * without a shell. A debuggable build prefers internal storage instead: `adb shell` cannot
     * read another app's {@code Android/data} directory on Android 11+, while {@code adb shell
     * run-as <pkg>} can read {@code /data/data/<pkg>/files}. A performance or bring-up session
     * needs the report on every run, so debug builds put it where the shell can reach it.
     */
    private static File diagnosticDir(Context context) {
        boolean debuggable =
                (context.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        if (debuggable) {
            File internal = context.getFilesDir();
            if (internal != null) return internal;
        }
        return context.getExternalFilesDir(null);
    }

    /** The runtime report file, or null when no private directory is available. */
    static File runtimeReportFile(Context context) {
        File dir = diagnosticDir(context);
        return dir == null ? null : new File(dir, REPORT_NAME);
    }

    /** The Java crash/error file, or null when no private directory is available. */
    static File errorFile(Context context) {
        File dir = diagnosticDir(context);
        return dir == null ? null : new File(dir, ERROR_NAME);
    }

    /**
     * Makes the :guest process persist its runtime report. Called once, before plugin code runs;
     * failures are not fatal, the run simply leaves no report behind.
     */
    static void startRuntimeReport(Context context) {
        if (isDisabled(context)) return;
        File file = runtimeReportFile(context);
        if (file == null) {
            Log.w(TAG, "no external files dir: the runtime report is not persisted");
            return;
        }
        try {
            if (!ZBridge.setReportFile(file.getAbsolutePath())) {
                Log.w(TAG, "cannot persist the runtime report to " + file);
            }
        } catch (Throwable t) {
            Log.w(TAG, "cannot start the runtime report: " + t);
        }
    }

    /** The last run report, or null when no run has written one. */
    static String readRuntimeReport(Context context) {
        return readTail(runtimeReportFile(context), -1);
    }

    /** Recent Java crash/error logs from zb-errors.txt, or null when absent. */
    static String readErrors(Context context) {
        return readTail(errorFile(context), MAX_ERROR_TAIL_CHARS);
    }

    private static String readTail(File file, int maxChars) {
        if (file == null || !file.isFile()) return null;
        try {
            String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            if (text.isEmpty()) return null;
            if (maxChars > 0 && text.length() > maxChars) {
                return "...\n" + text.substring(text.length() - maxChars);
            }
            return text;
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "cannot read diagnostic file " + file + ": " + e);
            return null;
        }
    }

    /** Copies text to the clipboard under `label`; false when the clipboard is unavailable. */
    static boolean copy(Context context, String label, String text) {
        try {
            ClipboardManager cm = (ClipboardManager) context.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm == null) return false;
            cm.setPrimaryClip(ClipData.newPlainText(label, text));
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot copy to the clipboard: " + e);
            return false;
        }
    }

    /** Records an error; copies it to the clipboard when copy is true. Returns the report text. */
    static String report(Context context, String what, Throwable t, boolean copy) {
        if (isDisabled(context)) return "";
        String stamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());
        String text = stamp + " " + what + "\n" + Log.getStackTraceString(t);
        Log.e(TAG, what, t);
        appendToFile(context, text);
        if (copy) copy(context, "ZettaBridge error", text);
        return text;
    }

    /** Uncaught exceptions of a process are appended to the error file before the default handler runs. */
    static void installCrashRecorder(Context context) {
        if (isDisabled(context)) return;
        final Context app = context.getApplicationContext() != null ? context.getApplicationContext() : context;
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, t) -> {
            try {
                appendToFile(app, new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date())
                        + " uncaught in thread " + thread.getName() + "\n" + Log.getStackTraceString(t));
            } catch (Throwable ignored) {
                // never mask the original crash
            }
            if (previous != null) previous.uncaughtException(thread, t);
        });
    }

    private static void appendToFile(Context context, String text) {
        try {
            File file = errorFile(context);
            if (file == null) return;
            try (FileOutputStream out = new FileOutputStream(file, true)) {
                out.write((text + "\n").getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception e) {
            Log.w(TAG, "cannot write the error file: " + e);
        }
    }
}
