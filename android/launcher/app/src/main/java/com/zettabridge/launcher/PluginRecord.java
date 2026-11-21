package com.zettabridge.launcher;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import java.util.Set;
import java.util.TreeSet;

/** One imported app: filesDir/plugins/<package>/{base.apk, lib/, data/, icon.png, meta.properties}. */
final class PluginRecord {
    static final String ABI_ARM64 = "arm64-v8a";
    static final String ABI_ARM32_V7A = "armeabi-v7a";
    static final String ABI_ARM32 = "armeabi";

    final File dir;
    String packageName;
    String label;
    String launcherActivity;
    /** Optional override for the plugin Application class (e.g. to neutralize a mod's PMS hook). */
    String application;
    String versionName;
    int targetSdk;
    /** ABI extracted into lib/, or null for Java-only/unsupported/legacy imports. */
    String selectedAbi;
    /** Hide the views of bundled ad SDKs: they cannot work for a plugin and only leave banners. */
    boolean hideAds = true;
    /** Diagnostic: stop memory faults at the exact instruction instead of the block end. Slow. */
    boolean preciseFaults = false;
    /** Debugging: GL instrumentation (framebuffer readbacks, per-draw pixel diffs). Very slow. */
    boolean diagnostics = false;
    /** Performance: allow relaxed IEEE-754 FP optimizations (ZB_UNSAFE_FP) in the translator. */
    boolean unsafeFp = false;
    /** Experimental: pass -force-gfx-mt to UnityPlayer to request multi-threaded rendering. */
    boolean unityMt = false;
    /** Every ABI directory found under lib/ in the APK. */
    final Set<String> abis = new TreeSet<>();
    /**
     * Import-time symbol precheck: "libgame.so: symbol" for strong undefined names the guest
     * cannot resolve. A guest library is linked with BIND_NOW, so each of these stops the whole
     * library from loading. Empty when the precheck found nothing.
     */
    final List<String> symbolProblems = new ArrayList<>();

    private Bitmap icon;
    private boolean iconLoaded;

    PluginRecord(File dir) {
        this.dir = dir;
    }

    File apk() {
        return new File(dir, "base.apk");
    }

    File libDir() {
        return new File(dir, "lib");
    }

    File dataDir() {
        return new File(dir, "data");
    }

    File proxyDir() {
        return new File(dir, "proxy");
    }

    File iconFile() {
        return new File(dir, "icon.png");
    }

    boolean hasArm64() {
        return abis.contains(ABI_ARM64);
    }

    boolean has32Bit() {
        return abis.contains(ABI_ARM32) || abis.contains(ABI_ARM32_V7A);
    }

    boolean isTranslated() {
        return ABI_ARM32.equals(selectedAbi) || ABI_ARM32_V7A.equals(selectedAbi);
    }

    boolean isLaunchable() {
        return launcherActivity != null && (abis.isEmpty() || selectedAbi != null);
    }

    String status() {
        if (launcherActivity == null) return "no launcher activity";
        if (ABI_ARM64.equals(selectedAbi)) return "arm64: ready";
        if (isTranslated()) return "32-bit " + selectedAbi + ": ready";
        if (abis.isEmpty()) return "Java only: ready";
        if (has32Bit()) return "32-bit: reimport required";
        return "unsupported ABIs: " + abis;
    }

    /**
     * One line for the library list describing the import-time symbol precheck, or null when it
     * found nothing. Only the first few names are shown; the full list stays in the record.
     */
    String symbolWarning() {
        if (symbolProblems.isEmpty()) return null;
        StringBuilder text = new StringBuilder();
        text.append(symbolProblems.size()).append(symbolProblems.size() == 1
                ? " unresolved native symbol: " : " unresolved native symbols: ");
        final int shown = Math.min(3, symbolProblems.size());
        for (int i = 0; i < shown; ++i) {
            if (i != 0) text.append(", ");
            text.append(symbolProblems.get(i));
        }
        if (symbolProblems.size() > shown) text.append(", ...");
        return text.toString();
    }

    synchronized Bitmap icon() {
        if (!iconLoaded) {
            iconLoaded = true;
            icon = BitmapFactory.decodeFile(iconFile().getPath());
        }
        return icon;
    }

    void save() throws IOException {
        Properties p = new Properties();
        p.setProperty("package", packageName);
        p.setProperty("label", label);
        if (launcherActivity != null) p.setProperty("launcher", launcherActivity);
        if (application != null) p.setProperty("application", application);
        if (versionName != null) p.setProperty("version", versionName);
        p.setProperty("targetSdk", Integer.toString(targetSdk));
        p.setProperty("abis", String.join(",", abis));
        if (selectedAbi != null) p.setProperty("selectedAbi", selectedAbi);
        p.setProperty("hideAds", Boolean.toString(hideAds));
        p.setProperty("preciseFaults", Boolean.toString(preciseFaults));
        p.setProperty("diagnostics", Boolean.toString(diagnostics));
        p.setProperty("unsafeFp", Boolean.toString(unsafeFp));
        p.setProperty("unityMt", Boolean.toString(unityMt));
        if (!symbolProblems.isEmpty()) p.setProperty("symbolProblems", String.join("\n", symbolProblems));
        java.io.ByteArrayOutputStream buffer = new java.io.ByteArrayOutputStream();
        p.store(buffer, "ZettaBridge plugin");
        PluginFiles.copyAtomic(new java.io.ByteArrayInputStream(buffer.toByteArray()), new File(dir, "meta.properties"));
    }

    static PluginRecord load(File dir) {
        File meta = new File(dir, "meta.properties");
        if (!meta.isFile()) return null;
        Properties p = new Properties();
        try (InputStream in = new FileInputStream(meta)) {
            p.load(in);
        } catch (IOException e) {
            return null;
        }
        PluginRecord r = new PluginRecord(dir);
        r.packageName = p.getProperty("package");
        if (r.packageName == null) return null;
        r.label = p.getProperty("label", r.packageName);
        r.launcherActivity = p.getProperty("launcher");
        r.application = p.getProperty("application");
        r.versionName = p.getProperty("version");
        try {
            r.targetSdk = Integer.parseInt(p.getProperty("targetSdk", "0"));
        } catch (NumberFormatException e) {
            r.targetSdk = 0;
        }
        for (String abi : p.getProperty("abis", "").split(",")) {
            if (!abi.isEmpty()) r.abis.add(abi);
        }
        r.selectedAbi = p.getProperty("selectedAbi");
        r.hideAds = !"false".equals(p.getProperty("hideAds"));
        r.preciseFaults = "true".equals(p.getProperty("preciseFaults"));
        r.diagnostics = "true".equals(p.getProperty("diagnostics"));
        r.unsafeFp = "true".equals(p.getProperty("unsafeFp"));
        r.unityMt = "true".equals(p.getProperty("unityMt"));
        final String problems = p.getProperty("symbolProblems");
        if (problems != null && !problems.isEmpty()) {
            for (String problem : problems.split("\n")) {
                if (!problem.isEmpty()) r.symbolProblems.add(problem);
            }
        }
        // Phase 0 arm64 imports are already complete. Old 32-bit records must be reimported so
        // every library passes through the non-atomic ELF fixer before metadata says ready.
        if (r.selectedAbi == null && r.abis.contains(ABI_ARM64)) r.selectedAbi = ABI_ARM64;
        return r;
    }
}
