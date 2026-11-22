package com.oll.ue4esp;

import android.content.Context;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

final class Dbg {
    private static final SimpleDateFormat TS =
            new SimpleDateFormat("HH:mm:ss.SSS", Locale.US);

    private Dbg() {
    }

    static void log(Context context, String message) {
        File file = new File(context.getFilesDir(), "esp.log");
        String line = TS.format(new Date()) + " " + message + "\n";
        try (FileWriter writer = new FileWriter(file, true)) {
            writer.write(line);
        } catch (IOException ignored) {
        }
        android.util.Log.i("UE4ESP", message);
    }

    static String read(Context context) {
        File file = new File(context.getFilesDir(), "esp.log");
        if (!file.exists()) return "(no log yet)";
        try {
            java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.FileReader(file));
            StringBuilder sb = new StringBuilder();
            String line;
            long count = 0;
            while ((line = reader.readLine()) != null) {
                sb.append(line).append('\n');
                if (++count > 400) break;
            }
            reader.close();
            return sb.length() == 0 ? "(empty)" : sb.toString();
        } catch (IOException e) {
            return "log read failed: " + e;
        }
    }

    static void clear(Context context) {
        File file = new File(context.getFilesDir(), "esp.log");
        if (file.exists()) file.delete();
    }
}
