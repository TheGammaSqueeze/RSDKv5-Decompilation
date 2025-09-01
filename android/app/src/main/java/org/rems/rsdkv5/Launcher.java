package org.rems.rsdkv5;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.UriPermission;
import android.net.Uri;
import android.os.Process;
import android.content.BroadcastReceiver;
import android.content.IntentFilter;
import android.os.PowerManager;
import android.os.Bundle;
import android.os.CountDownTimer;
import android.provider.DocumentsContract;
import android.util.Log;
import android.net.Uri;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.documentfile.provider.DocumentFile;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.util.concurrent.TimeUnit;

public class Launcher extends AppCompatActivity {

    // Lifecycle hooks to native so the engine can park/unpark cleanly.
    private static native void nativeSetBackgrounded(boolean bg);
    private static native void nativeSetHasFocus(boolean hasFocus);
    private static native void nativeOnTrimMemory();

    private static final int RSDK_VER = 5;
    private static Uri basePath = null;

    public static Launcher instance = null;

    private static File basePathStore;

    private static ActivityResultLauncher<Intent> folderLauncher = null;
    private static ActivityResultLauncher<Intent> gameLauncher = null;

    private static int takeFlags = (Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
            Intent.FLAG_GRANT_READ_URI_PERMISSION);

    // Screen state receiver (Android TV: covers power key / HDMI-CEC standby)
    private final BroadcastReceiver screenReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context ctx, Intent i) {
            final String a = i.getAction();
            try {
                if (Intent.ACTION_SCREEN_OFF.equals(a)) {
                    nativeSetHasFocus(false);
                    nativeSetBackgrounded(true);
                } else if (Intent.ACTION_SCREEN_ON.equals(a) || Intent.ACTION_USER_PRESENT.equals(a)) {
                    nativeSetBackgrounded(false);
                    nativeSetHasFocus(true);
                }
            } catch (Throwable ignored) {}
        }
    };

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        basePathStore = new File(getFilesDir(), "basePathStore");

        folderLauncher = registerForActivityResult(
                new ActivityResultContracts.StartActivityForResult(),
                result -> {
                    if (result.getResultCode() == Activity.RESULT_OK && result.getData() != null) {
                        basePath = result.getData().getData();
                    }
                    try {
                        Log.i("hi", String.format("%d", getContentResolver().openInputStream(
                                DocumentFile.fromTreeUri(this, basePath).findFile("Settings.ini").getUri()).read()));
                    } catch (FileNotFoundException e) {
                        e.printStackTrace();
                    } catch (Exception e) {
                        e.printStackTrace();
                    }
                    startGame(true);
                });

        gameLauncher = registerForActivityResult(
                new ActivityResultContracts.StartActivityForResult(),
                result -> {
                    quit(0);
                });

        boolean canRun = true;

        if (RSDK_VER == 5) {
            if (((ActivityManager) getSystemService(Context.ACTIVITY_SERVICE))
                    .getDeviceConfigurationInfo().reqGlEsVersion < 0x20000) {
                canRun = false;
                new AlertDialog.Builder(this)
                        .setTitle("GLES 2.0 unsupported")
                        .setMessage("This device does not support GLES 2.0, which is required for running RSDKv5.")
                        .setNegativeButton("OK", (dialog, i) -> {
                            dialog.cancel();
                            quit(2);
                        })
                        .setCancelable(false)
                        .show();
            }
        }

        // Register screen on/off + user-present (runtime; no manifest needed)
        IntentFilter f = new IntentFilter();
        f.addAction(Intent.ACTION_SCREEN_OFF);
        f.addAction(Intent.ACTION_SCREEN_ON);
        f.addAction(Intent.ACTION_USER_PRESENT);
        registerReceiver(screenReceiver, f);

        // Give the foreground thread display priority; reduces preemption/jank on low-power CPUs.
        try { Process.setThreadPriority(Process.THREAD_PRIORITY_DISPLAY); } catch (Throwable ignored) {}

        if (canRun)
            startGame(false);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Foreground → unpark
        try {
            nativeSetBackgrounded(false);
            nativeSetHasFocus(true);
        } catch (Throwable ignored) {}
    }

    @Override
    protected void onPause() {
        // Losing foreground → park ASAP
        try {
            nativeSetHasFocus(false);
            nativeSetBackgrounded(true);
        } catch (Throwable ignored) {}
        super.onPause();
    }

    @Override
    protected void onStop() {
        // Fully backgrounded
        try {
            nativeSetBackgrounded(true);
        } catch (Throwable ignored) {}
        super.onStop();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        try {
            nativeSetHasFocus(hasFocus);
            if (!hasFocus) nativeSetBackgrounded(true);
        } catch (Throwable ignored) {}
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        try {
            nativeOnTrimMemory(); // optional cache purge on pressure
        } catch (Throwable ignored) {}
    }

    @Override
    protected void onDestroy() {
        try { unregisterReceiver(screenReceiver); } catch (Throwable ignored) {}
        super.onDestroy();
    }

    private void quit(int code) {
        finishAffinity();
        System.exit(code);
    }

    static class DialogTimer extends CountDownTimer {
        public AlertDialog alert;

        public DialogTimer(long millisInFuture, long countDownInterval) {
            super(millisInFuture, countDownInterval);
        }

        @Override
        public void onTick(long l) {
            alert.setMessage(String.format(
                    "Game will start in %s in %d seconds...",
                    basePath.getPath(),
                    TimeUnit.MILLISECONDS.toSeconds(l) + 1));
        }

        @Override
        public void onFinish() {
            alert.getButton(AlertDialog.BUTTON_POSITIVE).callOnClick();
        }
    }

    public static Uri refreshStore() {
        if (basePathStore.exists() && basePath == null) {
            try {
                BufferedReader reader = new BufferedReader(new FileReader(basePathStore));
                String uri = reader.readLine();
                if (uri != null) {
                    basePath = Uri.parse(uri);
                }
                reader.close();
            } catch (IOException e) {
                e.printStackTrace();
            }
        }

        if (basePath != null) {
            try {
                FileWriter writer = new FileWriter(basePathStore);
                writer.write(basePath.toString() + "\n");
                writer.close();
            } catch (IOException e) {
                e.printStackTrace();
            }
        }
        // Default to /tree/primary:RSDK/V5 if nothing stored yet
        if (basePath == null) {
            try {
                basePath = DocumentsContract.buildTreeDocumentUri(
                        "com.android.externalstorage.documents",
                        "primary:RSDK/V5");
            } catch (Exception e) {
                e.printStackTrace();
            }
        }
return basePath;
    }

    private void startGame(boolean fromPicker) {

        refreshStore();

        boolean found = false;
        if (basePath != null) {
            for (UriPermission uriPermission : getContentResolver().getPersistedUriPermissions()) {
                if (uriPermission.getUri().toString().equals(basePath.toString())) {
                    found = true;
                    break;
                }
            }
        }

        if (!found && !fromPicker) {
            String shownPath;
            if (basePath != null) {
                String raw = basePath.toString();
                int idx = raw.indexOf("/tree/");
                if (idx != -1) {
                    raw = raw.substring(idx); // strip leading content://
                }
                shownPath = Uri.decode(raw); // decode %3A -> ":" etc.
            } else {
                shownPath = "/tree/primary:RSDK/V5";
            }

            new AlertDialog.Builder(this)
                    .setTitle("Path confirmation")
                    .setMessage("Use the default folder?\n\n" + shownPath +
                            "\n\nor choose a different one?")
                    .setPositiveButton("Use this", (dialog, i) -> {
                        // keep default basePath and proceed (via picker to grant access)
                        folderPicker();
                    })
                    .setNeutralButton("Change", (dialog, i) -> {
                        folderPicker();
                    })
                    .setNegativeButton("Exit", (dialog, i) -> {
                        dialog.cancel();
                        quit(3);
                    })
                    .setCancelable(false)
                    .show();
        } else {
            // Permission already persisted for basePath; start immediately (no timer/dialog)
            try {
                if (DocumentFile.fromTreeUri(this, basePath).findFile(".nomedia") == null)
                    createFile(".nomedia");
            } catch (Exception e) {
            }

            Intent intent = new Intent(this, RSDK.class);
            intent.setData(basePath);
            intent.setFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                    Intent.FLAG_GRANT_READ_URI_PERMISSION |
                    Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            grantUriPermission(getApplicationContext().getPackageName() + ".RSDK", basePath,
                    Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                            Intent.FLAG_GRANT_READ_URI_PERMISSION |
                            Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);

            getContentResolver().takePersistableUriPermission(basePath, takeFlags);

            instance = this;

            // Only warn if we truly don't see Data/ or Data.rsdk (case-insensitive) at the root.
            if (!hasGameDataAtRoot()) {
                new AlertDialog.Builder(this)
                        .setTitle("Missing game data")
                        .setMessage("Place the game data into the selected folder then press OK to proceed.")
                        .setPositiveButton("OK", (dialog, i) -> launchGame())
                        .setCancelable(true)
                        .show();
            } else {
                launchGame();
            }
        }
    }

    private void folderPicker() {
        refreshStore();
        folderLauncher.launch(
                new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                        .putExtra(DocumentsContract.EXTRA_INITIAL_URI, basePath)
                        .addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                                Intent.FLAG_GRANT_READ_URI_PERMISSION |
                                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION));
    }

    // Returns true if the selected root contains either a "Data" directory (any case)
    // or a "Data.rsdk" file (any case). This avoids false "missing data" dialogs.
    private boolean hasGameDataAtRoot() {
        try {
            DocumentFile root = DocumentFile.fromTreeUri(this, basePath);
            if (root == null) return false;
            DocumentFile[] kids = root.listFiles();
            boolean hasDataDir = false;
            boolean hasDataRsdk = false;
            for (DocumentFile f : kids) {
                final String name = f.getName();
                if (name == null) continue;
                if (name.equalsIgnoreCase("Data") && f.isDirectory()) {
                    hasDataDir = true;
                } else if (name.equalsIgnoreCase("Data.rsdk") && f.isFile()) {
                    hasDataRsdk = true;
                }
                if (hasDataDir || hasDataRsdk) return true;
            }
            return false;
        } catch (Throwable t) {
            // If SAF throws for any reason, don't block launch – let the engine handle it.
            return true;
        }
    }

    public Uri createFile(String filename) throws FileNotFoundException {

        DocumentFile path = DocumentFile.fromTreeUri(getApplicationContext(), basePath);
        while (filename.indexOf('/') != -1) {
            String sub = filename.substring(0, filename.indexOf('/'));
            if (!sub.isEmpty()) {
                DocumentFile find = path.findFile(sub);
                if (find == null)
                    path = path.createDirectory(sub);
                else
                    path = find;    
            }
            filename = filename.substring(filename.indexOf('/') + 1);
        }

        DocumentFile find = path.findFile(filename);
        if (find == null)
            return path.createFile("application/octet-stream", filename).getUri();
        else
            return find.getUri();
    }

    // Launch RSDK activity with the currently selected basePath.
    // We use the already-registered ActivityResultLauncher so quit() runs on return.
    private void launchGame() {
        Intent intent = new Intent(this, RSDK.class);
        intent.setData(basePath);
        intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        gameLauncher.launch(intent);
    }
}
