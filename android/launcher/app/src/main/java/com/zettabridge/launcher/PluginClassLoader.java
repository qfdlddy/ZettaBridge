package com.zettabridge.launcher;

import dalvik.system.DexClassLoader;

import java.io.File;
import java.io.IOException;

/**
 * Child-first plugin dex loader with explicit launcher bridge and native-library routing.
 *
 * The dex path also carries the platform's legacy Apache HttpClient jar. Android 9 removed those
 * classes from the boot class path, and a pre-28 plugin that calls them - Punch Quest does, from
 * the activity it constructs - dies with NoClassDefFoundError before its first frame. The jar is
 * still shipped and world-readable, so loading it next to the plugin's own dex restores exactly
 * the classes such a plugin was built against.
 */
final class PluginClassLoader extends DexClassLoader {
    private static final String BRIDGE_PREFIX = "com.zettabridge.core.";
    // Optional platform jars a pre-28 plugin may link against. Android 9 dropped both from the
    // boot class path; the jars are still shipped and world-readable.
    //   org.apache.http.legacy  - Punch Quest's activity imports SchemeRegistry.
    //   android.test.base       - Zombie Road Trip's startup path touches junit.framework.Assert.
    private static final String[] OPTIONAL_PLATFORM_JARS = {
            "/system/framework/org.apache.http.legacy.jar",
            "/system/framework/android.test.base.jar",
    };

    private final PluginRecord record;
    private final File proxyTemplate;
    private final ClassLoader bridgeLoader;

    private static String dexPath(PluginRecord record) {
        StringBuilder path = new StringBuilder(record.apk().getPath());
        for (String name : OPTIONAL_PLATFORM_JARS) {
            File jar = new File(name);
            if (jar.isFile()) path.append(File.pathSeparator).append(jar.getPath());
        }
        return path.toString();
    }

    PluginClassLoader(PluginRecord record, File codeCache, File proxyTemplate, ClassLoader bootLoader,
            ClassLoader bridgeLoader) {
        super(dexPath(record), codeCache.getPath(), record.isTranslated() ? null : record.libDir().getPath(),
                bootLoader);
        this.record = record;
        this.proxyTemplate = proxyTemplate;
        this.bridgeLoader = bridgeLoader;
    }

    static boolean delegateToBridge(String name) {
        return name != null && name.startsWith(BRIDGE_PREFIX);
    }

    private static boolean platformClass(String name) {
        return name.startsWith("java.") || name.startsWith("javax.") || name.startsWith("android.")
                || name.startsWith("dalvik.") || name.startsWith("sun.");
    }

    @Override
    protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
        synchronized (this) {
            Class<?> loaded = findLoadedClass(name);
            if (loaded == null) {
                if (delegateToBridge(name)) {
                    loaded = bridgeLoader.loadClass(name);
                } else if (platformClass(name)) {
                    loaded = super.loadClass(name, false);
                } else {
                    try {
                        loaded = findClass(name);
                    } catch (ClassNotFoundException missing) {
                        loaded = super.loadClass(name, false);
                    }
                }
            }
            if (resolve) resolveClass(loaded);
            return loaded;
        }
    }

    @Override
    public String findLibrary(String name) {
        try {
            File library = PluginFiles.resolveLibrary(record.libDir(), record.proxyDir(), proxyTemplate,
                    record.isTranslated(), name);
            return library != null ? library.getPath() : null;
        } catch (IOException e) {
            return null;  // ART reports the normal UnsatisfiedLinkError; Diagnostics adds the bridge detail.
        }
    }
}
