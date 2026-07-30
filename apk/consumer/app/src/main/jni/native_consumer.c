#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "anw_hidden.h"

#define TAG "VDRM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define VDRM_MAGIC "/vdrm_magic"
#define MAX_BUFS 6

/* ---- Path helpers (event / audio) ---- */

static int vdrm_open_path(const char *fmt, ...)
{
    char path[128];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(path, sizeof(path), fmt, ap);
    va_end(ap);
    if (len <= 0 || len >= (int)sizeof(path)) return -EINVAL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    close(fd);
    return 0;
}

static int vdrm_send_key(int code, int down)
{
    return vdrm_open_path(VDRM_MAGIC "/ev/key/%s/%d", down ? "down" : "up", code);
}

static int vdrm_send_motion(int dx, int dy)
{
    return vdrm_open_path(VDRM_MAGIC "/ev/motion/dx/%d/dy/%d", dx, dy);
}

static int vdrm_send_btn(int btn, int pressed)
{
    const char *name;
    if (btn == 0x110) name = "left";
    else if (btn == 0x111) name = "right";
    else if (btn == 0x112) name = "middle";
    else return -EINVAL;
    return vdrm_open_path(VDRM_MAGIC "/ev/btn/%s/%d", name, pressed ? 1 : 0);
}

static int vdrm_send_scroll(int axis, int val)
{
    return vdrm_open_path(VDRM_MAGIC "/ev/scroll/%d/%d", axis, val);
}

/* ---- ANativeWindow hidden API ---- */

struct buf_info {
    ANativeWindowBuffer *anb;
    int idx;
};

struct consumer {
    ANativeWindow *win;
    pthread_t thread;
    volatile bool running;

    int buf_count;
    struct buf_info bufs[MAX_BUFS];

    int screen_w;
    int screen_h;

    /* Audio */
    pthread_t play_thr;
    pthread_t cap_thr;
    volatile bool audio_running;
    int play_fd;
    int cap_fd;
};

static struct anw_api api;
static bool api_loaded;

static int vdrm_submit(int dmabuf_fd)
{
    char path[64];
    int len = snprintf(path, sizeof(path), VDRM_MAGIC "/submit/%d", dmabuf_fd);
    if (len <= 0) return -EINVAL;

    for (int i = 0; i < 50; i++) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) { close(fd); return 0; }
        int err = errno;
        if (err != EAGAIN) {
            LOGE("submit open failed: %s", strerror(err));
            return -err;
        }
        usleep(20000);
    }
    LOGE("submit EAGAIN timeout");
    return -EAGAIN;
}

static int vdrm_present(int idx)
{
    char path[64];
    int len = snprintf(path, sizeof(path), VDRM_MAGIC "/present/%d", idx);
    if (len <= 0) return -EINVAL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOGE("present open failed: %s", strerror(errno));
        return -errno;
    }
    close(fd);
    return 0;
}

static int vdrm_wait_fence(int idx)
{
    char path[64];
    int len = snprintf(path, sizeof(path), VDRM_MAGIC "/fence/%d", idx);
    if (len <= 0) return -EINVAL;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -errno;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    poll(&pfd, 1, 5000);
    close(fd);
    return 0;
}

static int collect_buffers(struct consumer *c)
{
    ANativeWindow *win = c->win;
    int target = c->buf_count;
    int found = 0;

    LOGI("collecting %d buffers", target);

    for (int attempt = 0; attempt < target * 4 && found < target; attempt++) {
        ANativeWindowBuffer *anb = NULL;
        int fence = -1;
        int dq_ret = api.dequeueBuffer(win, &anb, &fence);
        if (dq_ret != 0 || !anb) {
            LOGE("dequeue failed attempt %d ret=%d anb=%p fence=%d err=%s",
                 attempt, dq_ret, (void*)anb, fence, strerror(-dq_ret));
        if (fence >= 0) {
            struct pollfd pfd = { .fd = fence, .events = POLLIN };
            poll(&pfd, 1, 1000);
            close(fence);
        }
            usleep(16000);
            continue;
        }
        if (fence >= 0) close(fence);

        if (!anb->handle || anb->handle->numFds < 1) {
            LOGE("buffer has no dma-buf fd attempt %d", attempt);
            api.cancelBuffer(win, anb, -1);
            continue;
        }

        int fd = anb->handle->data[0];

        bool is_dup = false;
        for (int i = 0; i < found; i++) {
            if (c->bufs[i].anb == anb) { is_dup = true; break; }
        }

        int submit_fd = -1;
        if (!is_dup) submit_fd = dup(fd);

        api.queueBuffer(win, anb, -1);

        if (is_dup) continue;
        if (submit_fd < 0) continue;

        int ret = vdrm_submit(submit_fd);
        close(submit_fd);
        if (ret < 0) {
            LOGE("submit failed fd=%d ret=%d", fd, ret);
            continue;
        }

        c->bufs[found].anb = anb;
        c->bufs[found].idx = found;
        LOGI("  buf[%d]: anb=%p fd=%d %dx%d stride=%d idx=%d",
             found, (void *)anb, fd, anb->width, anb->height, anb->stride, found);
        found++;
    }

    if (found < target) {
        LOGE("only collected %d/%d", found, target);
        return -1;
    }

    c->buf_count = found;
    LOGI("collected %d buffers", found);
    return 0;
}

static void *render_loop(void *arg)
{
    struct consumer *c = arg;
    LOGI("render loop started");

    while (c->running) {
        ANativeWindowBuffer *anb = NULL;
        int fence = -1;
        fence = -1;
        int dq_ret = api.dequeueBuffer(c->win, &anb, &fence);
        if (dq_ret != 0 || !anb) {
            LOGE("render dequeue failed ret=%d err=%s", dq_ret, strerror(-dq_ret));
            usleep(16000);
            continue;
        }

        if (fence >= 0) {
            struct pollfd pfd = { .fd = fence, .events = POLLIN };
            poll(&pfd, 1, 1000);
            close(fence);
        }

        int idx = -1;
        for (int i = 0; i < c->buf_count; i++) {
            if (c->bufs[i].anb == anb) { idx = i; break; }
        }

        if (idx < 0) {
            api.queueBuffer(c->win, anb, -1);
            usleep(16000);
            continue;
        }

        int ret = vdrm_present(idx);
        if (ret < 0) {
            if (ret == -EINTR) {
                LOGI("present interrupted by signal");
                c->running = false;
            }
            api.queueBuffer(c->win, anb, -1);
            continue;
        }

        int fwait = vdrm_wait_fence(idx);
        if (fwait < 0) {
            LOGI("fence wait failed idx=%d ret=%d", idx, fwait);
        }
        api.queueBuffer(c->win, anb, -1);
    }

    LOGI("render loop stopped");
    return NULL;
}

/* ---- Audio threads ---- */

static void *audio_play_thread(void *arg)
{
    struct consumer *c = arg;

    int fd = open(VDRM_MAGIC "/audio/play", O_RDONLY);
    if (fd < 0) { LOGE("audio play open failed"); return NULL; }
    c->play_fd = fd;

    AAudioStreamBuilder *bld = NULL;
    AAudio_createStreamBuilder(&bld);
    AAudioStreamBuilder_setDirection(bld, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(bld, 48000);
    AAudioStreamBuilder_setChannelCount(bld, 2);
    AAudioStreamBuilder_setPerformanceMode(bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);

    AAudioStream *stream = NULL;
    if (AAudioStreamBuilder_openStream(bld, &stream) != AAUDIO_OK) {
        LOGE("audio play open stream failed");
        AAudioStreamBuilder_delete(bld);
        close(fd);
        c->play_fd = -1;
        return NULL;
    }
    AAudioStreamBuilder_delete(bld);
    if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
        LOGE("audio play start failed");
        AAudioStream_close(stream);
        close(fd);
        c->play_fd = -1;
        return NULL;
    }

    LOGI("audio play thread started");

    short buf[960 * 2]; /* 10ms @ 48kHz stereo */
    while (c->audio_running) {
        int n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            AAudioStream_write(stream, buf, n / sizeof(short), 1000000000LL);
        } else if (n == 0) {
            usleep(10000);
        } else if (n < 0 && errno != EINTR) {
            usleep(10000);
        }
    }

    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    close(fd);
    c->play_fd = -1;
    LOGI("audio play thread stopped");
    return NULL;
}

static void *audio_cap_thread(void *arg)
{
    struct consumer *c = arg;

    int fd = open(VDRM_MAGIC "/audio/cap", O_WRONLY);
    if (fd < 0) { LOGE("audio cap open failed"); return NULL; }
    c->cap_fd = fd;

    AAudioStreamBuilder *bld = NULL;
    AAudio_createStreamBuilder(&bld);
    AAudioStreamBuilder_setDirection(bld, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(bld, 48000);
    AAudioStreamBuilder_setChannelCount(bld, 1);
    AAudioStreamBuilder_setPerformanceMode(bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);

    AAudioStream *stream = NULL;
    if (AAudioStreamBuilder_openStream(bld, &stream) != AAUDIO_OK) {
        LOGE("audio cap open stream failed");
        AAudioStreamBuilder_delete(bld);
        close(fd);
        c->cap_fd = -1;
        return NULL;
    }
    AAudioStreamBuilder_delete(bld);
    if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
        LOGE("audio cap start failed");
        AAudioStream_close(stream);
        close(fd);
        c->cap_fd = -1;
        return NULL;
    }

    LOGI("audio cap thread started");

    short buf[480 * 1]; /* 10ms @ 48kHz mono */
    while (c->audio_running) {
        int n = AAudioStream_read(stream, buf, sizeof(buf) / sizeof(short), 1000000000LL);
        if (n > 0) {
            write(fd, buf, n * sizeof(short));
        } else {
            usleep(10000);
        }
    }

    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    close(fd);
    c->cap_fd = -1;
    LOGI("audio cap thread stopped");
    return NULL;
}

/* ---- JNI (static: event helpers don't need handle) ---- */

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendKey(JNIEnv *env, jclass clazz, jint code, jboolean down)
{
    (void)env; (void)clazz;
    return vdrm_send_key((int)code, down ? 1 : 0);
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendMotion(JNIEnv *env, jclass clazz, jint dx, jint dy)
{
    (void)env; (void)clazz;
    return vdrm_send_motion((int)dx, (int)dy);
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendBtn(JNIEnv *env, jclass clazz, jint btn, jboolean pressed)
{
    (void)env; (void)clazz;
    return vdrm_send_btn((int)btn, pressed ? 1 : 0);
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendScroll(JNIEnv *env, jclass clazz, jint axis, jint val)
{
    (void)env; (void)clazz;
    return vdrm_send_scroll((int)axis, (int)val);
}

/* ---- JNI (instance: lifecycle) ---- */

#define STATE(h) ((struct consumer *)(uintptr_t)(h))

static void sighandler_noop(int sig) { (void)sig; }

JNIEXPORT jlong JNICALL
Java_com_vdrm_consumer_Native_nativeCreate(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    struct consumer *c = calloc(1, sizeof(*c));
    if (!c) return 0;
    c->play_fd = -1;
    c->cap_fd = -1;

    /* SIGUSR1: empty handler instead of SIG_IGN — SIG_IGN 会让内核吞掉信号，
     * pthread_kill(SIGUSR1) 无法唤醒 present 里阻塞的线程。空 handler 可
     * 让信号正常传递，唤醒线程，handler 返回后线程继续执行。 */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sighandler_noop;
        sigaction(SIGUSR1, &sa, NULL);
    }

    LOGI("instance %p created", (void *)c);
    return (jlong)(uintptr_t)c;
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeDestroy(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c) return;

    if (c->audio_running) {
        c->audio_running = false;
        pthread_join(c->play_thr, NULL);
        pthread_join(c->cap_thr, NULL);
    }
    if (c->running) {
        c->running = false;
        pthread_kill(c->thread, SIGUSR1);
        pthread_join(c->thread, NULL);
    }
    if (c->win) {
        ANativeWindow_release(c->win);
        c->win = NULL;
    }
    LOGI("instance %p destroyed", (void *)c);
    free(c);
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStart(JNIEnv *env, jclass clazz, jlong handle, jobject surface)
{
    struct consumer *c = STATE(handle);
    if (!c) return;

    if (!api_loaded) {
        if (anw_api_load(&api) < 0) {
            LOGE("failed to load ANativeWindow hidden API");
            return;
        }
        api_loaded = true;
    }

    if (c->running) {
        c->running = false;
        pthread_kill(c->thread, SIGUSR1);
        pthread_join(c->thread, NULL);
    }
    if (c->audio_running) {
        c->audio_running = false;
        pthread_join(c->play_thr, NULL);
        pthread_join(c->cap_thr, NULL);
    }
    if (c->win) {
        ANativeWindow_release(c->win);
        c->win = NULL;
    }

    c->win = ANativeWindow_fromSurface(env, surface);
    if (!c->win) {
        LOGE("ANativeWindow_fromSurface failed");
        return;
    }

    c->screen_w = ANativeWindow_getWidth(c->win);
    c->screen_h = ANativeWindow_getHeight(c->win);

    int min_ud = 0;
    api.query(c->win, ANATIVEWINDOW_QUERY_MIN_UNDEQUEUED_BUFFERS, &min_ud);
    c->buf_count = min_ud + 2;
    if (c->buf_count > MAX_BUFS) c->buf_count = MAX_BUFS;

    /* Sanity check: can we lock/unlock (public API)? */
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(c->win, &buf, NULL) == 0) {
        LOGI("lock OK: %dx%d stride=%d fmt=%d bits=%p",
             buf.width, buf.height, buf.stride, buf.format, buf.bits);
        ANativeWindow_unlockAndPost(c->win);
    } else {
        LOGE("lock FAILED - surface may be invalid");
    }

    LOGI("min_ud=%d buf_count=%d", min_ud, c->buf_count);
    int geo_ret = ANativeWindow_setBuffersGeometry(c->win, c->screen_w, c->screen_h,
                                     AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    int sbc_ret = api.setBufferCount(c->win, c->buf_count);
    LOGI("setBuffersGeometry=%d setBufferCount=%d", geo_ret, sbc_ret);

    if (collect_buffers(c) < 0) {
        LOGW("collect_buffers failed, retrying without setBufferCount");
        ANativeWindow_release(c->win);
        c->win = ANativeWindow_fromSurface(env, surface);
        if (!c->win) { LOGE("ANativeWindow_fromSurface failed (retry)"); return; }
        c->screen_w = ANativeWindow_getWidth(c->win);
        c->screen_h = ANativeWindow_getHeight(c->win);
        geo_ret = ANativeWindow_setBuffersGeometry(c->win, c->screen_w, c->screen_h,
                                         AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        LOGI("retry setBuffersGeometry=%d (no setBufferCount)", geo_ret);
        if (collect_buffers(c) < 0) {
            LOGE("collect_buffers failed even without setBufferCount");
            ANativeWindow_release(c->win);
            c->win = NULL;
            return;
        }
    }

    c->running = true;
    pthread_create(&c->thread, NULL, render_loop, c);

    /* Start audio play thread — chroot writes PCM data to pipe, APK plays via AAudio */
    if (!c->audio_running) {
        c->audio_running = true;
        pthread_create(&c->play_thr, NULL, audio_play_thread, c);
    }

    LOGI("started %dx%d %d bufs", c->screen_w, c->screen_h, c->buf_count);
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStop(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c) return;

    if (c->running) {
        c->running = false;
        pthread_kill(c->thread, SIGUSR1);
        pthread_join(c->thread, NULL);
    }
    if (c->audio_running) {
        c->audio_running = false;
        pthread_join(c->play_thr, NULL);
        pthread_join(c->cap_thr, NULL);
    }
    if (c->win) {
        ANativeWindow_release(c->win);
        c->win = NULL;
    }
    LOGI("stopped");
}

/* ---- JNI (audio start / stop) ---- */

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStartAudio(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c || c->audio_running) return;

    c->audio_running = true;
    pthread_create(&c->play_thr, NULL, audio_play_thread, c);
    pthread_create(&c->cap_thr, NULL, audio_cap_thread, c);
    LOGI("audio started");
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeStopAudio(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer *c = STATE(handle);
    if (!c || !c->audio_running) return;

    c->audio_running = false;
    pthread_join(c->play_thr, NULL);
    pthread_join(c->cap_thr, NULL);
    LOGI("audio stopped");
}
