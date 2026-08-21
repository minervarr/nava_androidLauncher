package io.nava.launcher;

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Bundle;
import android.provider.Settings;
import android.util.Log;

import java.text.Collator;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

import io.nava.appshell.AppShellActivity;

/**
 * The Java half of navaLauncher: {@link AppShellActivity} plus the one thing a
 * purely native app cannot do for itself — ask the PackageManager what is
 * installed, and start it.
 *
 * <p>Everything else the launcher needs (the IME, the clipboard, the surface,
 * the event loop) is already inherited. This class exists for four methods and
 * one broadcast receiver.
 *
 * <p><strong>The static block is not optional.</strong> A NativeActivity loads
 * the .so through the manifest's {@code android.app.lib_name}, and that load
 * path does not register the library for resolving {@code native} methods
 * declared on a Java class — they throw UnsatisfiedLinkError even though the
 * symbol is in the library. See vk_canvas's USAGE.md.
 */
public class LauncherActivity extends AppShellActivity {
    static { System.loadLibrary("nava_launcher"); }

    private static final String TAG = "navaLauncher";

    private static native void nativeOnPackagesChanged();

    /**
     * Fires when anything is installed, removed or replaced. It only rings a
     * bell: the native side answers "what is installed now?" by asking again,
     * so there is nothing here worth carrying across the thread boundary.
     */
    private final BroadcastReceiver packages = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent i) {
            try {
                nativeOnPackagesChanged();
            } catch (UnsatisfiedLinkError e) {
                Log.e(TAG, "nativeOnPackagesChanged unresolved", e);  // never swallow this
            }
        }
    };

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        IntentFilter f = new IntentFilter();
        f.addAction(Intent.ACTION_PACKAGE_ADDED);
        f.addAction(Intent.ACTION_PACKAGE_REMOVED);
        f.addAction(Intent.ACTION_PACKAGE_REPLACED);
        f.addAction(Intent.ACTION_PACKAGE_CHANGED);
        f.addDataScheme("package");
        registerReceiver(packages, f);
    }

    @Override protected void onDestroy() {
        try {
            unregisterReceiver(packages);
        } catch (IllegalArgumentException ignored) {
            // Never registered, because onCreate threw. Not worth crashing over.
        }
        super.onDestroy();
    }

    // ── Up-calls from pm_bridge.cc ──────────────────────────────────────────

    /**
     * Every app with a launcher entry, as a flat array of label, package and
     * activity triples — see pm_bridge.cc on why the shape is that and not a
     * Parcelable.
     *
     * <p>Sorted HERE, with a {@link Collator}: ordering "Ábaco" against "Azul"
     * is a locale question, and the C++ side has no locale.
     */
    public String[] queryLauncherApps() {
        final PackageManager pm = getPackageManager();
        Intent main = new Intent(Intent.ACTION_MAIN, null);
        main.addCategory(Intent.CATEGORY_LAUNCHER);

        List<ResolveInfo> found = pm.queryIntentActivities(main, 0);
        final String self = getPackageName();

        List<ResolveInfo> keep = new ArrayList<>(found.size());
        for (ResolveInfo ri : found) {
            // The launcher itself is not something to launch from the launcher.
            if (!self.equals(ri.activityInfo.packageName)) keep.add(ri);
        }

        final Collator collator = Collator.getInstance();
        collator.setStrength(Collator.PRIMARY);   // "cafe" finds "café"
        final PackageManager fpm = pm;
        Collections.sort(keep, new Comparator<ResolveInfo>() {
            @Override public int compare(ResolveInfo a, ResolveInfo b) {
                return collator.compare(a.loadLabel(fpm).toString(),
                                        b.loadLabel(fpm).toString());
            }
        });

        String[] out = new String[keep.size() * 3];
        int i = 0;
        for (ResolveInfo ri : keep) {
            out[i++] = ri.loadLabel(pm).toString();
            out[i++] = ri.activityInfo.packageName;
            out[i++] = ri.activityInfo.name;
        }
        return out;
    }

    /**
     * An EXPLICIT component, not {@code getLaunchIntentForPackage}: a package
     * may declare more than one launcher activity, and the user picked one of
     * them by name — resolving by package again would show a chooser for it.
     */
    public void launchApp(String pkg, String activity) {
        Intent i = new Intent(Intent.ACTION_MAIN);
        i.addCategory(Intent.CATEGORY_LAUNCHER);
        i.setComponent(new ComponentName(pkg, activity));
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        try {
            startActivity(i);
        } catch (Exception e) {
            // Uninstalled between the query and the tap, or disabled for this
            // user. Nothing to show; the next query drops it from the list.
            Log.w(TAG, "launch failed: " + pkg, e);
        }
    }

    public void openAppInfo(String pkg) {
        Intent i = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                              Uri.fromParts("package", pkg, null));
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            startActivity(i);
        } catch (Exception e) {
            Log.w(TAG, "app info failed: " + pkg, e);
        }
    }

    /** The confirmation dialog is Android's. A launcher must not fake one. */
    public void requestUninstall(String pkg) {
        Intent i = new Intent(Intent.ACTION_DELETE, Uri.fromParts("package", pkg, null));
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            startActivity(i);
        } catch (Exception e) {
            // A system app: there is no uninstall to offer, and saying so is
            // the system's job, not ours.
            Log.w(TAG, "uninstall failed: " + pkg, e);
        }
    }
}
