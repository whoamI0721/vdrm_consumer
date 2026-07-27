package com.vdrm.consumer;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

public class VirtualKeyboardView extends View {

    private static final int KEY_W = 54;
    private static final int KEY_H = 44;
    private static final int KEY_GAP = 2;
    private static final int COLS = 15;

    private Paint bgPaint, keyPaint, textPaint, pressPaint;
    private int pressedKey = -1;
    private boolean fnMode = false;

    /* [label, code, fnLabel?, fnCode?, label, code, ...] */
    /* labels >3 chars get 120px width */
    private static final String[][] KEYS = {
        /* Row 0: function row (14 keys) */
        {"Esc","1",  "F1","59",  "F2","60",  "F3","61",  "F4","62",
         "F5","63",  "F6","64",  "F7","65",  "F8","66",  "F9","67",
         "F10","68", "F11","87", "F12","88", "Hide","--"},

        /* Row 1: number row (14 keys) */
        {"`","41",  "1","2",  "2","3",  "3","4",  "4","5",
         "5","6",  "6","7",  "7","8",  "8","9",  "9","10",
         "0","11",  "-","12",  "=","13",  "Backspace","14"},

        /* Row 2: q row (14 keys) */
        {"Tab","15",  "q","16",  "w","17",  "e","18",  "r","19",
         "t","20",  "y","21",  "u","22",  "i","23",  "o","24",
         "p","25",  "[","26",  "]","27",  "\\","43"},

        /* Row 3: a row (13 keys) */
        {"Caps","58",  "a","30",  "s","31",  "d","32",  "f","33",
         "g","34",  "h","35",  "j","36",  "k","37",  "l","38",
         ";","39",  "'","40",  "Enter","28"},

        /* Row 4: z row (13 keys) */
        {"Shift","42",  "z","44",  "x","45",  "c","46",  "v","47",
         "b","48",  "n","49",  "m","50",  ",","51",  ".","52",
         "/","53",  "Shift","42"},

        /* Row 5: bottom row (11 keys) */
        {"Ctrl","29",  "Super","125",  "Alt","56",
         "     Space     ","57",
         "Alt","56",  "Fn","-1",
         "←","105","↓","108","↑","103","→","106",
         "✕","--"},
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
        textPaint.setTextSize(18);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setAntiAlias(true);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        canvas.drawRect(0, 0, getWidth(), getHeight(), bgPaint);

        int startY = 10;
        for (int r = 0; r < KEYS.length; r++) {
            int rowW = 0;
            int colsOnRow = KEYS[r].length / 2;
            for (int c = 0; c < colsOnRow; c++) {
                String label = KEYS[r][c * 2];
                if (label == null) continue;
                rowW += (label.length() > 3 ? 120 : KEY_W) + KEY_GAP;
            }
            int startX = (getWidth() - rowW) / 2;
            int x = startX;
            int y = startY + r * (KEY_H + KEY_GAP);

            for (int c = 0; c < colsOnRow; c++) {
                String label = KEYS[r][c * 2];
                if (label == null) continue;
                int kw = label.length() > 3 ? 120 : KEY_W;
                String code = KEYS[r][c * 2 + 1];
                boolean hasFn = KEYS[r].length > c * 2 + 2;
                int idx = r * COLS + c;

                RectF rect = new RectF(x, y, x + kw, y + KEY_H);
                canvas.drawRoundRect(rect, 4, 4, idx == pressedKey ? pressPaint : keyPaint);

                String display = label;
                if (hasFn && fnMode) {
                    display = KEYS[r][c * 2 + 2];
                }
                canvas.drawText(display, rect.centerX(), rect.centerY() + 6, textPaint);

                if (hasFn && !fnMode) {
                    String fnLabel = KEYS[r][c * 2 + 2];
                    textPaint.setTextSize(11);
                    canvas.drawText(fnLabel, rect.centerX(), rect.bottom - 3, textPaint);
                    textPaint.setTextSize(18);
                }
                x += kw + KEY_GAP;
            }
        }
    }

    private static boolean isSpecial(String code) {
        return code.equals("--") || code.equals("-1");
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        float x = event.getX();
        float y = event.getY();
        int r = (int)((y - 10) / (KEY_H + KEY_GAP));
        if (r < 0 || r >= KEYS.length) return true;

        int startX = calcStartX(r);
        int cx = (int)(x - startX);
        if (cx < 0) return true;

        int c = 0, xPos = 0;
        int colsOnRow = KEYS[r].length / 2;
        for (; c < colsOnRow; c++) {
            String label = KEYS[r][c * 2];
            if (label == null) continue;
            int kw = label.length() > 3 ? 120 : KEY_W;
            if (cx >= xPos && cx < xPos + kw) break;
            xPos += kw + KEY_GAP;
        }
        if (c >= colsOnRow) return true;
        String codeStr = KEYS[r][c * 2 + 1];

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE: {
                int newIdx = r * COLS + c;
                if (newIdx != pressedKey) {
                    /* release previous key */
                    if (pressedKey >= 0) {
                        int pr = pressedKey / COLS;
                        int pc = pressedKey % COLS;
                        if (pc * 2 < KEYS[pr].length && KEYS[pr][pc * 2] != null) {
                            String oldCode = KEYS[pr][pc * 2 + 1];
                            if (!isSpecial(oldCode) && !oldCode.equals("57")) {
                                Native.nativeSendKey(Integer.parseInt(oldCode), false);
                            }
                        }
                    }
                    pressedKey = newIdx;
                    /* press new key */
                    if (!isSpecial(codeStr) && !codeStr.equals("57")) {
                        Native.nativeSendKey(Integer.parseInt(codeStr), true);
                    }
                    invalidate();
                }
                break;
            }
            case MotionEvent.ACTION_UP: {
                if (pressedKey >= 0) {
                    int pr = pressedKey / COLS;
                    int pc = pressedKey % COLS;
                    if (pc * 2 < KEYS[pr].length) {
                        String upCode = KEYS[pr][pc * 2 + 1];
                        if (upCode.equals("--") && pressedKey == r * COLS + c) {
                            setVisibility(GONE);
                        } else if (upCode.equals("-1")) {
                            fnMode = !fnMode;
                        } else if (!upCode.equals("57")) {
                            Native.nativeSendKey(Integer.parseInt(upCode), false);
                        }
                    }
                }
                pressedKey = -1;
                invalidate();
                break;
            }
        }
        return true;
    }

    private int calcStartX(int r) {
        if (r >= KEYS.length) return 0;
        int rowW = 0;
        int colsOnRow = KEYS[r].length / 2;
        for (int c = 0; c < colsOnRow; c++) {
            String label = KEYS[r][c * 2];
            if (label == null) continue;
            rowW += (label.length() > 3 ? 120 : KEY_W) + KEY_GAP;
        }
        return (getWidth() - rowW) / 2;
    }
}
