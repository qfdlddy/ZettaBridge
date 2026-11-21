package com.zettabridge.launcher;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.Typeface;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Icon;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Library screen (main process): import APKs, launch them, pin shortcuts, delete. UI built in code. */
public class LibraryActivity extends Activity {
    private static final String TAG = "zb-launcher";
    private static final int REQUEST_IMPORT = 1;
    private static final int REQUEST_EXPORT = 2;
    private static final int REQUEST_NOTIFICATIONS = 3;
    private static final String PREFS = "zb";
    private static final String PREF_NOTIFICATIONS_ASKED = "notification-permission-asked";

    private static final String STATE_EXPORT_RECORD = "zb.export.record";
    private static final String STATE_EXPORT_PACKAGE = "zb.export.package";
    private static final String STATE_EXPORT_LABEL = "zb.export.label";

    private final List<PluginRecord> plugins = new ArrayList<>();
    private BaseAdapter adapter;
    private TextView emptyView;
    private String pendingExportRecordPackage;
    private String pendingExportPackage;
    private String pendingExportLabel;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (state != null) {
            pendingExportRecordPackage = state.getString(STATE_EXPORT_RECORD);
            pendingExportPackage = state.getString(STATE_EXPORT_PACKAGE);
            pendingExportLabel = state.getString(STATE_EXPORT_LABEL);
        }
        int pad = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        root.setFitsSystemWindows(true);

        Button importButton = new Button(this);
        importButton.setText("Import APK");
        importButton.setOnClickListener(v -> pickApk());
        root.addView(importButton, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        emptyView = new TextView(this);
        emptyView.setText("No apps yet. Import an APK to add one. Tap to launch, long-press for more.");
        emptyView.setPadding(0, pad, 0, pad);
        root.addView(emptyView);

        ListView list = new ListView(this);
        adapter = new BaseAdapter() {
            @Override
            public int getCount() {
                return plugins.size();
            }

            @Override
            public Object getItem(int position) {
                return plugins.get(position);
            }

            @Override
            public long getItemId(int position) {
                return position;
            }

            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                return row(convertView, plugins.get(position));
            }
        };
        list.setAdapter(adapter);
        list.setOnItemClickListener((parent, view, position, id) -> launch(plugins.get(position)));
        list.setOnItemLongClickListener((parent, view, position, id) -> {
            showActions(plugins.get(position));
            return true;
        });
        root.addView(list, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        setContentView(root);
        requestNotificationPermission();
        // Pre-install the guest runtime bundle in the background so cold-launching a game after an
        // app update does not stall the :guest main thread.
        new Thread(() -> {
            try {
                RuntimeBundle.install(getApplicationContext());
            } catch (IOException ignored) {
                // GuestRuntime will surface any installation error when launching.
            }
        }, "zb-bundle-warmup").start();
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        if (pendingExportRecordPackage != null) outState.putString(STATE_EXPORT_RECORD, pendingExportRecordPackage);
        if (pendingExportPackage != null) outState.putString(STATE_EXPORT_PACKAGE, pendingExportPackage);
        if (pendingExportLabel != null) outState.putString(STATE_EXPORT_LABEL, pendingExportLabel);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    /**
     * Android 13+ will not post a notification without a runtime grant. A translated guest asks
     * for none (it was built for an older target), so the launcher asks once on its behalf.
     */
    private void requestNotificationPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return;
        if (checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)
                == android.content.pm.PackageManager.PERMISSION_GRANTED) {
            return;
        }
        android.content.SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        if (prefs.getBoolean(PREF_NOTIFICATIONS_ASKED, false)) return;
        prefs.edit().putBoolean(PREF_NOTIFICATIONS_ASKED, true).apply();
        requestPermissions(new String[]{android.Manifest.permission.POST_NOTIFICATIONS}, REQUEST_NOTIFICATIONS);
    }

    private void refresh() {
        plugins.clear();
        plugins.addAll(PluginStore.list(this));
        emptyView.setVisibility(plugins.isEmpty() ? View.VISIBLE : View.GONE);
        adapter.notifyDataSetChanged();
        repointPinnedShortcuts();
    }

    /**
     * Points shortcuts pinned by an older version at the router. They were pinned straight at the
     * guest entry point, which Android resolves to the running game's task instead of launching.
     */
    private void repointPinnedShortcuts() {
        ShortcutManager sm = getSystemService(ShortcutManager.class);
        if (sm == null) return;
        try {
            List<ShortcutInfo> updated = new ArrayList<>();
            for (ShortcutInfo pinned : sm.getPinnedShortcuts()) {
                for (PluginRecord r : plugins) {
                    if (!shortcutId(r).equals(pinned.getId())) continue;
                    ComponentName target = pinned.getIntent() != null ? pinned.getIntent().getComponent() : null;
                    if (target != null && PluginSwitchActivity.class.getName().equals(target.getClassName())) break;
                    updated.add(new ShortcutInfo.Builder(this, pinned.getId())
                            .setIntent(PluginSwitchActivity.intent(this, r.packageName))
                            .build());
                    break;
                }
            }
            if (!updated.isEmpty()) sm.updateShortcuts(updated);
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot repoint the pinned shortcuts: " + e);
        }
    }

    private static final class RowHolder {
        final ImageView icon;
        final TextView title;
        final TextView details;

        RowHolder(ImageView icon, TextView title, TextView details) {
            this.icon = icon;
            this.title = title;
            this.details = details;
        }
    }

    private View row(View convertView, PluginRecord r) {
        View rowView = convertView;
        RowHolder holder;
        if (rowView != null && rowView.getTag() instanceof RowHolder) {
            holder = (RowHolder) rowView.getTag();
        } else {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(0, dp(8), 0, dp(8));

            ImageView icon = new ImageView(this);
            row.addView(icon, new LinearLayout.LayoutParams(dp(48), dp(48)));

            LinearLayout texts = new LinearLayout(this);
            texts.setOrientation(LinearLayout.VERTICAL);
            texts.setPadding(dp(12), 0, 0, 0);
            TextView title = new TextView(this);
            title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
            title.setTypeface(Typeface.DEFAULT_BOLD);
            TextView details = new TextView(this);
            texts.addView(title);
            texts.addView(details);
            row.addView(texts);
            holder = new RowHolder(icon, title, details);
            row.setTag(holder);
            rowView = row;
        }

        Bitmap bmp = r.icon();
        if (bmp != null) {
            holder.icon.setImageBitmap(bmp);
        } else {
            holder.icon.setImageResource(android.R.drawable.sym_def_app_icon);
        }
        holder.title.setText(r.label);
        holder.details.setText(r.packageName + (r.versionName != null ? " " + r.versionName : "") + "\n" + r.status());
        final String warning = r.symbolWarning();
        if (warning != null) holder.details.setText(holder.details.getText() + "\n" + warning);
        return rowView;
    }

    private void pickApk() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*")
                .putExtra(Intent.EXTRA_MIME_TYPES,
                        new String[] {"application/vnd.android.package-archive", "application/octet-stream"});
        startActivityForResult(intent, REQUEST_IMPORT);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_IMPORT && resultCode == RESULT_OK && data != null && data.getData() != null) {
            importApk(data.getData());
        } else if (requestCode == REQUEST_EXPORT && resultCode == RESULT_OK && data != null && data.getData() != null) {
            startExport(data.getData());
        }
    }

    private void importApk(Uri uri) {
        // A running plugin keeps the old APK mapped: stop the guest process first.
        stopGuestProcess();
        final android.content.Context app = getApplicationContext();
        AlertDialog progress = new AlertDialog.Builder(this).setMessage("Importing...").setCancelable(false).show();
        new Thread(() -> {
            String success = null;
            String error = null;
            try {
                PluginRecord r = PluginStore.importApk(app, uri);
                success = "Imported " + r.label + " (" + r.status() + ")";
                final String warning = r.symbolWarning();
                if (warning != null) success += "\n" + warning;
            } catch (Exception e) {
                Log.e(TAG, "import failed", e);
                Diagnostics.report(app, "import failed", e, false);
                error = String.valueOf(e.getMessage());
            }
            String okMessage = success;
            String errMessage = error;
            runOnUiThread(() -> {
                if (progress.isShowing()) progress.dismiss();
                if (isFinishing() || isDestroyed()) return;
                if (errMessage == null) {
                    Toast.makeText(this, okMessage, Toast.LENGTH_LONG).show();
                } else {
                    showImportError(errMessage);
                }
                refresh();
            });
        }, "zb-import").start();
    }

    private void showImportError(String error) {
        TextView view = new TextView(this);
        view.setText(error);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        view.setTypeface(Typeface.MONOSPACE);
        view.setTextIsSelectable(true);
        int pad = dp(16);
        view.setPadding(pad, pad, pad, pad);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(view);
        new AlertDialog.Builder(this)
                .setTitle("Import failed")
                .setView(scroll)
                .setPositiveButton("Copy", (d, w) -> Diagnostics.copy(this, "ZettaBridge import error", error))
                .setNegativeButton("Close", null)
                .show();
    }

    private void launch(PluginRecord r) {
        if (!r.isLaunchable()) {
            Toast.makeText(this, r.label + ": " + r.status(), Toast.LENGTH_LONG).show();
            return;
        }
        startActivity(PluginSwitchActivity.intent(this, r.packageName));
    }

    private void showActions(PluginRecord r) {
        new AlertDialog.Builder(this)
                .setTitle(r.label)
                .setItems(new String[] {"Launch", "Last run report", "Pin shortcut",
                                        r.hideAds ? "Show ads" : "Hide ads",
                                        r.preciseFaults ? "Precise faults: on" : "Precise faults (slow)",
                                        r.diagnostics ? "GL diagnostics: on" : "GL diagnostics (slow)",
                                        r.unsafeFp ? "Fast FP: on" : "Fast FP (off)",
                                        r.unityMt ? "Unity MT: on" : "Unity MT (off)",
                                        "Export APK",
                                        "Delete"},
                        (dialog, which) -> {
                            if (which == 0) launch(r);
                            if (which == 1) showRuntimeReport(r);
                            if (which == 2) pinShortcut(r);
                            if (which == 3) toggleAds(r);
                            if (which == 4) togglePreciseFaults(r);
                            if (which == 5) toggleDiagnostics(r);
                            if (which == 6) toggleUnsafeFp(r);
                            if (which == 7) toggleUnityMt(r);
                            if (which == 8) promptExport(r);
                            if (which == 9) confirmDelete(r);
                        })
                .show();
    }

    /** Ads are hidden per plugin; it takes effect the next time the plugin process starts. */
    private void toggleAds(PluginRecord r) {
        r.hideAds = !r.hideAds;
        try {
            r.save();
        } catch (IOException e) {
            Diagnostics.report(this, "cannot save the ad setting of " + r.label, e, false);
            return;
        }
        Toast.makeText(this, (r.hideAds ? "Ads hidden for " : "Ads shown for ") + r.label
                + ". Restart the game to apply.", Toast.LENGTH_LONG).show();
    }

    /**
     * Precise memory faults stop at the exact faulting instruction with registers committed,
     * instead of at the end of the translated block, at roughly 2x cost on integer code. It takes
     * effect the next time the plugin process starts.
     */
    private void togglePreciseFaults(PluginRecord r) {
        r.preciseFaults = !r.preciseFaults;
        try {
            r.save();
        } catch (IOException e) {
            Diagnostics.report(this, "cannot save the precise-faults setting of " + r.label, e, false);
            return;
        }
        Toast.makeText(this, "Restart the game to apply.", Toast.LENGTH_LONG).show();
    }

    /**
     * GL diagnostics instrument every draw: whole-framebuffer readbacks, per-draw pixel diffs and
     * ASCII frame maps in the run report. Debugging only, and very slow. It takes effect the next
     * time the plugin process starts.
     */
    private void toggleDiagnostics(PluginRecord r) {
        r.diagnostics = !r.diagnostics;
        try {
            r.save();
        } catch (IOException e) {
            Diagnostics.report(this, "cannot save the diagnostics setting of " + r.label, e, false);
            return;
        }
        Toast.makeText(this, "Restart the game to apply.", Toast.LENGTH_LONG).show();
    }

    /** Toggles ZB_UNSAFE_FP for this plugin; takes effect on the next launch. */
    private void toggleUnsafeFp(PluginRecord r) {
        r.unsafeFp = !r.unsafeFp;
        try {
            r.save();
        } catch (IOException e) {
            Diagnostics.report(this, "cannot save the fast-FP setting of " + r.label, e, false);
            return;
        }
        Toast.makeText(this, "Restart the game to apply.", Toast.LENGTH_LONG).show();
    }

    /** Toggles experimental Unity multi-threaded rendering; takes effect on the next launch. */
    private void toggleUnityMt(PluginRecord r) {
        r.unityMt = !r.unityMt;
        try {
            r.save();
        } catch (IOException e) {
            Diagnostics.report(this, "cannot save the Unity MT setting of " + r.label, e, false);
            return;
        }
        Toast.makeText(this, "Restart the game to apply.", Toast.LENGTH_LONG).show();
    }

    private void promptExport(PluginRecord r) {
        if (!r.isLaunchable() || !r.isTranslated()) {
            Toast.makeText(this, "Only ready 32-bit translated apps can be exported as standalone APKs.",
                    Toast.LENGTH_LONG).show();
            return;
        }
        int pad = dp(16);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(pad, dp(8), pad, 0);

        TextView labelTitle = new TextView(this);
        labelTitle.setText("App Name (桌面名稱)");
        layout.addView(labelTitle);

        EditText labelInput = new EditText(this);
        labelInput.setText(r.label);
        labelInput.setSingleLine(true);
        layout.addView(labelInput);

        TextView pkgTitle = new TextView(this);
        pkgTitle.setText("Package Name (包名)");
        pkgTitle.setPadding(0, dp(10), 0, 0);
        layout.addView(pkgTitle);

        EditText pkgInput = new EditText(this);
        pkgInput.setText(r.packageName);
        pkgInput.setSingleLine(true);
        pkgInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        layout.addView(pkgInput);

        TextView note = new TextView(this);
        note.setText("Icon and version are copied from " + r.label + ".");
        note.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        note.setPadding(0, dp(10), 0, 0);
        layout.addView(note);

        AlertDialog.Builder builder = new AlertDialog.Builder(this)
                .setTitle("Export Standalone APK")
                .setView(layout)
                .setPositiveButton("Export", (dialog, which) -> {
                    String outLabel = labelInput.getText().toString().trim();
                    String outPkg = pkgInput.getText().toString().trim();
                    if (outLabel.isEmpty()) {
                        Toast.makeText(this, "App name cannot be empty", Toast.LENGTH_LONG).show();
                        return;
                    }
                    if (!PluginStore.isValidPackageName(outPkg)) {
                        Toast.makeText(this, "Invalid package name: " + outPkg, Toast.LENGTH_LONG).show();
                        return;
                    }
                    pendingExportRecordPackage = r.packageName;
                    pendingExportLabel = outLabel;
                    pendingExportPackage = outPkg;
                    String safeFile = outLabel.replaceAll("[\\\\/:*?\"<>|]", "_") + ".apk";
                    Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT)
                            .addCategory(Intent.CATEGORY_OPENABLE)
                            .setType("application/vnd.android.package-archive")
                            .putExtra(Intent.EXTRA_TITLE, safeFile);
                    startActivityForResult(intent, REQUEST_EXPORT);
                })
                .setNegativeButton("Cancel", null);
        Bitmap bmp = r.icon();
        if (bmp != null) {
            builder.setIcon(new BitmapDrawable(getResources(), bmp));
        }
        builder.show();
    }

    private void startExport(Uri destinationUri) {
        String recordPkg = pendingExportRecordPackage;
        String exportPkg = pendingExportPackage;
        String exportLabel = pendingExportLabel;
        pendingExportRecordPackage = null;
        pendingExportPackage = null;
        pendingExportLabel = null;
        PluginRecord r = recordPkg != null ? PluginStore.find(this, recordPkg) : null;
        if (r == null || exportPkg == null || exportLabel == null) return;

        final android.content.Context app = getApplicationContext();
        AlertDialog progress = new AlertDialog.Builder(this)
                .setTitle("Exporting " + exportLabel)
                .setMessage("Preparing...")
                .setCancelable(false)
                .show();

        new Thread(() -> {
            String error = null;
            try {
                ApkExporter.export(app, r, exportPkg, exportLabel, destinationUri,
                        stage -> runOnUiThread(() -> {
                            if (!isFinishing() && !isDestroyed() && progress.isShowing()) {
                                progress.setMessage(stage);
                            }
                        }));
            } catch (Throwable t) {
                Log.e(TAG, "export failed", t);
                Diagnostics.report(app, "export failed", t, false);
                error = t.getClass().getSimpleName() + ": " + t.getMessage();
            }
            String errMsg = error;
            runOnUiThread(() -> {
                if (progress.isShowing()) progress.dismiss();
                if (isFinishing() || isDestroyed()) return;
                if (errMsg == null) {
                    Toast.makeText(this, "Exported " + exportLabel + " APK", Toast.LENGTH_LONG).show();
                } else {
                    showExportError(errMsg);
                }
            });
        }, "zb-export").start();
    }

    private void showExportError(String error) {
        TextView view = new TextView(this);
        view.setText(error);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        view.setTypeface(Typeface.MONOSPACE);
        view.setTextIsSelectable(true);
        int pad = dp(16);
        view.setPadding(pad, pad, pad, pad);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(view);
        new AlertDialog.Builder(this)
                .setTitle("Export failed")
                .setView(scroll)
                .setPositiveButton("Copy", (d, w) -> Diagnostics.copy(this, "ZettaBridge export error", error))
                .setNegativeButton("Close", null)
                .show();
    }

    /**
     * What the last :guest run recorded, without a shell: OxygenOS drops third-party logcat
     * output, so this file is the only evidence a device run leaves behind. The text also goes to
     * the clipboard, ready to be sent back.
     */
    private void showRuntimeReport(PluginRecord r) {
        final android.content.Context app = getApplicationContext();
        new Thread(() -> {
            String report = Diagnostics.readRuntimeReport(app);
            String errors = Diagnostics.readErrors(app);
            File reportFile = report != null ? Diagnostics.runtimeReportFile(app) : null;
            String errorFileName = errors != null ? Diagnostics.errorFile(app).getName() : null;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                if (report == null && errors == null) {
                    Toast.makeText(this, "No run report yet. Launch " + r.label + " once, then look again.",
                            Toast.LENGTH_LONG).show();
                    return;
                }
                StringBuilder sb = new StringBuilder();
                if (report != null) {
                    sb.append(report);
                    if (reportFile != null) sb.append("\nfile: ").append(reportFile.getAbsolutePath()).append('\n');
                }
                if (errors != null) {
                    if (sb.length() > 0) sb.append('\n');
                    sb.append("=== ").append(errorFileName).append(" ===\n").append(errors);
                }
                String text = sb.toString();
                boolean copied = Diagnostics.copy(this, "ZettaBridge run report", text);

                TextView view = new TextView(this);
                view.setText(text);
                view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
                view.setTypeface(Typeface.MONOSPACE);
                view.setTextIsSelectable(true);
                int pad = dp(16);
                view.setPadding(pad, pad, pad, pad);
                ScrollView scroll = new ScrollView(this);
                scroll.addView(view);
                new AlertDialog.Builder(this)
                        .setTitle(copied ? "Last run report (copied)" : "Last run report")
                        .setView(scroll)
                        .setPositiveButton("Close", null)
                        .show();
            });
        }, "zb-read-report").start();
    }

    private static String shortcutId(PluginRecord r) {
        return "plugin:" + r.packageName;
    }

    private void pinShortcut(PluginRecord r) {
        ShortcutManager sm = getSystemService(ShortcutManager.class);
        if (sm == null || !sm.isRequestPinShortcutSupported()) {
            Toast.makeText(this, "The home screen does not support pinned shortcuts", Toast.LENGTH_LONG).show();
            return;
        }
        Bitmap bmp = r.icon();
        Icon icon = bmp != null ? Icon.createWithBitmap(bmp)
                : Icon.createWithResource(this, android.R.drawable.sym_def_app_icon);
        ShortcutInfo info = new ShortcutInfo.Builder(this, shortcutId(r))
                .setShortLabel(r.label)
                .setLongLabel(r.label)
                .setIcon(icon)
                .setIntent(PluginSwitchActivity.intent(this, r.packageName))
                .build();
        sm.requestPinShortcut(info, null);
    }

    private void confirmDelete(PluginRecord r) {
        new AlertDialog.Builder(this)
                .setMessage("Delete " + r.label + " and its data?")
                .setPositiveButton("Delete", (dialog, which) -> {
                    stopGuestProcess();
                    ShortcutManager sm = getSystemService(ShortcutManager.class);
                    if (sm != null) {
                        try {
                            sm.disableShortcuts(Collections.singletonList(shortcutId(r)), "App was deleted");
                        } catch (RuntimeException e) {
                            Log.w(TAG, "cannot disable shortcut: " + e);
                        }
                    }
                    new Thread(() -> {
                        PluginStore.delete(r);
                        runOnUiThread(() -> {
                            if (!isFinishing() && !isDestroyed()) refresh();
                        });
                    }, "zb-delete").start();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    /** Kills our own :guest process, if running (allowed for processes of the same uid). */
    private void stopGuestProcess() {
        ActivePlugin.endGuestProcess(this);
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics());
    }
}
