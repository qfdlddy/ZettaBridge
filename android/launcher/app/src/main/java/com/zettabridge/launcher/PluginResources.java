package com.zettabridge.launcher;

import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.util.DisplayMetrics;

/**
 * Plugin resources that remap the launcher package name in {@link #getIdentifier}.
 *
 * <p>{@link PluginContext#getPackageName()} keeps the launcher's package name so system services
 * accept the caller UID, which causes plugin libraries (such as UnityPlayer) that call
 * {@code getResources().getIdentifier(name, type, context.getPackageName())} to query the
 * launcher package instead of the plugin package.
 */
final class PluginResources extends Resources {
    private final String pluginPackage;
    private final String hostPackage;

    @SuppressWarnings("deprecation")
    PluginResources(Resources base, String pluginPackage, String hostPackage) {
        this(base.getAssets(), base.getDisplayMetrics(), base.getConfiguration(), pluginPackage, hostPackage);
    }

    @SuppressWarnings("deprecation")
    PluginResources(AssetManager assets, DisplayMetrics metrics, Configuration config,
                    String pluginPackage, String hostPackage) {
        super(assets, metrics, config);
        this.pluginPackage = pluginPackage;
        this.hostPackage = hostPackage;
    }

    @Override
    public int getIdentifier(String name, String defType, String defPackage) {
        if (name == null) return 0;
        String effectiveName = rewritePackagePrefix(name);
        if (pluginPackage != null && hostPackage != null && hostPackage.equals(defPackage)) {
            int id = super.getIdentifier(effectiveName, defType, pluginPackage);
            if (id != 0) return id;
        }
        if (!effectiveName.equals(name)) {
            int id = super.getIdentifier(effectiveName, defType, defPackage);
            if (id != 0) return id;
        }
        int id = super.getIdentifier(name, defType, defPackage);
        if (id == 0 && defPackage == null && pluginPackage != null) {
            id = super.getIdentifier(effectiveName, defType, pluginPackage);
        }
        return id;
    }

    private String rewritePackagePrefix(String name) {
        if (hostPackage == null || pluginPackage == null) return name;
        String prefix = hostPackage + ":";
        if (!name.startsWith(prefix)) return name;
        return pluginPackage + name.substring(hostPackage.length());
    }
}
