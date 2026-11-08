// [IzunaRecomp] The Android app: SDL's activity, with the game (libmain.so,
// SDL linked in) as its native side. Besides SDL's work it
//  - points the app's folder (settings, logs, mods, DLC, saves to import) at
//    /storage/emulated/0/IzunaRecomp, through REX_APP_FOLDER, before the game
//    starts - a folder any file manager reaches (Android/data is hidden from
//    them on Android 11+). Until "All files access" is granted it can only be
//    the app's own Android/data/<package>/files;
//  - asks for "All files access" on the first start, so the game can read the
//    player's own copy of the game wherever it is on the device's storage, and
//    starts again into the visible folder once it is granted;
//  - opens Android's folder picker for the hub (pickGameFolder, called from
//    native code through JNI) and turns its answer into a path;
//  - starts the app again when the hub asks (restartApp), through
//    IzunaRestartActivity, which runs in its own process.
package com.izunaproject.izunarecomp;

import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.system.Os;
import android.util.Log;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;

public class IzunaActivity extends SDLActivity {
    private static final String TAG = "IzunaRecomp";
    private static final int REQUEST_FOLDER = 0x495A;
    static final String EXTRA_ARGS = "izuna_args";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        File folder = appFolder();
        startedWithAccess = hasStorageAccess();
        copyAssets("izuna", folder);
        installDrivers(new File(folder, "drivers"));
        try {
            Os.setenv("REX_APP_FOLDER", folder.getAbsolutePath(), true);
        } catch (Exception e) {
            Log.e(TAG, "REX_APP_FOLDER not set", e);
        }
        super.onCreate(savedInstanceState);
        // The whole screen, also around a camera cutout: the game's picture is
        // the size of the display (the top of it was under the status bar).
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        getWindow().setAttributes(lp);
        getWindow().setDecorFitsSystemWindows(false);
        hideSystemBars();
        askForStorageAccess();
    }

    /** No status or navigation bar; a swipe from the edge shows them for a moment. */
    private void hideSystemBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    private boolean startedWithAccess;

    /**
     * GPU driver packages (AdrenoTools .zip: meta.json + the driver library, e.g. Turnip) put in the app
     * folder's drivers/, installed into the app's private storage - a library cannot be loaded from shared
     * storage. A package is (re)installed when its .zip is new or changed; native code lists and opens them
     * (izuna_gpu_driver.cpp). Only meta.json and .so files are taken, never a path outside the package's folder.
     */
    private void installDrivers(File from) {
        from.mkdirs();
        File[] zips = from.listFiles((dir, name) -> name.toLowerCase().endsWith(".zip"));
        if (zips == null) return;
        File root = new File(getFilesDir(), "gpu_drivers");
        root.mkdirs();
        for (File zip : zips) {
            String id = zip.getName().substring(0, zip.getName().length() - 4).replaceAll("[^A-Za-z0-9._-]", "_");
            File dest = new File(root, id);
            File stamp = new File(dest, ".installed");
            String want = zip.length() + ":" + zip.lastModified();
            try {
                if (stamp.isFile() && want.equals(new String(java.nio.file.Files.readAllBytes(stamp.toPath())))) continue;
            } catch (Exception e) { /* reinstall */ }
            deleteTree(dest);
            dest.mkdirs();
            int taken = 0;
            try (java.util.zip.ZipInputStream in = new java.util.zip.ZipInputStream(new java.io.FileInputStream(zip))) {
                for (java.util.zip.ZipEntry e; (e = in.getNextEntry()) != null; ) {
                    if (e.isDirectory()) continue;
                    String name = new File(e.getName()).getName();  // (flattened: no folders, no "..")
                    if (!name.equals("meta.json") && !name.endsWith(".so")) continue;
                    try (java.io.OutputStream out = new java.io.FileOutputStream(new File(dest, name))) {
                        byte[] buf = new byte[65536];
                        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                    }
                    ++taken;
                }
                java.nio.file.Files.write(stamp.toPath(), want.getBytes());
                Log.i(TAG, "GPU driver package installed: " + id + " (" + taken + " files)");
            } catch (Exception e) {
                Log.w(TAG, "GPU driver package " + zip.getName() + " not installed", e);
                deleteTree(dest);
            }
        }
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        f.delete();
    }

    /** The app folder's starting files (READMEs, licences, the hub's button pictures), never over one already there. */
    private void copyAssets(String from, File to) {
        try {
            String[] names = getAssets().list(from);
            if (names == null || names.length == 0) {  // a file
                if (to.exists()) return;
                File parent = to.getParentFile();
                if (parent != null) parent.mkdirs();
                try (java.io.InputStream in = getAssets().open(from);
                     java.io.OutputStream out = new java.io.FileOutputStream(to)) {
                    byte[] buf = new byte[65536];
                    for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                }
                return;
            }
            to.mkdirs();
            for (String name : names) copyAssets(from + "/" + name, new File(to, name));
        } catch (Exception e) {
            Log.w(TAG, "could not copy " + from, e);
        }
    }

    /** /storage/emulated/0/IzunaRecomp with "All files access", else the app's own files folder. */
    private File appFolder() {
        if (hasStorageAccess()) {
            File shared = new File(Environment.getExternalStorageDirectory(), "IzunaRecomp");
            if (shared.isDirectory() || shared.mkdirs()) return shared;
        }
        File files = getExternalFilesDir(null);
        return files != null ? files : getFilesDir();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Back from the settings screen with access granted: start again, so the
        // game reads and writes the visible IzunaRecomp folder from now on.
        if (!startedWithAccess && hasStorageAccess()) {
            startedWithAccess = true;
            restartApp("");
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected String[] getArguments() {
        String args = getIntent() != null ? getIntent().getStringExtra(EXTRA_ARGS) : null;
        if (args == null || args.trim().isEmpty()) return new String[0];
        return args.trim().split("\\s+");
    }

    // --- All files access --------------------------------------------------

    private boolean hasStorageAccess() {
        return Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }

    private void askForStorageAccess() {
        if (hasStorageAccess()) return;
        new AlertDialog.Builder(this)
            .setTitle("Access to your game files")
            .setMessage("IzunaRecomp plays your own copy of Ninja Gaiden II from your device's storage. "
                + "On the next screen, turn on \"Allow access to manage all files\" for IzunaRecomp, then come back.")
            .setCancelable(false)
            .setPositiveButton("Continue", (d, w) -> {
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                             Uri.parse("package:" + getPackageName())));
                } catch (Exception e) {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                }
            })
            .show();
    }

    // --- The game folder ---------------------------------------------------

    private final Object pickLock = new Object();
    private boolean pickDone;
    private String pickResult;

    /** Called from native code (ng2_platform.cpp) on the game's thread; blocks until the picker closes. */
    public String pickGameFolder(final String title) {
        synchronized (pickLock) {
            pickDone = false;
            pickResult = null;
        }
        runOnUiThread(() -> {
            if (!hasStorageAccess()) {
                askForStorageAccess();
                finishPick(null);
                return;
            }
            try {
                startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_FOLDER);
            } catch (Exception e) {
                Log.e(TAG, "no folder picker", e);
                finishPick(null);
            }
        });
        synchronized (pickLock) {
            while (!pickDone) {
                try {
                    pickLock.wait();
                } catch (InterruptedException e) {
                    return null;
                }
            }
            return pickResult;
        }
    }

    private void finishPick(String path) {
        synchronized (pickLock) {
            pickResult = path;
            pickDone = true;
            pickLock.notifyAll();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQUEST_FOLDER) {
            String path = null;
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                path = treeToPath(data.getData());
                if (path == null) {
                    new AlertDialog.Builder(this)
                        .setMessage("Choose a folder on the device's own storage or its SD card.")
                        .setPositiveButton("OK", null).show();
                }
            }
            finishPick(path);
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    /** content://com.android.externalstorage.documents/tree/primary:Games/NG2 -> /storage/emulated/0/Games/NG2 */
    static String treeToPath(Uri tree) {
        if (!"com.android.externalstorage.documents".equals(tree.getAuthority())) return null;
        String id;
        try {
            id = DocumentsContract.getTreeDocumentId(tree);
        } catch (Exception e) {
            return null;
        }
        int colon = id.indexOf(':');
        if (colon < 0) return null;
        String volume = id.substring(0, colon);
        String rest = id.substring(colon + 1);
        String base = "primary".equalsIgnoreCase(volume)
            ? Environment.getExternalStorageDirectory().getAbsolutePath()
            : "/storage/" + volume;
        return rest.isEmpty() ? base : base + "/" + rest;
    }

    // --- Starting again ----------------------------------------------------

    /** Called from native code: start the app again (with these arguments) once this process has gone. */
    public String restartApp(final String args) {
        Intent intent = new Intent(this, IzunaRestartActivity.class);
        intent.putExtra(IzunaRestartActivity.EXTRA_PID, android.os.Process.myPid());
        intent.putExtra(EXTRA_ARGS, args == null ? "" : args);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(intent);
        return "ok";
    }
}
