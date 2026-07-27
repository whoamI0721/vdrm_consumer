package com.vdrm.consumer;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

public class VirtualKeyboardView extends View {

    private Paint bgPaint, keyPaint, textPaint, pressPaint;
    private int pressedKey = -1;
    private boolean fnMode = false;
    private boolean shiftMode = false;

    private int[] rowKeyCount;
    private int[] rowWideCount;
    private int[] rowKeyWidth;
    private int[] rowWideWidth;
    private int keyH;
    private int totalH;

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
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setAntiAlias(true);

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

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int w = MeasureSpec.getSize(widthMeasureSpec);
        int gap = KEY_GAP;
        int rows = KEYS.length;

        /* Target height ~ width * 565/1288, capped at 650 */
        int targetH = (int)(w * 565f / 1288f);
        if (targetH > 650) targetH = 650;

        keyH = (targetH - 10 - (rows - 1) * gap) / rows;
        if (keyH < 50) keyH = 50;

        /* Calculate per-row key widths */
        rowKeyWidth = new int[rows];
        rowWideWidth = new int[rows];
        for (int r = 0; r < rows; r++) {
            int n = rowKeyCount[r] - rowWideCount[r];
            int wc = rowWideCount[r];
            int avail = w - (rowKeyCount[r] - 1) * gap;
            int totalU = n + wc * 2;
            int unit = avail / totalU;
            rowKeyWidth[r] = unit;
            rowWideWidth[r] = unit * 2 + gap;
        }
        totalH = 10 + rows * keyH + (rows - 1) * gap;

        setMeasuredDimension(w, totalH);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int w = getWidth();
        canvas.drawRect(0, 0, w, totalH, bgPaint);

        int gap = KEY_GAP;
        for (int r = 0; r < KEYS.length; r++) {
            int x = 0;
            int y = 10 + r * (keyH + gap);
            int kw = rowKeyWidth[r];
            int ww = rowWideWidth[r];

            for (int i = 0; i < KEYS[r].length; i += 4) {
                String label = KEYS[r][i];
                if (label == null) continue;
                String code = KEYS[r][i + 1];
                String shiftLabel = KEYS[r][i + 2];
                int c = i / 4;

                boolean isWide = label.length() > 3;
                int kx = isWide ? ww : kw;
                boolean isFnKey = fnMode && r == 0 && c * 2 < FN_ROW0.length && FN_ROW0[c * 2] != null;
                int idx = r * 20 + c;

                RectF rect = new RectF(x, y, x + kx, y + keyH);
                canvas.drawRoundRect(rect, 5, 5, idx == pressedKey ? pressPaint : keyPaint);

                /* Determine display label */
                String display = label;
                if (isFnKey) {
                    display = FN_ROW0[c * 2];
                } else if (shiftMode && shiftLabel != null) {
                    display = shiftLabel;
                }

                /* Main label */
                float mainSize = isWide ? 22 : Math.min(24, kw * 0.42f);
                textPaint.setTextSize(mainSize);
                canvas.drawText(display, rect.centerX(), rect.centerY() + mainSize * 0.35f, textPaint);

                /* Shift hint (small text at bottom) */
                if (shiftLabel != null && !isFnKey) {
                    float hintSize = Math.min(13, kw * 0.22f);
                    textPaint.setTextSize(hintSize);
                    canvas.drawText(shiftLabel, rect.centerX(), rect.bottom - 3, textPaint);
                }

                /* Fn hint (small text at bottom-left area for number row) */
                if (r == 0 && !fnMode && c * 2 < FN_ROW0.length && FN_ROW0[c * 2] != null) {
                    float fnSize = Math.min(11, kw * 0.2f);
                    textPaint.setTextSize(fnSize);
                    canvas.drawText(FN_ROW0[c * 2], rect.centerX(), rect.top + fnSize + 2, textPaint);
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

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        float x = event.getX();
        float y = event.getY();
        int r = (int)((y - 10) / (keyH + KEY_GAP));
        if (r < 0 || r >= KEYS.length) return true;

        int cx = (int)x;
        int c = -1, xPos = 0;
        for (int i = 0; i < KEYS[r].length; i += 4) {
            String label = KEYS[r][i];
            if (label == null) continue;
            int kw = label.length() > 3 ? rowWideWidth[r] : rowKeyWidth[r];
            if (cx >= xPos && cx < xPos + kw) { c = i / 4; break; }
            xPos += kw + KEY_GAP;
        }
        if (c < 0) return true;
        String codeStr = KEYS[r][c * 4 + 1];

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE: {
                int newIdx = r * 20 + c;
                if (newIdx != pressedKey) {
                    if (pressedKey >= 0) {
                        int pr = pressedKey / 20;
                        int pc = pressedKey % 20;
                        if (pc * 4 < KEYS[pr].length && KEYS[pr][pc * 4] != null) {
                            String oldCode = KEYS[pr][pc * 4 + 1];
                            if (!isSpecial(oldCode)) {
                                int uc = resolveCode(pr, pc, oldCode);
                                Native.nativeSendKey(uc, false);
                            }
                        }
                    }
                    pressedKey = newIdx;
                    if (!isSpecial(codeStr)) {
                        int sc = resolveCode(r, c, codeStr);
                        Native.nativeSendKey(sc, true);
                    }
                    invalidate();
                }
                break;
            }
            case MotionEvent.ACTION_UP: {
                if (pressedKey >= 0) {
                    int pr = pressedKey / 20;
                    int pc = pressedKey % 20;
                    if (pc * 4 < KEYS[pr].length) {
                        String upCode = KEYS[pr][pc * 4 + 1];
                        if (upCode.equals("--") && pressedKey == r * 20 + c) {
                            setVisibility(GONE);
                        } else if (upCode.equals("-1")) {
                            fnMode = !fnMode;
                        } else if (upCode.equals("-2")) {
                            shiftMode = !shiftMode;
                            Native.nativeSendKey(42, shiftMode);
                        } else if (!upCode.equals("--")) {
                            int uc = resolveCode(pr, pc, upCode);
                            Native.nativeSendKey(uc, false);
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
}
