package com.vdrm.consumer;

import android.content.Context;
import android.view.MotionEvent;
import android.view.ViewConfiguration;

public final class VirtualTouchpad {

    private static final int STATE_IDLE = 0;
    private static final int STATE_ONE_FINGER = 1;
    private static final int STATE_TWO_FINGER = 2;
    private static final int STATE_DRAGGING = 3;
    private int currentState = STATE_IDLE;

    private float lastX1, lastY1;
    private float startX1, startY1;
    private float lastX2, lastY2;
    private long downTime1;
    private final float touchSlop;

    private boolean isSingleTapCandidate = false;
    private boolean isTwoFingerTapCandidate = false;
    private boolean isDraggingActive = false;
    private boolean isMultiFinger = false;

    private static final long TOUCH_LONG_PRESS_TIMEOUT = 500;
    private boolean hasLongPressed = false;
    private boolean isLongPressPossible = false;

    private float mouseAccelStrength = 1.0f;

    /* Smoothing parameters */
    private static final float DEAD_ZONE = 0.3f;
    private static final float SMOOTHING_FACTOR = 0.45f;
    private static final float ACCUMULATED_THRESHOLD = 0.1f;

    private float smoothedDx = 0f;
    private float smoothedDy = 0f;
    private float accumulatedX = 0f;
    private float accumulatedY = 0f;
    private boolean smoothInitialized = false;

    VirtualTouchpad(Context context) {
        touchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
    }

    void setAccelStrength(float strength) {
        mouseAccelStrength = Math.max(0.5f, Math.min(10.0f, strength));
    }

    boolean onTouch(MotionEvent event) {
        int action = event.getActionMasked();
        int pointerCount = event.getPointerCount();

        switch (action) {
            case MotionEvent.ACTION_DOWN: {
                float x = event.getX();
                float y = event.getY();
                startX1 = lastX1 = x;
                startY1 = lastY1 = y;
                downTime1 = event.getEventTime();
                hasLongPressed = false;
                isLongPressPossible = true;
                isSingleTapCandidate = true;
                isTwoFingerTapCandidate = false;
                isDraggingActive = false;
                isMultiFinger = false;
                currentState = STATE_ONE_FINGER;
                resetSmoothing();
                break;
            }
            case MotionEvent.ACTION_POINTER_DOWN: {
                isMultiFinger = true;
                isSingleTapCandidate = false;
                isLongPressPossible = false;
                if (currentState == STATE_DRAGGING) {
                    Native.nativeSendBtn(0x110, false);
                    isDraggingActive = false;
                }
                if (pointerCount == 2) {
                    currentState = STATE_TWO_FINGER;
                    isTwoFingerTapCandidate = true;
                    lastX1 = event.getX(0);
                    lastY1 = event.getY(0);
                    lastX2 = event.getX(1);
                    lastY2 = event.getY(1);
                }
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                if (pointerCount == 1 && !isMultiFinger) {
                    float x = event.getX();
                    float y = event.getY();
                    float rawDx = x - lastX1;
                    float rawDy = y - lastY1;
                    float dist = (float) Math.hypot(x - startX1, y - startY1);

                    if (dist > touchSlop) {
                        isLongPressPossible = false;
                        isSingleTapCandidate = false;
                    }

                    if (isLongPressPossible && !hasLongPressed &&
                            (event.getEventTime() - downTime1) >= TOUCH_LONG_PRESS_TIMEOUT) {
                        hasLongPressed = true;
                        currentState = STATE_DRAGGING;
                        isDraggingActive = true;
                        Native.nativeSendBtn(0x110, true);
                        resetSmoothing();
                        break;
                    }

                    float[] smoothed = applySmoothing(rawDx, rawDy);
                    float smoothDx = smoothed[0];
                    float smoothDy = smoothed[1];

                    if (smoothDx != 0f || smoothDy != 0f) {
                        float distance = (float) Math.hypot(smoothDx, smoothDy);
                        float speedFactor = distance / 10.0f;
                        float dynamicScale = 1.0f + (mouseAccelStrength - 1.0f) * (speedFactor / (1.0f + speedFactor));
                        dynamicScale = Math.max(0.3f, Math.min(10.0f, dynamicScale));

                        int moveX = (int)(smoothDx * dynamicScale);
                        int moveY = (int)(smoothDy * dynamicScale);
                        Native.nativeSendMotion(moveX, moveY);
                    }

                    lastX1 = x;
                    lastY1 = y;

                } else if (pointerCount == 2) {
                    if (currentState == STATE_TWO_FINGER) {
                        float x1 = event.getX(0);
                        float y1 = event.getY(0);
                        float x2 = event.getX(1);
                        float y2 = event.getY(1);
                        float avgDx = ((x1 - lastX1) + (x2 - lastX2)) / 2;
                        float avgDy = ((y1 - lastY1) + (y2 - lastY2)) / 2;

                        if (Math.abs(avgDx) > 1 || Math.abs(avgDy) > 1) {
                            isTwoFingerTapCandidate = false;
                            if (Math.abs(avgDy) > Math.abs(avgDx) * 0.5) {
                                Native.nativeSendScroll(0, -(int)(avgDy * 0.5f));
                            }
                            if (Math.abs(avgDx) > Math.abs(avgDy) * 0.5) {
                                Native.nativeSendScroll(1, (int)(avgDx * 0.5f));
                            }
                            lastX1 = x1;
                            lastY1 = y1;
                            lastX2 = x2;
                            lastY2 = y2;
                        }
                    }
                }
                break;
            }
            case MotionEvent.ACTION_POINTER_UP: {
                int remaining = pointerCount - 1;
                if (remaining == 1) {
                    isMultiFinger = false;
                    isSingleTapCandidate = false;
                    isLongPressPossible = false;
                    int idx = (event.getActionIndex() == 0) ? 1 : 0;
                    lastX1 = event.getX(idx);
                    lastY1 = event.getY(idx);
                    startX1 = lastX1;
                    startY1 = lastY1;
                    downTime1 = event.getEventTime();
                    hasLongPressed = false;
                    currentState = STATE_ONE_FINGER;
                    resetSmoothing();
                }
                break;
            }
            case MotionEvent.ACTION_UP: {
                long duration = event.getEventTime() - downTime1;
                boolean isQuickTap = duration < 300;

                if (isDraggingActive) {
                    Native.nativeSendBtn(0x110, false);
                    isDraggingActive = false;
                    resetState();
                    resetSmoothing();
                    return true;
                }

                if (isTwoFingerTapCandidate && isQuickTap) {
                    Native.nativeSendBtn(0x111, true);
                    Native.nativeSendBtn(0x111, false);
                    resetState();
                    resetSmoothing();
                    return true;
                }

                if (currentState == STATE_ONE_FINGER && isSingleTapCandidate && isQuickTap) {
                    Native.nativeSendBtn(0x110, true);
                    Native.nativeSendBtn(0x110, false);
                    resetState();
                    resetSmoothing();
                    return true;
                }
                resetState();
                resetSmoothing();
                break;
            }
            case MotionEvent.ACTION_CANCEL: {
                if (isDraggingActive) {
                    Native.nativeSendBtn(0x110, false);
                    isDraggingActive = false;
                }
                resetState();
                resetSmoothing();
                break;
            }
        }
        return true;
    }

    private void resetState() {
        currentState = STATE_IDLE;
        isSingleTapCandidate = false;
        isTwoFingerTapCandidate = false;
        hasLongPressed = false;
        isDraggingActive = false;
        isLongPressPossible = false;
        isMultiFinger = false;
    }

    private void resetSmoothing() {
        smoothedDx = 0f;
        smoothedDy = 0f;
        accumulatedX = 0f;
        accumulatedY = 0f;
        smoothInitialized = false;
    }

    private float[] applySmoothing(float rawDx, float rawDy) {
        float deadDx = Math.abs(rawDx) < DEAD_ZONE ? 0f : rawDx;
        float deadDy = Math.abs(rawDy) < DEAD_ZONE ? 0f : rawDy;

        if (deadDx == 0f && deadDy == 0f) {
            return new float[]{0f, 0f};
        }

        if (!smoothInitialized) {
            smoothedDx = deadDx;
            smoothedDy = deadDy;
            smoothInitialized = true;
        } else {
            smoothedDx = SMOOTHING_FACTOR * deadDx + (1 - SMOOTHING_FACTOR) * smoothedDx;
            smoothedDy = SMOOTHING_FACTOR * deadDy + (1 - SMOOTHING_FACTOR) * smoothedDy;
        }

        accumulatedX += smoothedDx;
        accumulatedY += smoothedDy;

        float outX = 0f;
        float outY = 0f;
        if (Math.abs(accumulatedX) >= ACCUMULATED_THRESHOLD) {
            outX = accumulatedX;
            accumulatedX = 0f;
        }
        if (Math.abs(accumulatedY) >= ACCUMULATED_THRESHOLD) {
            outY = accumulatedY;
            accumulatedY = 0f;
        }
        return new float[]{outX, outY};
    }
}
