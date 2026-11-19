package com.zettabridge.launcher;

import android.app.ActionBar;
import android.app.Activity;
import android.content.res.TypedArray;
import android.os.Build;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/**
 * Applies a plugin activity's window decoration the way the plugin's own theme asks for it.
 *
 * Two things do not happen by themselves for a plugin. The activity's window was created from the
 * launcher's stub theme before the plugin theme was swapped in, so an action bar or title can
 * survive; and a fullscreen guest hides the navigation bar with setSystemUiVisibility, which newer
 * Android releases ignore. Both are read from the plugin theme and applied here, so nothing is
 * hidden that the guest did not ask to hide.
 */
final class GuestWindowStyle {
    private static final String TAG = "zb-launcher";

    private GuestWindowStyle() {}

    /**
     * The theme the framework would pick for an app that declares none, by its targetSdk. The old
     * default is a dark, title-bar theme; using a modern light one instead left a grey action bar
     * above games that never asked for one.
     */
    static int defaultTheme(int targetSdk) {
        if (targetSdk >= Build.VERSION_CODES.ICE_CREAM_SANDWICH) return android.R.style.Theme_DeviceDefault;
        if (targetSdk >= Build.VERSION_CODES.HONEYCOMB) return android.R.style.Theme_Holo;
        return android.R.style.Theme;
    }

    /**
     * Runs after the plugin's own onCreate. Old games ask for fullscreen there, by the time our
     * theme pass is long over, so honour what the window says now.
     */
    static void afterCreate(Activity activity) {
        Window window = activity.getWindow();
        if (window == null) return;
        final int flags = window.getAttributes().flags;
        if ((flags & WindowManager.LayoutParams.FLAG_FULLSCREEN) == 0) return;
        hideActionBar(activity);
        hideSystemBars(activity);
        final View decor = window.getDecorView();
        decor.getViewTreeObserver().addOnWindowFocusChangeListener(focused -> {
            if (focused) hideSystemBars(activity);
        });
    }

    static void apply(Activity activity, boolean translated) {
        Window window = activity.getWindow();
        if (window == null) return;
        boolean noTitle = false;
        boolean actionBar = true;
        boolean fullscreen = false;
        try {
            TypedArray a = activity.getTheme().obtainStyledAttributes(new int[] {
                android.R.attr.windowNoTitle,
                android.R.attr.windowActionBar,
                android.R.attr.windowFullscreen,
            });
            noTitle = a.getBoolean(0, false);
            actionBar = a.getBoolean(1, true);
            fullscreen = a.getBoolean(2, false);
            a.recycle();
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot read the plugin window theme: " + e);
        }

        // Requesting it here is the only thing that works: the feature has to be set before the
        // plugin's onCreate calls setContentView. A translated plugin is an old game, and those
        // expect no title bar at all; a 64-bit plugin keeps whatever its theme asks for.
        if (noTitle || !actionBar || translated) {
            try {
                window.requestFeature(Window.FEATURE_NO_TITLE);
            } catch (RuntimeException e) {
                Log.w(TAG, "cannot drop the title bar: " + e);
            }
            hideActionBar(activity);
        }
        if (!fullscreen) return;

        window.setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                WindowManager.LayoutParams.FLAG_FULLSCREEN);
        hideSystemBars(activity);
        // The bars come back on every focus change (a dialog, the recents switcher), and the
        // guest's own setSystemUiVisibility no longer brings them down, so re-apply. This runs
        // before the plugin's onCreate, where building the decor is not safe (see hideActionBar),
        // so the listener is only attached if the decor already exists; afterCreate() attaches it
        // otherwise, once the plugin has created its content view.
        if (!isDecorInstalled(activity)) return;
        final View decor = window.getDecorView();
        decor.getViewTreeObserver().addOnWindowFocusChangeListener(focused -> {
            if (focused) hideSystemBars(activity);
        });
    }

    /**
     * Hides the action bar if the window has one.
     *
     * <p>Activity.getActionBar() installs the window decor as a side effect, and
     * PhoneWindow can only build that decor while the activity still resolves framework resources:
     * generateLayout() inflates a platform layout and looks for its content container afterwards.
     * In prepareActivity the plugin's resources are already in place, so that inflation finds no
     * container and throws "Window couldn't find content container view" before the plugin's
     * onCreate ever runs. The bar is therefore only hidden when the decor is already installed;
     * a window that has not built its decor yet gets the feature request instead, which is what
     * actually prevents the bar.
     */
    private static void hideActionBar(Activity activity) {
        if (!isDecorInstalled(activity)) return;
        try {
            ActionBar bar = activity.getActionBar();
            if (bar != null) bar.hide();
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot hide the action bar: " + e);
        }
    }

    /** True once PhoneWindow has built its decor, read from the window without building it. */
    private static boolean isDecorInstalled(Activity activity) {
        Window window = activity.getWindow();
        return window != null && window.peekDecorView() != null;
    }

    @SuppressWarnings("deprecation")
    private static void hideSystemBars(Activity activity) {
        Window window = activity.getWindow();
        if (window == null) return;
        // getDecorView() / getInsetsController() builds the decor, which is only safe once the
        // plugin has created its content view (see hideActionBar).
        if (!isDecorInstalled(activity)) return;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                window.setDecorFitsSystemWindows(false);
                WindowInsetsController controller = window.getInsetsController();
                if (controller != null) {
                    controller.hide(WindowInsets.Type.systemBars());
                    controller.setSystemBarsBehavior(
                            WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                }
            } else {
                window.getDecorView().setSystemUiVisibility(
                        View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                                | View.SYSTEM_UI_FLAG_FULLSCREEN
                                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot hide the system bars: " + e);
        }
    }
}
