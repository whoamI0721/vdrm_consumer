package com.vdrm.consumer;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.ImageButton;
import android.widget.RelativeLayout;

public class MainActivity extends Activity implements SurfaceHolder.Callback {

    private SurfaceView surfaceView;
    private VirtualTouchpad touchpad;
    private VirtualKeyboardView keyboardView;
    private long nativeHandle;
    private PowerManager.WakeLock wakeLock;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        /* Fullscreen */
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                             WindowManager.LayoutParams.FLAG_FULLSCREEN);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }

        /* Keep screen on */
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        /* Wake lock */
        PowerManager pm = getSystemService(PowerManager.class);
        if (pm != null) {
            wakeLock = pm.newWakeLock(PowerManager.SCREEN_BRIGHT_WAKE_LOCK |
                                      PowerManager.ACQUIRE_CAUSES_WAKEUP, "vdrm:wakelock");
            wakeLock.acquire();
        }

        /* Foreground notification (prevents Android from killing us) */
        startForegroundService();

        /* Layout: surface + keyboard overlay */
        RelativeLayout root = new RelativeLayout(this);
        root.setLayoutParams(new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        root.addView(surfaceView, new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));

        /* Touchpad overlay — sits on top of surface, transparent */
        View touchOverlay = new View(this) {
            @Override
            public boolean onTouchEvent(MotionEvent event) {
                if (touchpad != null) return touchpad.onTouch(event);
                return true;
            }
        };
        touchOverlay.setLayoutParams(new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));
        root.addView(touchOverlay);

        /* Keyboard toggle button */
        ImageButton kbBtn = new ImageButton(this);
        kbBtn.setText("⌨");
        kbBtn.setTextSize(20);
        kbBtn.setBackgroundColor(0x60000000);
        RelativeLayout.LayoutParams kbBtnLp = new RelativeLayout.LayoutParams(80, 80);
        kbBtnLp.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        kbBtnLp.addRule(RelativeLayout.ALIGN_PARENT_RIGHT);
        kbBtnLp.setMargins(0, 40, 20, 0);
        kbBtn.setLayoutParams(kbBtnLp);
        root.addView(kbBtn);

        /* Virtual keyboard View — hidden by default */
        keyboardView = new VirtualKeyboardView(this);
        keyboardView.setVisibility(View.GONE);
        RelativeLayout.LayoutParams kvLp = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                350);
        kvLp.addRule(RelativeLayout.ALIGN_PARENT_BOTTOM);
        keyboardView.setLayoutParams(kvLp);
        root.addView(keyboardView);

        kbBtn.setOnClickListener(v -> {
            keyboardView.setVisibility(
                keyboardView.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE);
        });

        setContentView(root);

        touchpad = new VirtualTouchpad(this);
        nativeHandle = Native.nativeCreate();
    }

    @Override
    protected void onDestroy() {
        Native.nativeDestroy(nativeHandle);
        nativeHandle = 0;
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
        }
        stopForegroundService();
        super.onDestroy();
    }

    /* ---- Surface callbacks ---- */

    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Native.nativeStart(nativeHandle, holder.getSurface());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Native.nativeStop(nativeHandle);
    }

    /* ---- Key interception ---- */

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_VOLUME_UP) {
            Native.nativeSendKey(115, true);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) {
            Native.nativeSendKey(114, true);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            Native.nativeSendKey(1, true);  /* KEY_ESC */
            return true;
        }
        int lc = linuxKeyCode(keyCode);
        if (lc >= 0) {
            Native.nativeSendKey(lc, true);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_VOLUME_UP) {
            Native.nativeSendKey(115, false);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) {
            Native.nativeSendKey(114, false);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            Native.nativeSendKey(1, false);
            return true;
        }
        int lc = linuxKeyCode(keyCode);
        if (lc >= 0) {
            Native.nativeSendKey(lc, false);
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    /* Map Android keycode → Linux input keycode */
    private static int linuxKeyCode(int androidCode) {
        switch (androidCode) {
            case KeyEvent.KEYCODE_A: return 30;
            case KeyEvent.KEYCODE_B: return 48;
            case KeyEvent.KEYCODE_C: return 46;
            case KeyEvent.KEYCODE_D: return 32;
            case KeyEvent.KEYCODE_E: return 18;
            case KeyEvent.KEYCODE_F: return 33;
            case KeyEvent.KEYCODE_G: return 34;
            case KeyEvent.KEYCODE_H: return 35;
            case KeyEvent.KEYCODE_I: return 23;
            case KeyEvent.KEYCODE_J: return 36;
            case KeyEvent.KEYCODE_K: return 37;
            case KeyEvent.KEYCODE_L: return 38;
            case KeyEvent.KEYCODE_M: return 50;
            case KeyEvent.KEYCODE_N: return 49;
            case KeyEvent.KEYCODE_O: return 24;
            case KeyEvent.KEYCODE_P: return 25;
            case KeyEvent.KEYCODE_Q: return 16;
            case KeyEvent.KEYCODE_R: return 19;
            case KeyEvent.KEYCODE_S: return 31;
            case KeyEvent.KEYCODE_T: return 20;
            case KeyEvent.KEYCODE_U: return 22;
            case KeyEvent.KEYCODE_V: return 47;
            case KeyEvent.KEYCODE_W: return 17;
            case KeyEvent.KEYCODE_X: return 45;
            case KeyEvent.KEYCODE_Y: return 21;
            case KeyEvent.KEYCODE_Z: return 44;
            case KeyEvent.KEYCODE_0: return 11;
            case KeyEvent.KEYCODE_1: return 2;
            case KeyEvent.KEYCODE_2: return 3;
            case KeyEvent.KEYCODE_3: return 4;
            case KeyEvent.KEYCODE_4: return 5;
            case KeyEvent.KEYCODE_5: return 6;
            case KeyEvent.KEYCODE_6: return 7;
            case KeyEvent.KEYCODE_7: return 8;
            case KeyEvent.KEYCODE_8: return 9;
            case KeyEvent.KEYCODE_9: return 10;
            case KeyEvent.KEYCODE_SPACE: return 57;
            case KeyEvent.KEYCODE_ENTER: return 28;
            case KeyEvent.KEYCODE_TAB: return 15;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return 42;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 54;
            case KeyEvent.KEYCODE_CTRL_LEFT: return 29;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 97;
            case KeyEvent.KEYCODE_ALT_LEFT: return 56;
            case KeyEvent.KEYCODE_ALT_RIGHT: return 100;
            case KeyEvent.KEYCODE_META_LEFT: return 125;
            case KeyEvent.KEYCODE_META_RIGHT: return 126;
            case KeyEvent.KEYCODE_DEL: return 14;
            case KeyEvent.KEYCODE_FORWARD_DEL: return 111;
            case KeyEvent.KEYCODE_ESCAPE: return 1;
            case KeyEvent.KEYCODE_DPAD_UP: return 103;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 108;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 105;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 106;
            case KeyEvent.KEYCODE_PAGE_UP: return 104;
            case KeyEvent.KEYCODE_PAGE_DOWN: return 109;
            case KeyEvent.KEYCODE_HOME: return 102;
            case KeyEvent.KEYCODE_END: return 107;
            case KeyEvent.KEYCODE_INSERT: return 110;
            case KeyEvent.KEYCODE_F1: return 59;
            case KeyEvent.KEYCODE_F2: return 60;
            case KeyEvent.KEYCODE_F3: return 61;
            case KeyEvent.KEYCODE_F4: return 62;
            case KeyEvent.KEYCODE_F5: return 63;
            case KeyEvent.KEYCODE_F6: return 64;
            case KeyEvent.KEYCODE_F7: return 65;
            case KeyEvent.KEYCODE_F8: return 66;
            case KeyEvent.KEYCODE_F9: return 67;
            case KeyEvent.KEYCODE_F10: return 68;
            case KeyEvent.KEYCODE_F11: return 87;
            case KeyEvent.KEYCODE_F12: return 88;
            case KeyEvent.KEYCODE_COMMA: return 51;
            case KeyEvent.KEYCODE_PERIOD: return 52;
            case KeyEvent.KEYCODE_SLASH: return 53;
            case KeyEvent.KEYCODE_SEMICOLON: return 39;
            case KeyEvent.KEYCODE_APOSTROPHE: return 40;
            case KeyEvent.KEYCODE_MINUS: return 12;
            case KeyEvent.KEYCODE_EQUALS: return 13;
            case KeyEvent.KEYCODE_GRAVE: return 41;
            case KeyEvent.KEYCODE_LEFT_BRACKET: return 26;
            case KeyEvent.KEYCODE_RIGHT_BRACKET: return 27;
            case KeyEvent.KEYCODE_BACKSLASH: return 43;
            case KeyEvent.KEYCODE_NUMPAD_ENTER: return 96;
            default: return -1;
        }
    }

    /* ---- Foreground Service ---- */

    private void startForegroundService() {
        Intent intent = new Intent(this, ForegroundService.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent);
        } else {
            startService(intent);
        }
    }

    private void stopForegroundService() {
        Intent intent = new Intent(this, ForegroundService.class);
        stopService(intent);
    }
}
