package com.oll.ue4esp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

final class OverlayView extends View {
    static final class Box {
        final float l, t, r, b, cx, cy;
        final int dist;

        Box(float l, float t, float r, float b, int dist, float cx, float cy) {
            this.l = l; this.t = t; this.r = r; this.b = b;
            this.dist = dist; this.cx = cx; this.cy = cy;
        }
    }

    private final Paint boxPaint;
    private final Paint dotPaint;
    private final List<Box> boxes = new ArrayList<>();

    OverlayView(Context context) {
        super(context);
        boxPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        boxPaint.setColor(Color.RED);
        boxPaint.setStyle(Paint.Style.STROKE);
        boxPaint.setStrokeWidth(2f);
        dotPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        dotPaint.setColor(Color.YELLOW);
        dotPaint.setStyle(Paint.Style.FILL);
    }

    void update(List<Box> next) {
        synchronized (boxes) {
            boxes.clear();
            boxes.addAll(next);
        }
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        synchronized (boxes) {
            for (Box box : boxes) {
                canvas.drawRect(box.l, box.t, box.r, box.b, boxPaint);
                if (box.cx > 0 && box.cy > 0) {
                    canvas.drawCircle(box.cx, box.cy, 3f, dotPaint);
                }
            }
        }
    }
}
