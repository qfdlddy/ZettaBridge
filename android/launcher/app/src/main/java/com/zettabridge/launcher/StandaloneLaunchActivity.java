package com.zettabridge.launcher;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Properties;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Direct entry point for standalone builds: ensures bundled game assets and runtime are staged
 * on first launch with no extra UI chrome, then launches the game immediately.
 */
public class StandaloneLaunchActivity extends Activity {
    private static final String DEFAULT_PACKAGE = "com.kiwiwalks.witchspring3";
    private static final String DEFAULT_LAUNCHER = "com.unity3d.player.UnityPlayerActivity";
    private static final String DEFAULT_VERSION = "1.42";
    private static final int DEFAULT_VERSION_CODE = 107;
    private static final int DEFAULT_TARGET_SDK = 29;

    private static final String BUNDLE_INFO_ASSET = "bundled/bundle-info.properties";
    private static final String BUNDLE_APK_ASSET = "bundled/base.apk";
    private static final String BUNDLE_OBB_ASSET = "bundled/patch.obb";
    private static final String BUNDLE_MARKER = ".standalone-version";

    private static final ExecutorService EXECUTOR = Executors.newSingleThreadExecutor();
    private boolean launched;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        View blank = new View(this);
        blank.setBackgroundColor(Color.BLACK);
        setContentView(blank);

        final Context app = getApplicationContext();
        final Properties bundleInfo;
        try {
            bundleInfo = readBundleInfo(app);
        } catch (IOException e) {
            fail(e);
            return;
        }
        final String pluginPackage = pluginPackage(app, bundleInfo);
        if (!PluginStore.isValidPackageName(pluginPackage)) {
            fail(new IOException("Invalid bundled package name: " + pluginPackage));
            return;
        }

        if (isAlreadyInstalled(app, bundleInfo)) {
            startGame(app, pluginPackage);
            return;
        }

        final Handler main = new Handler(Looper.getMainLooper());
        EXECUTOR.execute(() -> {
            try {
                installStandaloneAssets(app, bundleInfo);
                main.post(() -> startGame(app, pluginPackage));
            } catch (Throwable t) {
                main.post(() -> fail(t));
            }
        });
    }

    @Override
    protected void onDestroy() {
        // Keep the app-scoped bootstrap executor alive so an in-progress first-launch extraction
        // finishes cleanly even if the launcher activity is recreated or backgrounded.
        super.onDestroy();
    }

    private void startGame(Context app, String pluginPackage) {
        if (launched || isFinishing() || isDestroyed()) return;
        launched = true;
        String active = ActivePlugin.of(app);
        boolean clearTask = !pluginPackage.equals(active);
        Intent intent = GuestLaunchActivity.intent(this, pluginPackage, clearTask);
        startActivity(intent);
        overridePendingTransition(0, 0);
        finish();
    }

    private void fail(Throwable t) {
        if (isFinishing() || isDestroyed()) return;
        Diagnostics.report(this, "standalone bootstrap failed", t, false);
        Toast.makeText(this, "Launch failed: " + t.getMessage(), Toast.LENGTH_LONG).show();
        finish();
    }

    private static Properties readBundleInfo(Context app) throws IOException {
        Properties props = new Properties();
        try (InputStream in = app.getAssets().open(BUNDLE_INFO_ASSET, AssetManager.ACCESS_BUFFER);
             java.io.InputStreamReader reader = new java.io.InputStreamReader(in, StandardCharsets.UTF_8)) {
            props.load(reader);
        }
        return props;
    }

    private static String pluginPackage(Context app, Properties bundleInfo) {
        String pkg = bundleInfo.getProperty("package", "").trim();
        if (!pkg.isEmpty()) return pkg;
        String hostPkg = app.getPackageName();
        return hostPkg != null && !hostPkg.isEmpty() ? hostPkg : DEFAULT_PACKAGE;
    }

    private static boolean isAlreadyInstalled(Context app, Properties bundleInfo) {
        String version = bundleInfo.getProperty("version", "").trim();
        if (version.isEmpty()) return false;
        String pluginPackage = pluginPackage(app, bundleInfo);
        PluginRecord existing = PluginStore.find(app, pluginPackage);
        if (existing == null || !existing.apk().isFile() || !existing.libDir().isDirectory()) return false;
        File marker = new File(existing.dir, BUNDLE_MARKER);
        if (!marker.isFile()) return false;
        try {
            String installed = new String(Files.readAllBytes(marker.toPath()), StandardCharsets.US_ASCII).trim();
            if (!version.equals(installed)) return false;
        } catch (IOException e) {
            return false;
        }
        long expectedObbSize = parseLong(bundleInfo.getProperty("obbSize"), -1L);
        if (expectedObbSize > 0) {
            File obbFile = targetObbFile(app, bundleInfo);
            if (obbFile == null || !obbFile.isFile() || obbFile.length() != expectedObbSize) return false;
        }
        return RuntimeBundle.proxyLibrary(app).isFile();
    }

    private static synchronized void installStandaloneAssets(Context app, Properties bundleInfo)
            throws IOException {
        if (isAlreadyInstalled(app, bundleInfo)) return;

        RuntimeBundle.install(app);

        String pluginPackage = pluginPackage(app, bundleInfo);
        AssetManager assets = app.getAssets();
        File pluginsRoot = PluginStore.root(app);
        if (!pluginsRoot.isDirectory() && !pluginsRoot.mkdirs()) {
            throw new IOException("cannot create " + pluginsRoot);
        }
        File target = new File(pluginsRoot, pluginPackage);
        File staging = new File(pluginsRoot, "." + pluginPackage + ".staging");
        PluginFiles.deleteRecursive(staging);
        if (!staging.mkdirs()) throw new IOException("cannot create " + staging);

        boolean ok = false;
        try {
            PluginRecord record = new PluginRecord(staging);
            record.packageName = pluginPackage;
            CharSequence hostLabel = app.getApplicationInfo().loadLabel(app.getPackageManager());
            record.label = hostLabel != null && hostLabel.length() > 0 ? hostLabel.toString() : pluginPackage;
            record.launcherActivity = bundleInfo.getProperty("launcher", DEFAULT_LAUNCHER).trim();
            if (record.launcherActivity.isEmpty()) record.launcherActivity = DEFAULT_LAUNCHER;
            String appOverride = bundleInfo.getProperty("application", android.app.Application.class.getName()).trim();
            record.application = appOverride.isEmpty() ? android.app.Application.class.getName() : appOverride;
            record.versionName = bundleInfo.getProperty("versionName", DEFAULT_VERSION).trim();
            record.targetSdk = (int) parseLong(bundleInfo.getProperty("targetSdk"), DEFAULT_TARGET_SDK);
            String selectedAbi = bundleInfo.getProperty("selectedAbi", PluginRecord.ABI_ARM32_V7A).trim();
            if (selectedAbi.isEmpty()) selectedAbi = PluginRecord.ABI_ARM32_V7A;
            record.abis.add(selectedAbi);
            record.selectedAbi = selectedAbi;
            record.hideAds = !"false".equalsIgnoreCase(bundleInfo.getProperty("hideAds", "true").trim());
            record.preciseFaults = "true".equalsIgnoreCase(bundleInfo.getProperty("preciseFaults", "false").trim());
            record.diagnostics = "true".equalsIgnoreCase(bundleInfo.getProperty("diagnostics", "false").trim());
            record.unsafeFp = "true".equalsIgnoreCase(bundleInfo.getProperty("unsafeFp", "true").trim());
            record.unityMt = "true".equalsIgnoreCase(bundleInfo.getProperty("unityMt", "true").trim());

            try (InputStream in = assets.open(BUNDLE_APK_ASSET, AssetManager.ACCESS_STREAMING)) {
                PluginFiles.copyAtomic(in, record.apk());
            }
            if (!record.apk().setReadOnly()) {
                throw new IOException("cannot make APK read-only: " + record.apk());
            }

            File libDir = record.libDir();
            if (!libDir.mkdirs()) throw new IOException("cannot create " + libDir);
            String libsProp = bundleInfo.getProperty("libs", "");
            for (String libName : libsProp.split(",")) {
                String trimmed = libName.trim();
                if (trimmed.isEmpty()) continue;
                if (!PluginFiles.safeRelativePath(trimmed) || trimmed.indexOf('/') >= 0) {
                    throw new IOException("unsafe bundled library name: " + trimmed);
                }
                File libOut = new File(libDir, trimmed);
                try (InputStream in = assets.open("bundled/lib/" + trimmed, AssetManager.ACCESS_STREAMING)) {
                    PluginFiles.copyAtomic(in, libOut);
                }
                if (!libOut.setReadOnly()) {
                    throw new IOException("cannot make native library read-only: " + libOut);
                }
            }

            long expectedObbSize = parseLong(bundleInfo.getProperty("obbSize"), -1L);
            if (expectedObbSize > 0) {
                File obbOut = targetObbFile(app, bundleInfo);
                if (obbOut != null && (!obbOut.isFile() || obbOut.length() != expectedObbSize)) {
                    try (InputStream in = assets.open(BUNDLE_OBB_ASSET, AssetManager.ACCESS_STREAMING)) {
                        PluginFiles.copyAtomic(in, obbOut);
                    }
                }
            }

            record.save();
            String version = bundleInfo.getProperty("version", "").trim();
            try (OutputStream out = new FileOutputStream(new File(staging, BUNDLE_MARKER))) {
                out.write((version + "\n").getBytes(StandardCharsets.US_ASCII));
            }
            PluginFiles.replaceDirectoryKeeping(staging, target, "data");
            PluginRecord published = PluginStore.find(app, pluginPackage);
            if (published != null) {
                // Pre-populate package-info.parcel so the :guest process starts warm.
                PluginStore.loadPackageInfo(app, published);
            }
            ok = true;
        } finally {
            if (!ok) PluginFiles.deleteRecursive(staging);
        }
    }

    private static File targetObbFile(Context app, Properties bundleInfo) {
        File obbRoot = app.getObbDir();
        if (obbRoot == null) return null;
        File dir = new File(obbRoot, pluginPackage(app, bundleInfo));
        long versionCode = parseLong(bundleInfo.getProperty("versionCode"), DEFAULT_VERSION_CODE);
        String name = "patch." + versionCode + "." + app.getPackageName() + ".obb";
        return new File(dir, name);
    }

    private static long parseLong(String value, long fallback) {
        if (value == null || value.trim().isEmpty()) return fallback;
        try {
            return Long.parseLong(value.trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }
}
