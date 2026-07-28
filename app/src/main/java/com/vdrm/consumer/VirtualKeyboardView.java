package com.vdrm.consumer;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

public class VirtualKeyboardView extends View {

    private Paint bgPaint, keyPaint, textPaint, pressPaint, barPaint, whiteDotPaint;
    private int pressedKey = -1;
    private boolean fnMode = false;
    private boolean shiftMode = false;

    /* Layout constants */
    private static final int BAR_W = 48;       /* both bars same width */
    private static final float ASPECT_MIN = 7f / 5f;
    private static final float ALPHA_MIN = 0.2f;

    /* State */
    private boolean showExtra = false;
    private float kbAlpha = 1.0f;
    private int baseW, baseH;
    private int kbW, kbH;
    private int screenW, screenH;

    /* Computed layout */
    private int halfKey;
    private int dragBarY, dragBarLeft, dragBarRight;
    private int alphaBarX, alphaBarTop, alphaBarBottom;
    private float dotCX, dotCY, dotR;
    private int keyAreaTop;

    /* Drag (move keyboard) */
    private boolean isDragging = false;
    private float dragStartX, dragStartY;
    private float dragViewStartX, dragViewStartY;

    /* Resize */
    private boolean isResizing = false;
    private float resizeStartX, resizeStartY;
    private int resizeStartW, resizeStartH;

    /* Alpha */
    private boolean isAdjustingAlpha = false;
    private float alphaStartY, alphaStartVal;

    private int[] rowKeyCount;
    private int[] rowWideCount;
    private int[] rowKeyWidth;
    private int[] rowWideWidth;
    private int keyH;
    private int keyAreaH;

    /* [label, code, shiftLabel or null, shiftCode or null] */
    private static final String[][] KEYS = {
        /* Row 0: number/symbol row */
        {"`","41","~","41",  "1","2","!","2",  "2","3","@","3",
         "3","4","#","4",  "4","5","$","5",  "5","6","%","6",
         "6","7","^","7",  "7","8","&","8",  "8","9","*","9",
         "9","10","(","10",  "0","11",")","11",
         "-","12","_","12",  "=","13","+","13",
         "Backspace","14",null,null},

        /* Row 1: q row */
        {"Tab","15",null,null,  "q","16",null,null,
         "w","17",null,null,  "e","18",null,null,
         "r","19",null,null,  "t","20",null,null,
         "y","21",null,null,  "u","22",null,null,
         "i","23",null,null,  "o","24",null,null,
         "p","25",null,null,  "[","26","{","26",
         "]","27","}","27",  "\\","43","|","43"},

        /* Row 2: a row */
        {"Caps","58",null,null,  "a","30",null,null,
         "s","31",null,null,  "d","32",null,null,
         "f","33",null,null,  "g","34",null,null,
         "h","35",null,null,  "j","36",null,null,
         "k","37",null,null,  "l","38",null,null,
         ";","39",":","39",  "'","40","\"","40",
         "Enter","28",null,null},

        /* Row 3: z row */
        {"Shift","-2",null,null,  "z","44",null,null,
         "x","45",null,null,  "c","46",null,null,
         "v","47",null,null,  "b","48",null,null,
         "n","49",null,null,  "m","50",null,null,
         ",","51","<","51",  ".","52",">","52",
         "/","53","?","53",  "Shift","-2",null,null},

        /* Row 4: bottom row */
        {"Ctrl","29",null,null,  "Super","125",null,null,
         "Alt","56",null,null,
         "     Space     ","57",null,null,
         "Alt","56",null,null,  "Fn","-1",null,null},
    };

    private static final int KEY_GAP = 3;

    private static final String[] FN_ROW0 = {
        null, null,
        "F1","59",  "F2","60",  "F3","61",  "F4","62",  "F5","63",
        "F6","64",  "F7","65",  "F8","66",  "F9","67",  "F10","68",
        null, null,  null, null,  null, null,  null, null,
    };

    public VirtualKeyboardView(Context context) {
        super(context);
        bgPaint = new Paint();
        bgPaint.setColor(0x80000000);
        bgPaint.setStyle(Paint.Style.FILL);

        keyPaint = new Paint();
        keyPaint.setColor(0xA0A0A0A0);
        keyPaint.setStyle(Paint.Style.FILL);

        pressPaint = new Paint();
        pressPaint.setColor(0xFFCCCCCC);

        textPaint = new Paint();
        textPaint.setColor(Color.WHITE);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setAntiAlias(true);

        barPaint = new Paint();
        barPaint.setColor(0xFF505050);
        barPaint.setStyle(Paint.Style.FILL);

        whiteDotPaint = new Paint();
        whiteDotPaint.setColor(Color.WHITE);
        whiteDotPaint.setStyle(Paint.Style.FILL);
        whiteDotPaint.setAntiAlias(true);

        initRowData();
    }

    private void initRowData() {
        int rows = KEYS.length;
        rowKeyCount = new int[rows];
        rowWideCount = new int[rows];
        for (int r = 0; r < rows; r++) {
            int n = 0, w = 0;
            for (int i = 0; i < KEYS[r].length; i += 4) {
                String label = KEYS[r][i];
                if (label == null) continue;
                n++;
                if (label.length() > 3) w++;
            }
            rowKeyCount[r] = n;
            rowWideCount[r] = w;
        }
    }

    public void setScreenSize(int w, int h) {
        screenW = w;
        screenH = h;
        baseW = w * 4 / 5;
        baseH = h * 1 / 2;
        if (baseW > w / 2) baseW = w / 2;
        if (baseH > h / 2) baseH = h / 2;
        kbW = baseW;
        kbH = baseH;
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int parentW = MeasureSpec.getSize(widthMeasureSpec);
        int parentH = MeasureSpec.getSize(heightMeasureSpec);
        if (screenW == 0) setScreenSize(parentW, parentH);

        int gap = KEY_GAP;
        int rows = KEYS.length;

        keyH = (kbH - 10 - (rows - 1) * gap) / rows;
        if (keyH < 40) keyH = 40;

        rowKeyWidth = new int[rows];
        rowWideWidth = new int[rows];
        for (int r = 0; r < rows; r++) {
            int n = rowKeyCount[r] - rowWideCount[r];
            int wc = rowWideCount[r];
            int avail = kbW - (rowKeyCount[r] - 1) * gap;
            int totalU = n + wc * 2;
            int unit = avail / totalU;
            rowKeyWidth[r] = unit;
            rowWideWidth[r] = unit * 2 + gap;
        }

        keyAreaH = rows * keyH + (rows - 1) * gap;
        halfKey = rowKeyWidth[0] / 2;

        /* Layout positions */
        dragBarY = halfKey;
        keyAreaTop = dragBarY + BAR_W + halfKey;
        int totalKeyAreaBottom = keyAreaTop + keyAreaH;

        int dragBarWidth = BAR_W * 5;   /* drag bar wider: ~5x bar width */
        dragBarLeft = (kbW - dragBarWidth) / 2;
        dragBarRight = dragBarLeft + dragBarWidth;

        /* Alpha bar: height = key area height - halfKey, centered vertically in key area */
        int alphaBarH = keyAreaH - halfKey;
        alphaBarX = kbW + halfKey;
        alphaBarTop = keyAreaTop + (keyAreaH - alphaBarH) / 2;
        alphaBarBottom = alphaBarTop + alphaBarH;

        /* Dot at intersection of bar center axes */
        dotCX = alphaBarX + BAR_W / 2f;
        dotCY = dragBarY + BAR_W / 2f;
        dotR = BAR_W * 0.75f;   /* diameter = 1.5x bar width */

        int totalW = alphaBarX + BAR_W;
        int totalH = totalKeyAreaBottom;

        setMeasuredDimension(totalW, totalH);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        /* Background over key area only */
        canvas.drawRect(0, keyAreaTop, kbW, keyAreaTop + keyAreaH, bgPaint);

        /* Drag bar (always visible, centered above keyboard) */
        RectF dragBar = new RectF(dragBarLeft, dragBarY, dragBarRight, dragBarY + BAR_W);
        canvas.drawRoundRect(dragBar, 8, 8, barPaint);

        if (showExtra) {
            /* Alpha bar (right side, vertical, gray) */
            RectF alphaBar = new RectF(alphaBarX, alphaBarTop, alphaBarX + BAR_W, alphaBarBottom);
            canvas.drawRoundRect(alphaBar, 8, 8, barPaint);

            /* White dot on alpha bar indicating current alpha */
            float alphaDotY = alphaBarBottom - (alphaBarBottom - alphaBarTop) * kbAlpha;
            canvas.drawCircle(alphaBarX + BAR_W / 2f, alphaDotY, dotR, whiteDotPaint);

            /* Resize dot at intersection of bar center axes */
            canvas.drawCircle(dotCX, dotCY, dotR, barPaint);
            /* Inner highlight */
            Paint innerPaint = new Paint();
            innerPaint.setColor(0xFF808080);
            innerPaint.setStyle(Paint.Style.FILL);
            innerPaint.setAntiAlias(true);
            canvas.drawCircle(dotCX, dotCY, dotR * 0.4f, innerPaint);
        }

        /* Keys */
        int gap = KEY_GAP;
        for (int r = 0; r < KEYS.length; r++) {
            int y = keyAreaTop + r * (keyH + gap);
            int kw = rowKeyWidth[r];
            int ww = rowWideWidth[r];

            int rowW = 0;
            for (int i = 0; i < KEYS[r].length; i += 4) {
                String label = KEYS[r][i];
                if (label == null) continue;
                rowW += (label.length() > 3 ? ww : kw) + gap;
            }
            rowW -= gap;
            int startX = (kbW - rowW) / 2;
            int x = startX;

            for (int i = 0; i < KEYS[r].length; i += 4) {
                String label = KEYS[r][i];
                if (label == null) continue;
                String shiftLabel = KEYS[r][i + 2];
                int c = i / 4;

                boolean isWide = label.length() > 3;
                int kx = isWide ? ww : kw;
                boolean isFnKey = fnMode && r == 0 && c * 2 < FN_ROW0.length && FN_ROW0[c * 2] != null;
                int idx = r * 20 + c;

                RectF rect = new RectF(x, y, x + kx, y + keyH);
                canvas.drawRoundRect(rect, 5, 5, idx == pressedKey ? pressPaint : keyPaint);

                String display = label;
                if (isFnKey) {
                    display = FN_ROW0[c * 2];
                } else if (shiftMode && shiftLabel != null) {
                    display = shiftLabel;
                }

                float mainSize = isWide ? 36 : Math.min(48, kw * 0.58f);
                textPaint.setTextSize(mainSize);
                canvas.drawText(display, rect.centerX(), rect.centerY() + mainSize * 0.35f, textPaint);

                if (r == 0 && !fnMode && c * 2 < FN_ROW0.length && FN_ROW0[c * 2] != null) {
                    float fnSize = Math.min(22, kw * 0.28f);
                    textPaint.setTextSize(fnSize);
                    canvas.drawText(FN_ROW0[c * 2], rect.centerX(), rect.top + fnSize + 3, textPaint);
                }

                x += kx + gap;
            }
        }
    }

    private static boolean isSpecial(String code) {
        return code.equals("--") || code.equals("-1") || code.equals("-2");
    }

    private int resolveCode(int r, int c, String baseCode) {
        if (fnMode && r == 0 && c * 2 < FN_ROW0.length && FN_ROW0[c * 2] != null) {
            return Integer.parseInt(FN_ROW0[c * 2 + 1]);
        }
        return Integer.parseInt(baseCode);
    }

    private int clampW(int w) {
        int maxW = screenW / 2;
        return Math.max(200, Math.min(maxW, w));
    }

    private int clampH(int h) {
        int maxH = screenH / 2;
        return Math.max(150, Math.min(maxH, h));
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        float x = event.getX();
        float y = event.getY();

        /* === Drag bar touch === */
        if (x >= dragBarLeft && x <= dragBarRight && y >= dragBarY && y <= dragBarY + BAR_W) {
            switch (action) {
                case MotionEvent.ACTION_DOWN:
                    return true;
                case MotionEvent.ACTION_MOVE:
                    if (!isDragging && Math.abs(x - (dragBarLeft + dragBarRight) / 2f) > 10) {
                        isDragging = true;
                        dragStartX = x;
                        dragStartY = y;
                        dragViewStartX = getTranslationX();
                        dragViewStartY = getTranslationY();
                    }
                    if (isDragging) {
                        setTranslationX(dragViewStartX + (x - dragStartX));
                        setTranslationY(dragViewStartY + (y - dragStartY));
                    }
                    return true;
                case MotionEvent.ACTION_UP:
                    if (!isDragging) {
                        showExtra = !showExtra;
                        requestLayout();
                        invalidate();
                    }
                    isDragging = false;
                    return true;
            }
        }

        if (isDragging) {
            if (action == MotionEvent.ACTION_MOVE) {
                setTranslationX(dragViewStartX + (x - dragStartX));
                setTranslationY(dragViewStartY + (y - dragStartY));
                return true;
            }
            isDragging = false;
            return true;
        }

        if (!showExtra) return true;

        /* === Resize dot touch === */
        float dx = x - dotCX, dy = y - dotCY;
        if (dx * dx + dy * dy <= (dotR + 10) * (dotR + 10)) {
            switch (action) {
                case MotionEvent.ACTION_DOWN:
                    isResizing = true;
                    resizeStartX = x;
                    resizeStartY = y;
                    resizeStartW = kbW;
                    resizeStartH = kbH;
                    return true;
                case MotionEvent.ACTION_MOVE:
                    if (isResizing) {
                        int newW = (int)(resizeStartW + (x - resizeStartX));
                        int newH = (int)(resizeStartH + (y - resizeStartY));
                        newW = clampW(newW);
                        newH = clampH(newH);
                        if ((float)newW / newH < ASPECT_MIN) {
                            newW = (int)(newH * ASPECT_MIN);
                            newW = clampW(newW);
                        }
                        kbW = newW;
                        kbH = newH;
                        requestLayout();
                        invalidate();
                    }
                    return true;
                case MotionEvent.ACTION_UP:
                    isResizing = false;
                    return true;
            }
        }

        /* === Alpha bar touch === */
        if (x >= alphaBarX && x <= alphaBarX + BAR_W && y >= alphaBarTop && y <= alphaBarBottom) {
            switch (action) {
                case MotionEvent.ACTION_DOWN:
                    isAdjustingAlpha = true;
                    alphaStartY = y;
                    alphaStartVal = kbAlpha;
                    return true;
                case MotionEvent.ACTION_MOVE:
                    if (isAdjustingAlpha) {
                        float dy2 = (alphaStartY - y) / (alphaBarBottom - alphaBarTop);
                        kbAlpha = Math.max(ALPHA_MIN, Math.min(1.0f, alphaStartVal + dy2));
                        setAlpha(kbAlpha);
                        invalidate();
                    }
                    return true;
                case MotionEvent.ACTION_UP:
                    isAdjustingAlpha = false;
                    return true;
            }
        }

        return true;
    }
}
