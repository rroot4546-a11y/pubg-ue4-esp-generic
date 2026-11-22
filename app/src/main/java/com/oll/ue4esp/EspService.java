package com.oll.ue4esp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.provider.Settings;
import android.view.WindowManager;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

public final class EspService extends Service {
    public static volatile boolean running = false;

    private static final String CHANNEL = "ue4esp";
    private static final int NOTIFICATION_ID = 4401;

    private final Handler main = new Handler(Looper.getMainLooper());
    private WindowManager windowManager;
    private OverlayView overlay;
    private Process child;
    private Thread reader;
    private volatile boolean stopping;

    @Override
    public void onCreate() {
        super.onCreate();
        startForeground(NOTIFICATION_ID, notification());
    }

    private Notification notification() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            if (nm != null && nm.getNotificationChannel(CHANNEL) == null) {
                nm.createNotificationChannel(new NotificationChannel(
                        CHANNEL, "ESP", NotificationManager.IMPORTANCE_LOW));
            }
        }
        Intent open = new Intent(this, MainActivity.class);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) flags |= PendingIntent.FLAG_IMMUTABLE;
        PendingIntent pi = PendingIntent.getActivity(this, 0, open, flags);
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            b = new Notification.Builder(this, CHANNEL);
        } else {
            b = new Notification.Builder(this);
        }
        return b.setSmallIcon(android.R.drawable.ic_menu_view)
                .setContentTitle("UE4 ESP")
                .setContentText("Root reader active")
                .setContentIntent(pi)
                .setOngoing(true)
                .build();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            stopSelf();
            return START_NOT_STICKY;
        }
        String pkg = intent.getStringExtra("pkg");
        int width = intent.getIntExtra("width", 1080);
        int height = intent.getIntExtra("height", 2400);
        if (pkg == null || pkg.trim().isEmpty()) {
            Dbg.log(this, "start failed: empty package");
            stopSelf();
            return START_NOT_STICKY;
        }

        if (!Settings.canDrawOverlays(this)) {
            Dbg.log(this, "start failed: overlay permission missing");
            stopSelf();
            return START_NOT_STICKY;
        }

        try {
            attachOverlay();
        } catch (RuntimeException e) {
            Dbg.log(this, "attach overlay failed: " + e);
            stopSelf();
            return START_NOT_STICKY;
        }

        File helper = installHelper();
        if (helper == null) {
            Dbg.log(this, "start failed: helper install failed");
            stopSelf();
            return START_NOT_STICKY;
        }
        File cfg = new File(getFilesDir(), "config.txt");
        File log = new File(getFilesDir(), "esp.log");

        running = true;
        Dbg.log(this, "start pkg=" + pkg + " " + width + "x" + height);
        reader = new Thread(() -> runHelper(helper, pkg, width, height, cfg, log), "esp-reader");
        reader.start();
        return START_STICKY;
    }

    private void attachOverlay() {
        windowManager = (WindowManager) getSystemService(Context.WINDOW_SERVICE);
        if (windowManager == null) throw new IllegalStateException("no WindowManager");
        overlay = new OverlayView(this);
        WindowManager.LayoutParams p = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.RGBA_8888);
        p.gravity = android.view.Gravity.TOP | android.view.Gravity.START;
        windowManager.addView(overlay, p);
    }

    private File installHelper() {
        File out = new File(getFilesDir(), "esphelper64");
        try (InputStream in = getAssets().open("esphelper64");
             OutputStream os = new FileOutputStream(out)) {
            byte[] buf = new byte[16384];
            int n;
            while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
        } catch (Exception e) {
            Dbg.log(this, "asset copy failed: " + e);
            return null;
        }
        if (!out.setExecutable(true)) {
            Dbg.log(this, "chmod failed");
            return null;
        }
        try {
            new ProcessBuilder("su", "-c", "chmod 755 " + out.getAbsolutePath())
                    .start().waitFor();
        } catch (Exception e) {
            Dbg.log(this, "su chmod failed: " + e);
        }
        return out;
    }

    private void runHelper(File helper, String pkg, int width, int height, File cfg, File log) {
        String command = helper.getAbsolutePath() + " " + pkg + " " + width + " "
                + height + " " + cfg.getAbsolutePath();
        List<OverlayView.Box> pending = new ArrayList<>();
        try {
            child = new ProcessBuilder("su", "-c", command).start();
            Thread err = new Thread(() -> drain(child.getErrorStream(), true, log), "esp-stderr");
            err.start();
            BufferedReader out = new BufferedReader(new InputStreamReader(child.getInputStream()));
            String line;
            while (!stopping && (line = out.readLine()) != null) {
                if (line.startsWith("T ")) {
                    String[] f = line.split(" ");
                    if (f.length >= 9) {
                        try {
                            pending.add(new OverlayView.Box(
                                    Float.parseFloat(f[1]), Float.parseFloat(f[2]),
                                    Float.parseFloat(f[3]), Float.parseFloat(f[4]),
                                    Integer.parseInt(f[6]),
                                    Float.parseFloat(f[7]), Float.parseFloat(f[8])));
                        } catch (NumberFormatException ignored) {
                        }
                    }
                } else if (line.startsWith("F")) {
                    final List<OverlayView.Box> frame = new ArrayList<>(pending);
                    pending.clear();
                    main.post(() -> {
                        if (overlay != null) overlay.update(frame);
                    });
                }
            }
            int code = child.waitFor();
            Dbg.log(this, "helper exited code=" + code);
        } catch (Exception e) {
            Dbg.log(this, "helper error: " + e);
        } finally {
            final List<OverlayView.Box> empty = new ArrayList<>();
            main.post(() -> {
                if (overlay != null) overlay.update(empty);
            });
            running = false;
            stopSelf();
        }
    }

    private void drain(InputStream stream, boolean toLog, File file) {
        try (BufferedReader r = new BufferedReader(new InputStreamReader(stream))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (toLog) Dbg.log(this, line);
            }
        } catch (Exception ignored) {
        }
    }

    @Override
    public void onDestroy() {
        stopping = true;
        if (child != null) child.destroy();
        if (reader != null) reader.interrupt();
        if (overlay != null && windowManager != null) {
            try {
                windowManager.removeViewImmediate(overlay);
            } catch (RuntimeException ignored) {
            }
            overlay = null;
        }
        running = false;
        stopForeground(true);
        super.onDestroy();
    }

    @Override
    public IBinder onBinder() {
        return null;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
