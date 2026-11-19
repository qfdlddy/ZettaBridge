package com.zettabridge.launcher;

import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.WeakHashMap;

/**
 * Compatibility shim for the Play Store licensing service.
 *
 * A guest that checks its Play licence binds {@code
 * com.android.vending.licensing.ILicensingService}. On a device without Play, and under any
 * translation layer, that package does not exist: {@code Context.bindService} returns false,
 * {@code onServiceConnected} never runs, and the guest's licence callback is never invoked. Games
 * that gate their boot scene on that callback then wait forever.
 *
 * The shim answers only that one intent. It hands the guest's {@code ServiceConnection} a local
 * binder whose {@code queryLocalInterface} returns a {@code Proxy} of the plugin's own copy of
 * {@code ILicensingService}, so AIDL's {@code Stub.asInterface} accepts it as the real interface.
 * {@code checkLicenseV2} then delivers {@code verifyLicense} to the guest's listener, exactly as
 * the Play service does. Nothing else about service binding changes: any other intent reaches the
 * framework unchanged.
 *
 * The delivery is posted to the main looper, which is where the framework itself delivers
 * {@code onServiceConnected}.
 */
final class PlayLicensingShim {
    private static final String TAG = "zb-launcher";
    private static final String SERVICE_PACKAGE = "com.android.vending";
    private static final String DESCRIPTOR = "com.android.vending.licensing.ILicensingService";
    private static final String LISTENER = "com.android.vending.licensing.ILicenseV2ResultListener";

    /** Synthetic connections are remembered weakly so their unbind is answered without leaking contexts. */
    private static final WeakHashMap<ServiceConnection, Boolean> synthetic = new WeakHashMap<>();

    private PlayLicensingShim() {}

    /** True for the Play licensing bind only; every other service intent keeps its real path. */
    static boolean isPlayLicensingBinding(Intent service) {
        if (service == null) return false;
        ComponentName component = service.getComponent();
        if (component != null && SERVICE_PACKAGE.equals(component.getPackageName())
                && DESCRIPTOR.equals(component.getClassName())) {
            return true;
        }
        return DESCRIPTOR.equals(service.getAction());
    }

    /**
     * Behaves like a successful {@code bindService} to the Play licensing service. Returns false
     * when the plugin does not carry the AIDL classes, so the caller falls back to the framework.
     */
    static boolean bind(ClassLoader loader, ServiceConnection connection) {
        if (connection == null) return false;
        final Object service;
        try {
            service = createService(loader, connection);
        } catch (Throwable t) {
            Log.w(TAG, "play licensing: cannot build the local service", t);
            return false;
        }
        final IBinder binder = new LocalBinder(service);
        synchronized (synthetic) {
            synthetic.put(connection, Boolean.TRUE);
        }
        new Handler(Looper.getMainLooper()).post(() -> {
            synchronized (synthetic) {
                if (!synthetic.containsKey(connection)) return;
            }
            try {
                connection.onServiceConnected(
                        new ComponentName(SERVICE_PACKAGE, DESCRIPTOR), binder);
                if (!BuildConfig.DISABLE_DIAGNOSTICS) Log.i(TAG, "play licensing: service connection delivered");
            } catch (Throwable t) {
                if (!BuildConfig.DISABLE_DIAGNOSTICS) Log.w(TAG, "play licensing: service connection callback failed", t);
            }
        });
        if (!BuildConfig.DISABLE_DIAGNOSTICS) Log.i(TAG, "play licensing: bindService answered with a local licensing service");
        return true;
    }

    /** True when this connection was served by the shim, so nothing needs unbinding. */
    static boolean forget(ServiceConnection connection) {
        if (connection == null) return false;
        synchronized (synthetic) {
            return synthetic.remove(connection) != null;
        }
    }

    private static Object createService(ClassLoader loader, ServiceConnection connection)
            throws ClassNotFoundException {
        final Class<?> iface = loader.loadClass(DESCRIPTOR);
        if (!iface.isInterface() || !IInterface.class.isAssignableFrom(iface)) {
            throw new IllegalStateException("licensing descriptor is not an IInterface: " + iface.getName());
        }
        final Class<?> listener = loader.loadClass(LISTENER);
        InvocationHandler handler = (proxy, method, args) -> {
            final String name = method.getName();
            if ("checkLicenseV2".equals(name) && args != null && args.length == 3 && args[1] != null) {
                // The Play service reports a verification result; the guest's listener stores its
                // payload in the service callback. Delivery is the only part the guest waits for.
                listener.getMethod("verifyLicense", int.class, Bundle.class)
                        .invoke(args[1], 0, args[2]);
                if (!BuildConfig.DISABLE_DIAGNOSTICS) Log.i(TAG, "play licensing: verifyLicense delivered to the guest listener");
                return null;
            }
            if (!BuildConfig.DISABLE_DIAGNOSTICS) Log.i(TAG, "play licensing: " + name + " answered without a real store");
            return defaultValue(method.getReturnType());
        };
        return Proxy.newProxyInstance(loader, new Class<?>[] {iface}, handler);
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive() || type == void.class) return null;
        if (type == boolean.class) return false;
        if (type == long.class) return 0L;
        if (type == float.class) return 0f;
        if (type == double.class) return 0d;
        if (type == char.class) return (char) 0;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        return 0;
    }

    /** A local binder that presents the proxy as the licensing interface of this process. */
    private static final class LocalBinder extends Binder {
        private final Object service;

        LocalBinder(Object service) {
            this.service = service;
        }

        @Override
        public IInterface queryLocalInterface(String descriptor) {
            if (DESCRIPTOR.equals(descriptor)) return (IInterface) service;
            return null;
        }
    }
}
