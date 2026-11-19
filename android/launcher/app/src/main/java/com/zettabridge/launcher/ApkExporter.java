package com.zettabridge.launcher;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.net.Uri;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * Exports an imported 32-bit plugin into a standalone, v1/v2/v3-signed arm64 APK directly on the
 * device, using the running ZettaBridge APK as the native/Java template and pulling package name,
 * desktop label, version, and launcher icons from the imported APK.
 */
final class ApkExporter {
    interface ProgressListener {
        void onStage(String message);
    }

    private static final String STANDALONE_LIB_ASSET = "assets/zb-standalone/libzbridge.so";
    private static final int IO_BUFFER_SIZE = 256 * 1024;

    private ApkExporter() {}

    static void export(
            Context context,
            PluginRecord record,
            String exportPackage,
            String exportLabel,
            Uri destinationUri,
            ProgressListener progress
    ) throws IOException, GeneralSecurityException {
        if (record == null || !record.apk().isFile()) {
            throw new IOException("Plugin APK is missing");
        }
        if (exportPackage == null || exportPackage.trim().isEmpty()) {
            throw new IOException("Export package name cannot be empty");
        }
        if (exportLabel == null || exportLabel.trim().isEmpty()) {
            throw new IOException("Export app name cannot be empty");
        }
        exportPackage = exportPackage.trim();
        exportLabel = exportLabel.trim();
        if (!PluginStore.isValidPackageName(exportPackage)) {
            throw new IOException("Invalid export package name: " + exportPackage);
        }

        PackageInfo pkgInfo = PluginStore.loadPackageInfo(context, record);
        if (pkgInfo == null) {
            throw new IOException("Cannot parse imported plugin package info");
        }
        long longVer = pkgInfo.getLongVersionCode();
        int versionCode = longVer > 0 && longVer <= Integer.MAX_VALUE ? (int) longVer : 1;
        String versionName = (record.versionName != null && !record.versionName.isEmpty())
                ? record.versionName
                : (pkgInfo.versionName != null ? pkgInfo.versionName : "1.0");

        File workDir = java.nio.file.Files.createTempDirectory(context.getCacheDir().toPath(), "zb-export-").toFile();

        File unsignedApk = new File(workDir, "unsigned.apk");
        File signedApk = new File(workDir, "signed.apk");
        try {
            if (progress != null) progress.onStage("Preparing manifest and icons...");
            String selfApkPath = context.getApplicationInfo().sourceDir;
            byte[] patchedManifest;
            Map<String, byte[]> replacementIcons;

            try (ZipFile templateZip = new ZipFile(selfApkPath);
                 ZipFile sourceZip = new ZipFile(record.apk())) {
                ZipEntry manifestEntry = templateZip.getEntry("AndroidManifest.xml");
                if (manifestEntry == null) {
                    throw new IOException("Template APK missing AndroidManifest.xml");
                }
                byte[] origManifest = readZipEntryBytes(templateZip, manifestEntry, 16 << 20);
                patchedManifest = AxmlEditor.patchManifest(
                        origManifest, exportPackage, exportLabel, versionCode, versionName);

                ZipEntry arscEntry = templateZip.getEntry("resources.arsc");
                byte[] arscBytes = arscEntry != null ? readZipEntryBytes(templateZip, arscEntry, 32 << 20) : null;
                Map<String, ArscIconFinder.IconSlot> iconSlots =
                        ArscIconFinder.findIconSlots(arscBytes, R.mipmap.ic_launcher, R.mipmap.ic_launcher_round);
                replacementIcons = buildReplacementIcons(context, record, pkgInfo, sourceZip, iconSlots);
            }

            if (progress != null) progress.onStage("Packaging game bundle...");
            byte[] buf = new byte[IO_BUFFER_SIZE];

            try (ZipFile templateZip = new ZipFile(selfApkPath);
                 ZipFile sourceZip = new ZipFile(record.apk());
                 ZipOutputStream zos = new ZipOutputStream(
                         new BufferedOutputStream(new FileOutputStream(unsignedApk), IO_BUFFER_SIZE))) {
                ZipEntry standaloneLibEntry = templateZip.getEntry(STANDALONE_LIB_ASSET);
                Set<String> writtenNames = new HashSet<>();

                // 1. Copy template APK entries, patching manifest, icons, and optional zero-telemetry core.
                Enumeration<? extends ZipEntry> entries = templateZip.entries();
                while (entries.hasMoreElements()) {
                    ZipEntry entry = entries.nextElement();
                    String name = entry.getName();
                    if (entry.isDirectory()) continue;
                    if (name.startsWith("META-INF/")) continue;
                    if (name.startsWith("assets/zb-standalone/")) continue;
                    if (name.startsWith("assets/bundled/")) continue;
                    if (!writtenNames.add(name)) continue;

                    if ("AndroidManifest.xml".equals(name)) {
                        writeDeflatedBytes(zos, name, patchedManifest);
                    } else if (replacementIcons.containsKey(name)) {
                        writeStoredBytes(zos, name, replacementIcons.get(name));
                    } else if ("lib/arm64-v8a/libzbridge.so".equals(name) && standaloneLibEntry != null) {
                        copyStoredEntry(templateZip, standaloneLibEntry, name, zos, buf);
                    } else if (entry.getMethod() == ZipEntry.STORED) {
                        copyStoredEntry(templateZip, entry, name, zos, buf);
                    } else {
                        copyDeflatedEntry(templateZip, entry, name, zos, buf);
                    }
                }

                // 2. Build slim base.apk (without lib/armeabi-v7a/*.so or assets/obb) and stream it STORED.
                File slimApkFile = new File(workDir, "slim-base.apk");
                ZipEntry obbEntry = null;
                try {
                    try (ZipOutputStream slimOut = new ZipOutputStream(
                            new BufferedOutputStream(new FileOutputStream(slimApkFile), IO_BUFFER_SIZE))) {
                        Enumeration<? extends ZipEntry> srcEntries = sourceZip.entries();
                        Set<String> slimNames = new HashSet<>();
                        while (srcEntries.hasMoreElements()) {
                            ZipEntry se = srcEntries.nextElement();
                            String name = se.getName();
                            if (se.isDirectory()) continue;
                            if ("assets/obb".equals(name)) {
                                obbEntry = se;
                                continue;
                            }
                            if (name.startsWith("lib/")) {
                                String[] parts = name.split("/");
                                String stripAbi = record.selectedAbi != null ? record.selectedAbi : PluginRecord.ABI_ARM32_V7A;
                                if (parts.length == 3 && stripAbi.equals(parts[1]) && parts[2].endsWith(".so")) {
                                    continue;
                                }
                            }
                            if (!slimNames.add(name)) continue;
                            if (se.getMethod() == ZipEntry.STORED) {
                                copyStoredEntry(sourceZip, se, name, slimOut, buf);
                            } else {
                                copyDeflatedEntry(sourceZip, se, name, slimOut, buf);
                            }
                        }
                    }

                    List<String> hashManifestLines = new ArrayList<>();
                    FileDigest slimDigest = writeStoredFile(slimApkFile, "assets/bundled/base.apk", zos, buf);
                    hashManifestLines.add(slimDigest.sha256Hex + " base.apk");
                    slimApkFile.delete();

                    // 3. Stream assets/obb -> assets/bundled/patch.obb if present.
                    long obbSize = 0;
                    if (obbEntry != null && obbEntry.getSize() > 0) {
                        obbSize = obbEntry.getSize();
                        if (obbEntry.getMethod() == ZipEntry.STORED && obbEntry.getCrc() >= 0) {
                            String obbSha = copyStoredEntryWithSha(
                                    sourceZip, obbEntry, "assets/bundled/patch.obb", zos, buf);
                            hashManifestLines.add(obbSha + " patch.obb");
                        } else {
                            File tempObb = new File(workDir, "patch.obb");
                            try {
                                try (InputStream in = sourceZip.getInputStream(obbEntry);
                                     OutputStream out = new BufferedOutputStream(new FileOutputStream(tempObb), IO_BUFFER_SIZE)) {
                                    copyStream(in, out, buf);
                                }
                                FileDigest obbDigest = writeStoredFile(tempObb, "assets/bundled/patch.obb", zos, buf);
                                obbSize = tempObb.length();
                                hashManifestLines.add(obbDigest.sha256Hex + " patch.obb");
                            } finally {
                                tempObb.delete();
                            }
                        }
                    }

                    // 4. Stream pre-fixed native guest libraries from record.libDir().
                    File[] libs = record.libDir().listFiles((dir, name) -> name.endsWith(".so"));
                    List<String> libNames = new ArrayList<>();
                    if (libs != null) {
                        Arrays.sort(libs, (a, b) -> a.getName().compareTo(b.getName()));
                        for (File lib : libs) {
                            if (!lib.isFile()) continue;
                            FileDigest d = writeStoredFile(lib, "assets/bundled/lib/" + lib.getName(), zos, buf);
                            libNames.add(lib.getName());
                            hashManifestLines.add(d.sha256Hex + " " + lib.getName());
                        }
                    }
                    if (libNames.isEmpty()) {
                        throw new IOException("No guest native libraries found for " + record.packageName);
                    }

                    // 5. Write bundle-info.properties using java.util.Properties for safe escaping.
                    String bundleVersion = sha256Hex(String.join("\n", hashManifestLines) + "\n").substring(0, 16);
                    String launcherClass = (record.launcherActivity != null && !record.launcherActivity.isEmpty())
                            ? record.launcherActivity
                            : "com.unity3d.player.UnityPlayerActivity";
                    String appClass = (record.application != null && !record.application.isEmpty())
                            ? record.application
                            : android.app.Application.class.getName();
                    java.util.Properties props = new java.util.Properties();
                    props.setProperty("version", bundleVersion);
                    props.setProperty("package", record.packageName);
                    props.setProperty("launcher", launcherClass);
                    props.setProperty("application", appClass);
                    props.setProperty("versionName", versionName);
                    props.setProperty("versionCode", String.valueOf(longVer > 0 ? longVer : versionCode));
                    props.setProperty("targetSdk", String.valueOf(record.targetSdk > 0 ? record.targetSdk : 30));
                    props.setProperty("selectedAbi",
                            record.selectedAbi != null ? record.selectedAbi : PluginRecord.ABI_ARM32_V7A);
                    props.setProperty("hideAds", String.valueOf(record.hideAds));
                    props.setProperty("preciseFaults", String.valueOf(record.preciseFaults));
                    props.setProperty("diagnostics", String.valueOf(record.diagnostics));
                    props.setProperty("unsafeFp", String.valueOf(record.unsafeFp));
                    props.setProperty("unityMt", String.valueOf(record.unityMt));
                    props.setProperty("obbSize", String.valueOf(obbSize));
                    props.setProperty("libs", String.join(",", libNames));
                    ByteArrayOutputStream propsOut = new ByteArrayOutputStream();
                    try (java.io.OutputStreamWriter writer = new java.io.OutputStreamWriter(propsOut, StandardCharsets.UTF_8)) {
                        props.store(writer, null);
                    }
                    writeDeflatedBytes(zos, "assets/bundled/bundle-info.properties", propsOut.toByteArray());
                } finally {
                    slimApkFile.delete();
                }
            }

            if (progress != null) progress.onStage("Signing APK (v1/v2/v3)...");
            ApkSignerHelper.signApk(context, unsignedApk, signedApk);
            unsignedApk.delete();

            if (progress != null) progress.onStage("Saving exported APK...");
            try (InputStream in = new BufferedInputStream(new FileInputStream(signedApk), IO_BUFFER_SIZE);
                 OutputStream out = context.getContentResolver().openOutputStream(destinationUri)) {
                if (out == null) {
                    throw new IOException("Cannot open destination URI: " + destinationUri);
                }
                copyStream(in, out, buf);
            }
        } finally {
            PluginFiles.deleteRecursive(workDir);
        }
    }

    private static Map<String, byte[]> buildReplacementIcons(
            Context context,
            PluginRecord record,
            PackageInfo pkgInfo,
            ZipFile sourceZip,
            Map<String, ArscIconFinder.IconSlot> slots
    ) {
        Map<String, byte[]> out = new HashMap<>();
        if (slots.isEmpty()) return out;

        Bitmap fallbackBitmap = loadFallbackIconBitmap(context, record, pkgInfo);
        try {
            for (ArscIconFinder.IconSlot slot : slots.values()) {
                byte[] png = findMatchingIconPng(sourceZip, slot.densityDpi, slot.round);
                if (png == null && slot.round) {
                    png = findMatchingIconPng(sourceZip, slot.densityDpi, false);
                }
                if (png == null && fallbackBitmap != null) {
                    int px = iconSizeForDensity(slot.densityDpi);
                    Bitmap scaled = Bitmap.createScaledBitmap(fallbackBitmap, px, px, true);
                    try {
                        ByteArrayOutputStream baos = new ByteArrayOutputStream();
                        scaled.compress(Bitmap.CompressFormat.PNG, 100, baos);
                        png = baos.toByteArray();
                    } finally {
                        if (scaled != fallbackBitmap) scaled.recycle();
                    }
                }
                if (png != null) {
                    out.put(slot.zipEntryPath, png);
                }
            }
        } finally {
            if (fallbackBitmap != null && !fallbackBitmap.isRecycled()) {
                fallbackBitmap.recycle();
            }
        }
        return out;
    }

    private static byte[] findMatchingIconPng(ZipFile sourceZip, int densityDpi, boolean round) {
        String bucket = densityBucketName(densityDpi);
        if (bucket == null) return null;
        List<String> candidates = new ArrayList<>();
        if (round) {
            candidates.add("res/mipmap-" + bucket + "/app_icon_round.png");
            candidates.add("res/mipmap-" + bucket + "-v4/app_icon_round.png");
            candidates.add("res/mipmap-" + bucket + "/ic_launcher_round.png");
            candidates.add("res/mipmap-" + bucket + "-v4/ic_launcher_round.png");
        } else {
            candidates.add("res/mipmap-" + bucket + "/app_icon.png");
            candidates.add("res/mipmap-" + bucket + "-v4/app_icon.png");
            candidates.add("res/mipmap-" + bucket + "/ic_launcher.png");
            candidates.add("res/mipmap-" + bucket + "-v4/ic_launcher.png");
        }
        for (String path : candidates) {
            ZipEntry e = sourceZip.getEntry(path);
            if (e != null && !e.isDirectory()) {
                try {
                    return readZipEntryBytes(sourceZip, e, 8 << 20);
                } catch (IOException ignored) {
                    // Fall through to next candidate or bitmap scaling.
                }
            }
        }
        return null;
    }

    private static Bitmap loadFallbackIconBitmap(Context context, PluginRecord record, PackageInfo pkgInfo) {
        File iconFile = record.iconFile();
        if (iconFile.isFile()) {
            Bitmap b = BitmapFactory.decodeFile(iconFile.getPath());
            if (b != null) return b;
        }
        try {
            PackageManager pm = context.getPackageManager();
            ApplicationInfo ai = PluginStore.applicationInfo(pkgInfo, record);
            Drawable d = ai.loadIcon(pm);
            if (d instanceof BitmapDrawable) {
                Bitmap b = ((BitmapDrawable) d).getBitmap();
                if (b != null) return b.copy(Bitmap.Config.ARGB_8888, false);
            }
            if (d != null) {
                Bitmap b = Bitmap.createBitmap(192, 192, Bitmap.Config.ARGB_8888);
                Canvas canvas = new Canvas(b);
                d.setBounds(0, 0, 192, 192);
                d.draw(canvas);
                return b;
            }
        } catch (Throwable ignored) {
            // Fall through.
        }
        return null;
    }

    private static String densityBucketName(int densityDpi) {
        if (densityDpi == 120) return "ldpi";
        if (densityDpi == 160) return "mdpi";
        if (densityDpi == 240) return "hdpi";
        if (densityDpi == 320) return "xhdpi";
        if (densityDpi == 480) return "xxhdpi";
        if (densityDpi == 640 || densityDpi == 0) return "xxxhdpi";
        return null;
    }

    private static int iconSizeForDensity(int densityDpi) {
        if (densityDpi <= 0) return 192;
        return Math.max(48, Math.round(48f * densityDpi / 160f));
    }

    private static final class FileDigest {
        final String sha256Hex;

        FileDigest(String sha256Hex) {
            this.sha256Hex = sha256Hex;
        }
    }

    private static FileDigest writeStoredFile(File file, String entryName, ZipOutputStream zos, byte[] buf)
            throws IOException {
        CRC32 crc = new CRC32();
        MessageDigest sha = newSha256();
        long size = 0;
        try (InputStream in = new BufferedInputStream(new FileInputStream(file), IO_BUFFER_SIZE)) {
            int n;
            while ((n = in.read(buf)) != -1) {
                crc.update(buf, 0, n);
                sha.update(buf, 0, n);
                size += n;
            }
        }
        ZipEntry outEntry = new ZipEntry(entryName);
        outEntry.setMethod(ZipEntry.STORED);
        outEntry.setSize(size);
        outEntry.setCompressedSize(size);
        outEntry.setCrc(crc.getValue());
        zos.putNextEntry(outEntry);
        try (InputStream in = new BufferedInputStream(new FileInputStream(file), IO_BUFFER_SIZE)) {
            copyStream(in, zos, buf);
        }
        zos.closeEntry();
        return new FileDigest(toHex(sha.digest()));
    }

    private static String copyStoredEntryWithSha(
            ZipFile zf, ZipEntry srcEntry, String dstName, ZipOutputStream zos, byte[] buf) throws IOException {
        ZipEntry outEntry = new ZipEntry(dstName);
        outEntry.setMethod(ZipEntry.STORED);
        outEntry.setSize(srcEntry.getSize());
        outEntry.setCompressedSize(srcEntry.getSize());
        outEntry.setCrc(srcEntry.getCrc());
        zos.putNextEntry(outEntry);
        MessageDigest sha = newSha256();
        try (InputStream in = zf.getInputStream(srcEntry)) {
            int n;
            while ((n = in.read(buf)) != -1) {
                sha.update(buf, 0, n);
                zos.write(buf, 0, n);
            }
        }
        zos.closeEntry();
        return toHex(sha.digest());
    }

    private static void copyStoredEntry(
            ZipFile zf, ZipEntry srcEntry, String dstName, ZipOutputStream zos, byte[] buf) throws IOException {
        long size = srcEntry.getSize();
        long crc = srcEntry.getCrc();
        if (size < 0 || crc < 0) {
            byte[] data = readZipEntryBytes(zf, srcEntry, 64 << 20);
            writeStoredBytes(zos, dstName, data);
            return;
        }
        ZipEntry outEntry = new ZipEntry(dstName);
        outEntry.setMethod(ZipEntry.STORED);
        outEntry.setSize(size);
        outEntry.setCompressedSize(size);
        outEntry.setCrc(crc);
        zos.putNextEntry(outEntry);
        try (InputStream in = zf.getInputStream(srcEntry)) {
            copyStream(in, zos, buf);
        }
        zos.closeEntry();
    }

    private static void copyDeflatedEntry(
            ZipFile zf, ZipEntry srcEntry, String dstName, ZipOutputStream zos, byte[] buf) throws IOException {
        ZipEntry outEntry = new ZipEntry(dstName);
        outEntry.setMethod(ZipEntry.DEFLATED);
        zos.putNextEntry(outEntry);
        try (InputStream in = zf.getInputStream(srcEntry)) {
            copyStream(in, zos, buf);
        }
        zos.closeEntry();
    }

    private static void writeStoredBytes(ZipOutputStream zos, String name, byte[] data) throws IOException {
        CRC32 crc = new CRC32();
        crc.update(data);
        ZipEntry outEntry = new ZipEntry(name);
        outEntry.setMethod(ZipEntry.STORED);
        outEntry.setSize(data.length);
        outEntry.setCompressedSize(data.length);
        outEntry.setCrc(crc.getValue());
        zos.putNextEntry(outEntry);
        zos.write(data);
        zos.closeEntry();
    }

    private static void writeDeflatedBytes(ZipOutputStream zos, String name, byte[] data) throws IOException {
        ZipEntry outEntry = new ZipEntry(name);
        outEntry.setMethod(ZipEntry.DEFLATED);
        zos.putNextEntry(outEntry);
        zos.write(data);
        zos.closeEntry();
    }

    private static byte[] readZipEntryBytes(ZipFile zf, ZipEntry entry, int maxBytes) throws IOException {
        long declared = entry.getSize();
        if (declared > maxBytes) {
            throw new IOException("ZIP entry too large (" + declared + " bytes): " + entry.getName());
        }
        try (InputStream in = zf.getInputStream(entry);
             ByteArrayOutputStream baos = new ByteArrayOutputStream()) {
            byte[] tmp = new byte[16384];
            long total = 0;
            int n;
            while ((n = in.read(tmp)) != -1) {
                total += n;
                if (total > maxBytes) {
                    throw new IOException("ZIP entry exceeds limit (" + maxBytes + " bytes): " + entry.getName());
                }
                baos.write(tmp, 0, n);
            }
            return baos.toByteArray();
        }
    }

    private static void copyStream(InputStream in, OutputStream out, byte[] buf) throws IOException {
        int n;
        while ((n = in.read(buf)) != -1) {
            out.write(buf, 0, n);
        }
    }

    private static MessageDigest newSha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    private static String sha256Hex(String text) {
        MessageDigest md = newSha256();
        md.update(text.getBytes(StandardCharsets.UTF_8));
        return toHex(md.digest());
    }

    private static String toHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            sb.append(String.format(Locale.US, "%02x", b & 0xFF));
        }
        return sb.toString();
    }
}
