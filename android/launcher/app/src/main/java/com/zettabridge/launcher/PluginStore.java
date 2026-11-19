package com.zettabridge.launcher;

import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.os.Build;
import android.os.Parcel;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

import com.zettabridge.core.ZBridge;

/** Imported plugins on disk. Import is staged and published only after every native fixup succeeds. */
final class PluginStore {
    static final int ARCHIVE_FLAGS = PackageManager.GET_ACTIVITIES | PackageManager.GET_PROVIDERS
            | PackageManager.GET_SERVICES | PackageManager.GET_RECEIVERS | PackageManager.GET_META_DATA;
    private static final int PACKAGE_CACHE_MAGIC = 0x5A425049;
    private static final int PACKAGE_CACHE_VERSION = 1;
    private static final String PACKAGE_CACHE_NAME = "package-info.parcel";
    private static final java.util.regex.Pattern PACKAGE_NAME_PATTERN =
            java.util.regex.Pattern.compile("^[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+$");

    private PluginStore() {}

    static boolean isValidPackageName(String packageName) {
        return packageName != null && packageName.length() <= 255
                && PACKAGE_NAME_PATTERN.matcher(packageName).matches();
    }

    static File root(Context c) {
        return new File(c.getFilesDir(), "plugins");
    }

    static List<PluginRecord> list(Context c) {
        List<PluginRecord> out = new ArrayList<>();
        File[] dirs = root(c).listFiles();
        if (dirs == null) return out;
        for (File dir : dirs) {
            PluginRecord r = dir.isDirectory() ? PluginRecord.load(dir) : null;
            if (r != null) out.add(r);
        }
        out.sort((a, b) -> a.label.compareToIgnoreCase(b.label));
        return out;
    }

    static PluginRecord find(Context c, String packageName) {
        if (!isValidPackageName(packageName)) return null;
        return PluginRecord.load(new File(root(c), packageName));
    }

    /** Application info of an imported APK, with paths pointing at our copy. */
    static ApplicationInfo applicationInfo(PackageInfo info, PluginRecord r) {
        ApplicationInfo ai = info.applicationInfo;
        ai.sourceDir = r.apk().getPath();
        ai.publicSourceDir = ai.sourceDir;
        ai.nativeLibraryDir = r.libDir().getPath();
        ai.dataDir = r.dataDir().getPath();
        return ai;
    }

    static PluginRecord importApk(Context c, Uri uri) throws IOException {
        File tmp = File.createTempFile("zb-import-", ".apk", c.getCacheDir());
        try (InputStream in = c.getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("cannot open " + uri);
            copy(in, tmp);
        }
        PackageManager pm = c.getPackageManager();
        PackageInfo info = pm.getPackageArchiveInfo(tmp.getPath(), ARCHIVE_FLAGS);
        if (info == null || info.applicationInfo == null) {
            tmp.delete();
            throw new IOException("not an installable APK (split bundles are not supported)");
        }
        if (!isValidPackageName(info.packageName) || info.packageName.equals(c.getPackageName())) {
            tmp.delete();
            throw new IOException("refusing to import " + info.packageName);
        }

        File plugins = root(c);
        if (!plugins.isDirectory() && !plugins.mkdirs()) throw new IOException("cannot create " + plugins);
        File target = new File(plugins, info.packageName);
        File staging = new File(plugins, "." + info.packageName + ".importing");
        PluginFiles.deleteRecursive(staging);
        if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
        try {
            PluginRecord r = new PluginRecord(staging);
            File apk = r.apk();
            PluginFiles.moveOrCopy(tmp, apk);
            // Android 14+ refuses to load dex code from writable files.
            if (!apk.setReadOnly()) throw new IOException("cannot make APK read-only: " + apk);

            r.packageName = info.packageName;
            r.versionName = info.versionName;
            ApplicationInfo ai = applicationInfo(info, r);
            r.targetSdk = ai.targetSdkVersion;
            r.abis.addAll(PluginFiles.scanAbis(apk));
            r.selectedAbi = PluginFiles.selectAbi(r.abis);
            if (r.selectedAbi != null) {
                final boolean translated = r.isTranslated();
                if (translated) {
                    RuntimeBundle.install(c);
                    r.unsafeFp = true;
                }
                PluginFiles.extractLibraries(apk, r.libDir(), r.selectedAbi, library -> {
                    if (!translated) return;
                    String result = ZBridge.fixGuestLibrary(library.getPath());
                    if (result == null || result.startsWith("skipped:")) {
                        throw new IOException("cannot prepare " + library.getName() + ": " + result);
                    }
                });
            } else if (!r.libDir().mkdirs()) {
                throw new IOException("cannot create " + r.libDir());
            }
            CharSequence label = ai.loadLabel(pm);
            r.label = label != null && label.length() > 0 ? label.toString() : r.packageName;
            saveIcon(ai.loadIcon(pm), r.iconFile());
            try {
                r.launcherActivity = ManifestReader.findLauncherActivity(pm.getResourcesForApplication(ai), r.packageName);
            } catch (PackageManager.NameNotFoundException e) {
                r.launcherActivity = null;
            }
            if ("cc.binmt.signature.PmsHookApplication".equals(ai.className)) {
                r.application = android.app.Application.class.getName();
            }
            if ("com.fiveplay.mod.RMS.Recovery".equals(r.launcherActivity) && hasActivity(info, "com.unity3d.player.UnityPlayerActivity")) {
                r.launcherActivity = "com.unity3d.player.UnityPlayerActivity";
            }
            // Completion marker: never write this until extraction and all in-place fixups pass.
            r.save();
            savePackageInfoCache(r, info);
            PluginFiles.replaceDirectoryKeeping(staging, target, "data");
            PluginRecord published = PluginRecord.load(target);
            if (published == null) throw new IOException("cannot read published metadata for " + info.packageName);
            return published;
        } finally {
            tmp.delete();
            PluginFiles.deleteRecursive(staging);
        }
    }

    static PackageInfo loadPackageInfo(Context c, PluginRecord r) {
        File apk = r.apk();
        File cache = new File(r.dir, PACKAGE_CACHE_NAME);
        if (cache.isFile() && apk.isFile()) {
            try (DataInputStream in = new DataInputStream(new FileInputStream(cache))) {
                if (in.readInt() == PACKAGE_CACHE_MAGIC
                        && in.readInt() == PACKAGE_CACHE_VERSION
                        && in.readInt() == ARCHIVE_FLAGS
                        && in.readLong() == apk.length()
                        && in.readLong() == apk.lastModified()
                        && Build.FINGERPRINT.equals(in.readUTF())) {
                    int len = in.readInt();
                    if (len > 0 && len <= (16 << 20)) {
                        byte[] bytes = new byte[len];
                        in.readFully(bytes);
                        Parcel parcel = Parcel.obtain();
                        try {
                            parcel.unmarshall(bytes, 0, bytes.length);
                            parcel.setDataPosition(0);
                            PackageInfo cached = PackageInfo.CREATOR.createFromParcel(parcel);
                            if (cached != null && cached.applicationInfo != null) {
                                return cached;
                            }
                        } finally {
                            parcel.recycle();
                        }
                    }
                }
            } catch (Throwable ignored) {
                // Fall back to parsing the archive if the parcel format changed or is corrupt.
            }
        }
        PackageInfo parsed = c.getPackageManager().getPackageArchiveInfo(apk.getPath(), ARCHIVE_FLAGS);
        if (parsed != null && parsed.applicationInfo != null) {
            savePackageInfoCache(r, parsed);
        }
        return parsed;
    }

    private static void savePackageInfoCache(PluginRecord r, PackageInfo info) {
        File apk = r.apk();
        if (!apk.isFile() || info == null) return;
        Parcel parcel = Parcel.obtain();
        byte[] parcelBytes;
        try {
            info.writeToParcel(parcel, 0);
            parcelBytes = parcel.marshall();
        } catch (Throwable ignored) {
            return;
        } finally {
            parcel.recycle();
        }
        try {
            ByteArrayOutputStream baos = new ByteArrayOutputStream(parcelBytes.length + 128);
            DataOutputStream out = new DataOutputStream(baos);
            out.writeInt(PACKAGE_CACHE_MAGIC);
            out.writeInt(PACKAGE_CACHE_VERSION);
            out.writeInt(ARCHIVE_FLAGS);
            out.writeLong(apk.length());
            out.writeLong(apk.lastModified());
            out.writeUTF(Build.FINGERPRINT);
            out.writeInt(parcelBytes.length);
            out.write(parcelBytes);
            out.flush();
            PluginFiles.copyAtomic(new ByteArrayInputStream(baos.toByteArray()), new File(r.dir, PACKAGE_CACHE_NAME));
        } catch (IOException ignored) {
            // Cache write is best-effort.
        }
    }

    static void delete(PluginRecord r) {
        PluginFiles.deleteRecursive(r.dir);
    }

    private static boolean hasActivity(PackageInfo info, String name) {
        if (info == null || info.activities == null) return false;
        for (ActivityInfo a : info.activities) {
            if (name.equals(a.name)) return true;
        }
        return false;
    }

    /** Extracts a 5play/mod bundled assets/obb payload once if the plugin's OBB directory is empty. */
    static void ensureBundledObb(Context c, PluginRecord r, PackageInfo info) {
        if (r == null || info == null) return;
        File obbRoot = c.getObbDir();
        if (obbRoot == null) return;
        File obbDir = new File(obbRoot, r.packageName);
        File[] existing = obbDir.listFiles();
        if (existing != null) {
            for (File f : existing) {
                if (f.isFile() && f.getName().endsWith(".obb") && f.length() > 0) return;
            }
        }
        File apk = r.apk();
        if (!apk.isFile()) return;
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(apk)) {
            java.util.zip.ZipEntry entry = zip.getEntry("assets/obb");
            if (entry == null || entry.isDirectory() || entry.getSize() <= 0) return;
            if (!obbDir.isDirectory() && !obbDir.mkdirs()) return;
            String obbName = "patch." + info.getLongVersionCode() + "." + c.getPackageName() + ".obb";
            File out = new File(obbDir, obbName);
            if (out.isFile() && out.length() == entry.getSize()) return;
            try (InputStream in = zip.getInputStream(entry)) {
                PluginFiles.copyAtomic(in, out);
            }
        } catch (IOException ignored) {
            // Best-effort extraction; missing OBB will be visible in the runtime report.
        }
    }

    private static void saveIcon(Drawable d, File out) {
        if (d == null) return;
        int size = 192;
        Bitmap bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bmp);
        d.setBounds(0, 0, size, size);
        d.draw(canvas);
        try (OutputStream o = new FileOutputStream(out)) {
            bmp.compress(Bitmap.CompressFormat.PNG, 100, o);
        } catch (IOException ignored) {
            // no icon is fine
        }
    }

    private static void copy(InputStream in, File out) throws IOException {
        try (OutputStream o = new FileOutputStream(out)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
        }
    }

}
