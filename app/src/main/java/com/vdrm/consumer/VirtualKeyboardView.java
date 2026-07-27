package com.vdrm.consumer;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

public class VirtualKeyboardView extends View {

    private static final int KEY_W = 60;
    private static final int KEY_H = 50;
    private static final int KEY_GAP = 2;
    private static final int ROWS = 6;
    private static final int COLS = 15;

    private Paint bgPaint, keyPaint, textPaint, pressPaint;
    private int pressedKey = -1;
    private boolean fnMode = false;

    /* key label, linux keycode, fn-label, fn-keycode */
    private static final String[][] KEYS = {
        {"Esc", "41",  null,  null,   "Tab", "15",  "Ctrl","29",  "Alt", "56",  "Super","125",  "←", "105", "↑", "103", "→", "106", "↓", "108"},
        {"`",   "41",  "F1",  "59",   "1",   "2",    "F2",  "60",  "2",   "3",   "F3",  "61",   "3",  "4",   "F4", "62",  "4",  "5",   "F5", "63"},
        {"5",   "6",   "F6",  "64",   "6",   "7",    "F7",  "65",  "7",   "8",   "F8",  "66",   "8",  "9",   "F9", "67",  "9",  "10",  "F10","68"},
        {"0",   "11",  "F11", "87",   "-",   "12",   "=",   "13",  "BS",  "14",  null,  null,   null, null,  null, null,  null, null,  null, null},
        {"q",   "16",  "w",   "17",   "e",   "18",   "r",   "19",  "t",   "20",  "y",   "21",   "u",  "22",  "i",  "23",  "o",  "24",  "p",  "25"},
        {"[",   "26",  "]",   "27",   "\\",  "43",   "Del", "111", null,  null,  null,  null,   null, null,  null, null,  null, null,  null, null},
        {"Caps","58",  "a",   "30",   "s",   "31",   "d",   "32",  "f",   "33",  "g",   "34",   "h",  "35",  "j",  "36",  "k",  "37",  "l",  "38"},
        {";",   "39",  "'",   "40",   "Ent", "28",   null,  null,  null,  null,  null,  null,   null, null,  null, null,  null, null,  null, null},
        {"Shft","42",  "z",   "44",   "x",   "45",   "c",   "46",  "v",   "47",  "b",   "48",   "n",  "49",  "m",  "50",  ",",  "51",  ".",  "52"},
        {"/",   "53",  "Shft","42",   null,  null,   null,  null,  null,  null,  null,  null,   null, null,  null, null,  null, null,  null, null},
        {"Ctrl","29",  "Super","125","Alt",  "56",   "      Space      ", "57", "Alt","56","Fn","--","Hide","--", null, null, null, null, null, null},
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
        for (int r = 0; r < ROWS && r < KEYS.length; r++) {
            int colsOnRow = KEYS[r].length / 2;
            int rowW = 0;
            for (int c = 0; c < colsOnRow && c < COLS; c++) {
                String label = KEYS[r][c * 2];
                if (label == null) continue;
                int kw = label.length() > 3 ? 120 : KEY_W;
                rowW += kw + KEY_GAP;
            }
            int startX = (getWidth() - rowW) / 2;
            int x = startX;
            int y = startY + r * (KEY_H + KEY_GAP);

            for (int c = 0; c < colsOnRow && c < COLS; c++) {
                String label = KEYS[r][c * 2];
                if (label == null) continue;
                int kw = label.length() > 3 ? 120 : KEY_W;
                String key = KEYS[r][c * 2 + 1];
                int idx = r * COLS + c;

                RectF rect = new RectF(x, y, x + kw, y + KEY_H);
                canvas.drawRoundRect(rect, 4, 4, idx == pressedKey ? pressPaint : keyPaint);

                String display = (!fnMode && key.equals("Fn")) ? "Fn" :
                                 (fnMode && KEYS[r][c * 2 + 0] != null && c % 2 == 0 && r < KEYS.length ? label : label);
                if (fnMode && KEYS[r].length > c * 2 + 2 && KEYS[r][c * 2 + 2] != null) {
                    String fnLabel = KEYS[r][c * 2 + 2];
                    canvas.drawText(fnLabel, rect.centerX(), rect.centerY() + 6, textPaint);
                } else {
                    canvas.drawText(display, rect.centerX(), rect.centerY() + 6, textPaint);
                }
                x += kw + KEY_GAP;
            }
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        float x = event.getX();
        float y = event.getY();
        int r = (int)((y - 10) / (KEY_H + KEY_GAP));
        if (r < 0 || r >= ROWS || r >= KEYS.length) return true;

        int startY = 10;
        int startX = calcStartX(r);
        int cx = (int)(x - startX);
        if (cx < 0) return true;

        int c = 0;
        int xPos = 0;
        int colsOnRow = KEYS[r].length / 2;
        for (; c < colsOnRow; c++) {
            String label = KEYS[r][c * 2];
            if (label == null) continue;
            int kw = label.length() > 3 ? 120 : KEY_W;
            if (cx >= xPos && cx < xPos + kw) break;
            xPos += kw + KEY_GAP;
        }
        if (c >= colsOnRow) return true;
        String label = KEYS[r][c * 2];
        int kw = label.length() > 3 ? 120 : KEY_W;

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE: {
                int newIdx = r * COLS + c;
                if (newIdx != pressedKey) {
                    if (pressedKey >= 0) {
                        int pr = pressedKey / COLS;
                        int pc = pressedKey % COLS;
                        if (pc * 2 < KEYS[pr].length && KEYS[pr][pc * 2] != null) {
                            String oldKey = KEYS[pr][pc * 2 + 1];
                            if (oldKey.equals("--")) { /* hide btn handle separately */ }
                            if (!oldKey.equals("--") && !oldKey.equals("57")) { /* not space */
                                Native.nativeSendKey(Integer.parseInt(oldKey), false);
                            }
                        }
                    }
                    pressedKey = newIdx;
                    if (label.equals("Fn") || label.equals("fn")) {
                        fnMode = true;
                    }
                    String codeStr = KEYS[r][c * 2 + 1];
                    if (!codeStr.equals("--") && !codeStr.equals("57")) {
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
                        String labelUp = KEYS[pr][pc * 2];
                        String codeStrUp = KEYS[pr][pc * 2 + 1];
                        if (labelUp != null && codeStrUp.equals("--")) {
                            if (pressedKey == r * COLS + c) {
                                /* Hide button */
                                setVisibility(GONE);
                            }
                        }
                        if (!codeStrUp.equals("--") && !codeStrUp.equals("57")) {
                            Native.nativeSendKey(Integer.parseInt(codeStrUp), false);
                        }
                        if (labelUp != null && (labelUp.equals("Fn") || labelUp.equals("fn"))) {
                            fnMode = !fnMode;
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
