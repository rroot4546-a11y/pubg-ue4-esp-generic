package com.oll.ue4esp;

import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.view.Display;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;

public final class MainActivity extends android.app.Activity {

    private EditText pkgField;
    private EditText widthField;
    private EditText heightField;
    private CheckBox manualOffsets;
    private EditText offsetsField;
    private TextView status;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle("PUBG Flash ESP — com.tencent.igfit");

        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.TOP);
        int pad = 24;
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(scroll);

        status = new TextView(this);
        status.setTextSize(14f);
        root.addView(status);

        pkgField = field(root, "Package (PUBG Flash)", "com.tencent.igfit");
        widthField = field(root, "Screen width", "1080");
        heightField = field(root, "Screen height", "2400");

        manualOffsets = new CheckBox(this);
        manualOffsets.setText("Manual offsets (skip auto-discovery)");
        root.addView(manualOffsets);

        offsetsField = new EditText(this);
        offsetsField.setHint("GWorld=0x...\nUWorld_Level=0x30\nULevel_Actors=0xa0\n...");
        offsetsField.setMinLines(4);
        root.addView(offsetsField);

        Button overlayBtn = button(root, "1. Grant overlay permission");
        overlayBtn.setOnClickListener(v -> {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
                Intent i = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                startActivity(i);
            } else {
                toast("Overlay permission already granted");
            }
        });

        Button presetBtn = button(root, "Flash 4.6.1 preset (fill offsets)");
        presetBtn.setOnClickListener(v -> {
            manualOffsets.setChecked(true);
            offsetsField.setText(
                    "lib=libUE4.so\n" +
                    "GWorld=0xF3B85F8\n" +
                    "UWorld_Level=0x30\n" +
                    "ULevel_Actors=0xA0\n" +
                    "AActor_Root=0x168\n" +
                    "UWorld_FirstPC=0x0\n" +
                    "PC_AckPawn=0x0\n" +
                    "PC_CamMgr=0x0\n" +
                    "CamMgr_POV=0x0\n");
            toast("preset filled — edit POV offsets if needed");
        });

        Button startBtn = button(root, "2. Start ESP");
        startBtn.setOnClickListener(v -> startEsp());

        Button stopBtn = button(root, "3. Stop ESP");
        stopBtn.setOnClickListener(v -> {
            stopService(new Intent(this, EspService.class));
            toast("stopping");
        });

        Button logBtn = button(root, "Show log");
        logBtn.setOnClickListener(v -> showLog());

        Button clearBtn = button(root, "Clear log");
        clearBtn.setOnClickListener(v -> {
            Dbg.clear(this);
            toast("log cleared");
        });

        Display d = getWindowManager().getDefaultDisplay();
        android.graphics.Point size = new android.graphics.Point();
        d.getRealSize(size);
        widthField.setText(String.valueOf(size.x));
        heightField.setText(String.valueOf(size.y));
        refresh();
    }

    private EditText field(LinearLayout parent, String hint, String value) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setText(value);
        parent.addView(e);
        return e;
    }

    private Button button(LinearLayout parent, String text) {
        Button b = new Button(this);
        b.setText(text);
        parent.addView(b);
        return b;
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private void refresh() {
        boolean overlay = Build.VERSION.SDK_INT < Build.VERSION_CODES.M
                || Settings.canDrawOverlays(this);
        status.setText("overlay permission: " + (overlay ? "GRANTED" : "MISSING")
                + "\nservice: " + (EspService.running ? "RUNNING" : "stopped")
                + "\nhelper: assets/esphelper64 (ARM64, root)");
    }

    private void toast(String text) {
        Toast.makeText(this, text, Toast.LENGTH_SHORT).show();
    }

    private void startEsp() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            toast("grant overlay permission first");
            return;
        }
        String pkg = pkgField.getText().toString().trim();
        if (pkg.isEmpty()) {
            toast("enter a package name");
            return;
        }
        int w, h;
        try {
            w = Integer.parseInt(widthField.getText().toString().trim());
            h = Integer.parseInt(heightField.getText().toString().trim());
        } catch (NumberFormatException e) {
            toast("bad screen size");
            return;
        }
        if (w <= 0 || h <= 0) {
            toast("bad screen size");
            return;
        }

        File cfg = new File(getFilesDir(), "config.txt");
        try (FileWriter writer = new FileWriter(cfg, false)) {
            writer.write("width=" + w + "\nheight=" + h + "\n");
            if (manualOffsets.isChecked()) {
                String text = offsetsField.getText().toString();
                if (text.trim().isEmpty()) {
                    toast("offsets text is empty");
                    return;
                }
                writer.write(text);
                if (!text.endsWith("\n")) writer.write("\n");
            }
        } catch (IOException e) {
            toast("config write failed: " + e.getMessage());
            return;
        }

        Dbg.clear(this);
        Dbg.log(this, "config written for " + pkg);
        Intent i = new Intent(this, EspService.class);
        i.putExtra("pkg", pkg);
        i.putExtra("width", w);
        i.putExtra("height", h);
        startForegroundService(i);
        toast("ESP starting");
        refresh();
    }

    private void showLog() {
        AlertDialog.Builder b = new AlertDialog.Builder(this);
        TextView t = new TextView(this);
        t.setText(Dbg.read(this));
        t.setTextSize(11f);
        ScrollView s = new ScrollView(this);
        s.addView(t);
        b.setTitle("ESP log")
                .setView(s)
                .setPositiveButton("close", null)
                .show();
    }
}
