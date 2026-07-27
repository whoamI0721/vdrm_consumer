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
    private boolean shiftMode = false;

    /* [label, code, shiftLabel?, shiftCode?, ...] — shiftLabel only for symbol keys */
    private static final String[][] KEYS = {
        /* Row 0: number/symbol row */
        {"`","41","~","41",  "1","2","!","2",  "2","3","@","3",
         "3","4","#","4",  "4","5","$","5",  "5","6","%","6",
         "6","7","^","7",  "7","8","&","8",  "8","9","*","9",
         "9","10","(","10",  "0","11",")","11",
         "-","12","_","12",  "=","13","+","13",  "Backspace","14"},

        /* Row 1: q row */
        {"Tab","15",  "q","16",  "w","17",  "e","18",  "r","19",
         "t","20",  "y","21",  "u","22",  "i","23",  "o","24",
         "p","25",  "[","26","{","26",  "]","27","}","27",
         "\\","43","|","43"},

        /* Row 2: a row */
        {"Caps","58",  "a","30",  "s","31",  "d","32",  "f","33",
         "g","34",  "h","35",  "j","36",  "k","37",  "l","38",
         ";","39",":","39",  "'","40","\"","40",  "Enter","28"},

        /* Row 3: z row */
        {"Shift","-2",  "z","44",  "x","45",  "c","46",  "v","47",
         "b","48",  "n","49",  "m","50",
         ",","51","<","51",  ".","52",">","52",  "/","53","?","53",
         "Shift","-2"},

        /* Row 4: bottom row */
        {"Ctrl","29",  "Super","125",  "Alt","56",
         "     Space     ","57",
         "Alt","56",  "Fn","-1"},
    };

    /* Fn mapping for row 0 (label,code pairs; null for no mapping) */
    private static final String[] FN_ROW0 = {
        null, null,                     /* c=0  ` */
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
                boolean hasShift = KEYS[r].length > c * 2 + 2;
                boolean isFnRow0 = fnMode && r == 0 && c * 2 < FN_ROW0.length && FN_ROW0[c * 2] != null;
                int idx = r * COLS + c;

                RectF rect = new RectF(x, y, x + kw, y + KEY_H);
                canvas.drawRoundRect(rect, 4, 4, idx == pressedKey ? pressPaint : keyPaint);

                String display = label;
                if (isFnRow0) {
                    display = FN_ROW0[c * 2];
                } else if (hasShift && shiftMode) {
                    display = KEYS[r][c * 2 + 2];
                }

                canvas.drawText(display, rect.centerX(), rect.centerY() + 6, textPaint);

                if (hasShift && !isFnRow0) {
                    textPaint.setTextSize(11);
                    canvas.drawText(KEYS[r][c * 2 + 2], rect.centerX(), rect.bottom - 3, textPaint);
                    textPaint.setTextSize(18);
                }
                x += kw + KEY_GAP;
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
                    if (pressedKey >= 0) {
                        int pr = pressedKey / COLS;
                        int pc = pressedKey % COLS;
                        if (pc * 2 < KEYS[pr].length && KEYS[pr][pc * 2] != null) {
                            String oldCode = KEYS[pr][pc * 2 + 1];
                            if (!isSpecial(oldCode)) {
                                int upCode = resolveCode(pr, pc, oldCode);
                                Native.nativeSendKey(upCode, false);
                            }
                        }
                    }
                    pressedKey = newIdx;
                    if (!isSpecial(codeStr)) {
                        int sendCode = resolveCode(r, c, codeStr);
                        Native.nativeSendKey(sendCode, true);
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
                        } else if (upCode.equals("-2")) {
                            shiftMode = !shiftMode;
                            Native.nativeSendKey(42, shiftMode);
                        } else if (!upCode.equals("--")) {
                            int upCodeInt = resolveCode(pr, pc, upCode);
                            Native.nativeSendKey(upCodeInt, false);
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
