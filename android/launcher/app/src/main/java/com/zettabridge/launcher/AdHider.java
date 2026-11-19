package com.zettabridge.launcher;

import android.app.Activity;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;

/**
 * Hides ad views inside a plugin activity when the plugin has ads turned off.
 *
 * Ad SDKs bundled in old games cannot work here (their network calls fail, and Google Play
 * services answers a plugin with DEVELOPER_ERROR), so what is left on screen is a leftover banner
 * or an error message. This walks the view hierarchy on every layout pass and hides views whose
 * class comes from a known ad SDK package; nothing else is touched, and no SDK code is patched.
 */
final class AdHider {
    private static final String TAG = "zb-launcher";
    private static final String[] AD_PACKAGES = {
        "com.google.android.gms.ads.",
        "com.google.ads.",
        "com.mopub.",
        "com.applovin.",
        "com.chartboost.",
        "com.inmobi.",
        "com.unity3d.ads.",
        "com.vungle.",
        "com.facebook.ads.",
        "com.ironsource.",
        "com.adcolony.",
        "com.startapp.",
    };
    private static final int MAX_DEPTH = 24;

    private AdHider() {}

    static void attach(Activity activity) {
        // getDecorView() builds the window decor, and PhoneWindow can only do that while the
        // activity still resolves framework resources. Called from prepareActivity the plugin's
        // resources are already in place, so building it here throws "Window couldn't find content
        // container view" before the plugin's onCreate runs. Defer to the decor view directly once
        // it exists; the listener is attached from afterCreate() instead, after the plugin has
        // created its content view.
        final View root = decorOf(activity);
        if (root == null) return;
        root.getViewTreeObserver().addOnGlobalLayoutListener(new ViewTreeObserver.OnGlobalLayoutListener() {
            @Override
            public void onGlobalLayout() {
                try {
                    hide(root, 0);
                } catch (RuntimeException e) {
                    Log.w(TAG, "ad hider: " + e);
                    root.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                }
            }
        });
    }

    /** The window's decor view, but only if the window has already built it. */
    static View decorOf(Activity activity) {
        if (activity.getWindow() == null) return null;
        return activity.getWindow().peekDecorView();
    }

    private static void hide(View view, int depth) {
        if (depth > MAX_DEPTH) return;
        if (isAdView(view)) {
            if (view.getVisibility() != View.GONE) {
                if (!BuildConfig.DISABLE_DIAGNOSTICS) Log.i(TAG, "hiding ad view " + view.getClass().getName());
                view.setVisibility(View.GONE);
            }
            return;  // its children go with it
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) hide(group.getChildAt(i), depth + 1);
        }
    }

    private static boolean isAdView(View view) {
        String name = view.getClass().getName();
        for (String prefix : AD_PACKAGES) {
            if (name.startsWith(prefix)) return true;
        }
        return false;
    }
}
