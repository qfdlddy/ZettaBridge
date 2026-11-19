package com.zettabridge.launcher;

import android.content.ComponentName;
import android.content.Intent;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.util.Log;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;

/**
 * Package manager virtualization for the :guest process. Plugins run under the launcher's package
 * name, so SDKs that look up their own components or meta-data (Firebase component discovery,
 * AdMob/AppMetrica APPLICATION_ID, ...) would read the launcher's manifest. Queries for the
 * launcher package that name a plugin component, or ask for application/package info, are
 * answered from the plugin's parsed APK instead.
 */
final class PackageManagerHook implements InvocationHandler {
    private static final String TAG = "zb-launcher";

    private static volatile Object installedProxy;

    private final Object real;
    private final String hostPackage;
    private final GuestRuntime runtime;
    private final java.util.concurrent.atomic.AtomicInteger packageQueries =
            new java.util.concurrent.atomic.AtomicInteger();
    private static final int MAX_LOGGED_PACKAGE_QUERIES = 12;

    private PackageManagerHook(Object real, String hostPackage, GuestRuntime runtime) {
        this.real = real;
        this.hostPackage = hostPackage;
        this.runtime = runtime;
    }

    static void install(Context host, GuestRuntime runtime) {
        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Object current = Reflect.method(activityThread, "getPackageManager").invoke(null);
            if (current == null || Proxy.isProxyClass(current.getClass())) return;
            Class<?> ipm = Class.forName("android.content.pm.IPackageManager");
            Object proxy = Proxy.newProxyInstance(ipm.getClassLoader(), new Class<?>[] {ipm},
                    new PackageManagerHook(current, host.getPackageName(), runtime));
            Field field = Reflect.field(activityThread, "sPackageManager");
            field.set(null, proxy);
            installedProxy = proxy;
            // ContextImpl caches an ApplicationPackageManager that holds the binder proxy directly.
            PackageManager pm = host.getPackageManager();
            Reflect.trySet(pm.getClass(), pm, "mPM", proxy);
            disableCaches();
            if (!BuildConfig.DISABLE_DIAGNOSTICS) Log.i(TAG, "package manager virtualization installed");
        } catch (ReflectiveOperationException | RuntimeException e) {
            Diagnostics.report(host, "cannot install package manager virtualization", e, false);
        }
    }

    /** Patch a package manager instance a plugin context hands out, so its binder is our proxy too. */
    static void patch(PackageManager pm) {
        Object proxy = installedProxy;
        if (proxy != null && pm != null) Reflect.trySet(pm.getClass(), pm, "mPM", proxy);
    }

    /** Android 11+ caches application/package info per process; stale entries would bypass the hook. */
    private static void disableCaches() {
        for (String name : new String[] {"disableApplicationInfoCache", "disablePackageInfoCache"}) {
            try {
                Reflect.method(PackageManager.class, name).invoke(null);
            } catch (ReflectiveOperationException | RuntimeException ignored) {
                // not present on this Android version
            }
        }
    }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        final String name = method.getName();
        if (!BuildConfig.DISABLE_DIAGNOSTICS && args != null && args.length > 0
                && packageQueries.get() < MAX_LOGGED_PACKAGE_QUERIES
                && ("getPackageInfo".equals(name) || "getApplicationInfo".equals(name)
                        || "getInstallerPackageName".equals(name))) {
            Log.i(TAG, "pm query " + name + " arg=" + args[0]
                    + (args.length > 1 ? " flags=" + args[1] : ""));
        }
        try {
            Object result = method.invoke(real, args);
            if (!BuildConfig.DISABLE_DIAGNOSTICS && args != null && args.length > 0
                    && packageQueries.get() < MAX_LOGGED_PACKAGE_QUERIES
                    && "getPackageInfo".equals(name) && result instanceof PackageInfo) {
                PackageInfo pi = (PackageInfo) result;
                Log.i(TAG, "pm answer pkg=" + pi.packageName + " versionCode=" + pi.getLongVersionCode());
            }
            return rewrite(name, args, result);
        } catch (InvocationTargetException e) {
            Object fallback = rewriteMissing(method.getName(), args);
            if (fallback != null) return fallback;
            throw e.getCause();
        }
    }

    private Object rewrite(String name, Object[] args, Object result) {
        if (args == null || args.length == 0) return result;
        switch (name) {
            case "getServiceInfo":
            case "getActivityInfo":
            case "getReceiverInfo":
            case "getProviderInfo": {
                Object component = pluginComponent(name, args[0]);
                return component != null ? component : result;
            }
            case "queryIntentActivities": {
                // Explicit intents for a plugin activity: old AdMob refuses to show ads unless
                // queryIntentActivities finds AdActivity with the right configChanges.
                ResolveInfo resolved = pluginActivityResolve(args[0]);
                return resolved != null ? withResolved(result, resolved) : result;
            }
            case "resolveIntent": {
                if (result != null) return result;
                return pluginActivityResolve(args[0]);
            }
            case "getApplicationInfo":
                if (args[0] instanceof String && result instanceof ApplicationInfo
                        && (hostPackage.equals(args[0]) || isCurrentPluginPackage((String) args[0]))) {
                    return withPluginMetaData((ApplicationInfo) result);
                }
                return result;
            case "getPackageInfo":
                if (args[0] instanceof String && result instanceof PackageInfo
                        && (hostPackage.equals(args[0]) || isCurrentPluginPackage((String) args[0]))) {
                    PackageInfo rewritten = withPluginVersion((PackageInfo) result);
                    notePackageQuery((String) args[0], rewritten);
                    return rewritten;
                }
                return result;
            case "getInstallerPackageName":
                if (args[0] instanceof String
                        && (hostPackage.equals(args[0]) || isCurrentPluginPackage((String) args[0]))) {
                    if (!"com.android.vending".equals(result)) {
                        if (!BuildConfig.DISABLE_DIAGNOSTICS) {
                            Log.i(TAG, "pm installer spoofed for " + args[0] + " (" + result + ") -> com.android.vending");
                        }
                        return "com.android.vending";
                    }
                }
                return result;
            default:
                return result;
        }
    }

    /** The real call threw (e.g. NameNotFound wrapped by the binder layer): answer plugin components anyway. */
    private Object rewriteMissing(String name, Object[] args) {
        if (args == null || args.length == 0) return null;
        switch (name) {
            case "getServiceInfo":
            case "getActivityInfo":
            case "getReceiverInfo":
            case "getProviderInfo":
                return pluginComponent(name, args[0]);
            case "getPackageInfo": {
                // The plugin package is not installed, so the real query throws. A guest asking
                // about itself (Unity reads versionCode to build its OBB file name) must be
                // answered from the parsed APK, or it gets null and builds a broken path.
                if (!(args[0] instanceof String) || !isCurrentPluginPackage((String) args[0])) return null;
                LoadedPlugin p = runtime.current();
                if (p == null || p.info == null) return null;
                PackageInfo copy = clonePackageInfo(p.info);
                notePackageQuery((String) args[0], copy);
                return copy;
            }
            case "getApplicationInfo": {
                if (!(args[0] instanceof String) || !isCurrentPluginPackage((String) args[0])) return null;
                LoadedPlugin p = runtime.current();
                return p == null ? null : withPluginMetaData(p.appInfo);
            }
            case "getInstallerPackageName":
                if (args[0] instanceof String && isCurrentPluginPackage((String) args[0])) {
                    return "com.android.vending";
                }
                return null;
            default:
                return null;
        }
    }

    /** Bounded echo of what a plugin's own package query answered, so a device log proves it. */
    private void notePackageQuery(String requested, PackageInfo answer) {
        if (BuildConfig.DISABLE_DIAGNOSTICS) return;
        int queries = packageQueries.incrementAndGet();
        if (queries > MAX_LOGGED_PACKAGE_QUERIES) return;
        Log.i(TAG, "package query " + queries + ": " + requested + " -> " + answer.packageName
                + " versionCode=" + answer.getLongVersionCode()
                + " versionName=" + answer.versionName);
    }
    private Object pluginComponent(String method, Object arg) {
        if (!(arg instanceof ComponentName)) return null;
        ComponentName cn = (ComponentName) arg;
        if (!hostPackage.equals(cn.getPackageName()) && !isCurrentPluginPackage(cn.getPackageName())) return null;
        ComponentInfo info = runtime.findComponent(method, cn.getClassName());
        if (info == null) return null;
        if (info instanceof ServiceInfo) return new ServiceInfo((ServiceInfo) info);
        if (info instanceof ActivityInfo) return new ActivityInfo((ActivityInfo) info);
        if (info instanceof ProviderInfo) return new ProviderInfo((ProviderInfo) info);
        return info;
    }

    /** NativeActivity asks for its own intent's component, which names the plugin's package. */
    private boolean isCurrentPluginPackage(String packageName) {
        LoadedPlugin p = runtime.current();
        return p != null && p.info != null && p.info.packageName.equals(packageName);
    }

    private ResolveInfo pluginActivityResolve(Object arg) {
        if (!(arg instanceof Intent)) return null;
        Object info = pluginComponent("getActivityInfo", ((Intent) arg).getComponent());
        if (!(info instanceof ActivityInfo)) return null;
        ResolveInfo resolved = new ResolveInfo();
        resolved.activityInfo = (ActivityInfo) info;
        return resolved;
    }

    /** A query result (a ParceledListSlice, or a List on old releases) that holds `resolved` when it was empty. */
    private static Object withResolved(Object result, ResolveInfo resolved) {
        try {
            if (result instanceof List) {
                if (!((List<?>) result).isEmpty()) return result;
                List<ResolveInfo> list = new ArrayList<>();
                list.add(resolved);
                return list;
            }
            Class<?> slice = Class.forName("android.content.pm.ParceledListSlice");
            if (result != null && slice.isInstance(result)) {
                List<?> current = (List<?>) Reflect.method(slice, "getList").invoke(result);
                if (current != null && !current.isEmpty()) return result;
            }
            List<ResolveInfo> list = new ArrayList<>();
            list.add(resolved);
            return slice.getConstructor(List.class).newInstance(list);
        } catch (ReflectiveOperationException | RuntimeException e) {
            Log.w(TAG, "cannot rewrite an activity query: " + e);
            return result;
        }
    }

    private ApplicationInfo withPluginMetaData(ApplicationInfo hostInfo) {
        LoadedPlugin p = runtime.current();
        if (p == null) return hostInfo;
        ApplicationInfo copy = new ApplicationInfo(hostInfo);
        copy.metaData = p.appInfo.metaData;
        copy.sourceDir = p.appInfo.sourceDir;
        copy.publicSourceDir = p.appInfo.publicSourceDir;
        copy.nativeLibraryDir = p.appInfo.nativeLibraryDir;
        copy.dataDir = p.appInfo.dataDir;
        return copy;
    }

    private static PackageInfo clonePackageInfo(PackageInfo src) {
        android.os.Parcel parcel = android.os.Parcel.obtain();
        try {
            src.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            PackageInfo copy = PackageInfo.CREATOR.createFromParcel(parcel);
            return copy != null ? copy : src;
        } catch (RuntimeException e) {
            return src;
        } finally {
            parcel.recycle();
        }
    }

    private PackageInfo withPluginVersion(PackageInfo hostInfo) {
        LoadedPlugin p = runtime.current();
        if (p == null || p.info == null) return hostInfo;
        // Clone before mutating so framework-cached PackageInfo instances are never altered in place.
        PackageInfo copy = clonePackageInfo(hostInfo);
        // The caller may be asking about the launcher package or about the plugin's own package;
        // both must see the plugin's identity, or a guest reading its own versionCode gets 0.
        copy.packageName = p.info.packageName;
        copy.versionName = p.info.versionName;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            copy.setLongVersionCode(p.info.getLongVersionCode());
        } else {
            copy.versionCode = p.info.versionCode;
        }
        if (copy.applicationInfo != null) copy.applicationInfo = withPluginMetaData(copy.applicationInfo);
        return copy;
    }
}
