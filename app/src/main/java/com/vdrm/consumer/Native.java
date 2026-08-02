package com.vdrm.consumer;

import android.view.Surface;

public class Native {
    static { System.loadLibrary("vdrm_consumer"); }

    public static native long nativeCreate();
    public static native void nativeDestroy(long handle);
    public static native void nativeStart(long handle, Surface surface);
    public static native void nativeStop(long handle);

    /* FD import test: import container-rendered dma-buf and display it.
     * Red screen = GPU content shown, green screen = import failed. */
    public static native void nativeTestFd(Surface surface);

    /* Event senders (static, no handle needed) */
    public static native int nativeSendKey(int code, boolean down);
    public static native int nativeSendMotion(int dx, int dy);
    public static native int nativeSendBtn(int btn, boolean pressed);
    public static native int nativeSendScroll(int axis, int val);

    /* Audio */
    public static native void nativeStartAudio(long handle);
    public static native void nativeStopAudio(long handle);
}
