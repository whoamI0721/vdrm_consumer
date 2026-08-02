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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

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
    AAudioStream *play_stream;
    AAudioStream *cap_stream;
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

    if (!c->play_stream) {
        LOGE("audio play stream not initialized");
        close(fd);
        c->play_fd = -1;
        return NULL;
    }

    LOGI("audio play thread started");

    aaudio_format_t fmt = AAudioStream_getFormat(c->play_stream);
    int bytes_per_sample = (fmt == AAUDIO_FORMAT_PCM_FLOAT) ? 4 : 2;
    int frame_size = bytes_per_sample * AAudioStream_getChannelCount(c->play_stream);
    short buf[960 * 2];
    int write_count = 0;
    while (c->audio_running) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) continue;
        int n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            aaudio_result_t wr = AAudioStream_write(c->play_stream, buf, n / frame_size, 500000000LL);
            if (++write_count % 50 == 1)
                LOGI("audio wrote %d frames (ret=%d)", n / frame_size, wr);
        } else if (n == 0) {
            usleep(10000);
        } else if (n < 0 && errno != EINTR) {
            usleep(10000);
        }
    }

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

    if (!c->cap_stream) {
        LOGE("audio cap stream not initialized");
        close(fd);
        c->cap_fd = -1;
        return NULL;
    }

    LOGI("audio cap thread started");

    if (AAudioStream_requestStart(c->cap_stream) != AAUDIO_OK) {
        LOGE("audio cap start failed");
        close(fd);
        c->cap_fd = -1;
        return NULL;
    }

    short buf[480 * 1];
    while (c->audio_running) {
        int n = AAudioStream_read(c->cap_stream, buf, sizeof(buf) / sizeof(short), 20000000LL);
        if (n > 0) {
            write(fd, buf, n * sizeof(short));
        } else {
            usleep(10000);
        }
    }

    AAudioStream_requestStop(c->cap_stream);
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
        if (c->cap_thr) pthread_join(c->cap_thr, NULL);
    }
    if (c->play_stream) {
        AAudioStream_requestStop(c->play_stream);
        AAudioStream_close(c->play_stream);
        c->play_stream = NULL;
    }
    if (c->cap_stream) {
        AAudioStream_close(c->cap_stream);
        c->cap_stream = NULL;
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

    /* Initialize AAudio output + input streams (outside threads, so errors are visible) */
    LOGI("audio init: play_stream=%p cap_stream=%p audio_running=%d",
         (void*)c->play_stream, (void*)c->cap_stream, c->audio_running);
    if (!c->play_stream) {
        AAudioStreamBuilder *bld = NULL;
        if (AAudio_createStreamBuilder(&bld) == AAUDIO_OK && bld) {
            AAudioStreamBuilder_setDirection(bld, AAUDIO_DIRECTION_OUTPUT);
            AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I16);
            AAudioStreamBuilder_setSampleRate(bld, 48000);
            AAudioStreamBuilder_setChannelCount(bld, 2);
            AAudioStreamBuilder_setPerformanceMode(bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            AAudioStreamBuilder_setSharingMode(bld, AAUDIO_SHARING_MODE_SHARED);
            AAudioStream *s = NULL;
            aaudio_result_t r = AAudioStreamBuilder_openStream(bld, &s);
            AAudioStreamBuilder_delete(bld);
            if (r == AAUDIO_OK && s) {
                AAudioStream_requestStart(s);
                c->play_stream = s;
                LOGI("play stream initialized (%d Hz x%d)", AAudioStream_getSampleRate(s), AAudioStream_getChannelCount(s));
            } else {
                LOGE("AAudio play open failed: %s", AAudio_convertResultToText(r));
            }
        }
    }
    if (!c->cap_stream) {
        AAudioStreamBuilder *bld = NULL;
        if (AAudio_createStreamBuilder(&bld) == AAUDIO_OK && bld) {
            AAudioStreamBuilder_setDirection(bld, AAUDIO_DIRECTION_INPUT);
            AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I16);
            AAudioStreamBuilder_setSampleRate(bld, 48000);
            AAudioStreamBuilder_setChannelCount(bld, 1);
            AAudioStreamBuilder_setPerformanceMode(bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            AAudioStreamBuilder_setSharingMode(bld, AAUDIO_SHARING_MODE_SHARED);
            AAudioStream *s = NULL;
            aaudio_result_t r = AAudioStreamBuilder_openStream(bld, &s);
            AAudioStreamBuilder_delete(bld);
            if (r == AAUDIO_OK && s) {
                c->cap_stream = s;
                LOGI("cap stream initialized (%d Hz x%d)", AAudioStream_getSampleRate(s), AAudioStream_getChannelCount(s));
            } else {
                LOGE("AAudio cap open failed: %s", AAudio_convertResultToText(r));
            }
        }
    }

    /* Start audio play thread */
    if (c->play_stream && !c->audio_running) {
        c->audio_running = true;
        int ret = pthread_create(&c->play_thr, NULL, audio_play_thread, c);
        if (ret != 0) {
            LOGE("audio play thread create failed: %d", ret);
            c->audio_running = false;
        }
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
        if (c->cap_thr) pthread_join(c->cap_thr, NULL);
    }
    if (c->play_stream) {
        AAudioStream_requestStop(c->play_stream);
        AAudioStream_close(c->play_stream);
        c->play_stream = NULL;
    }
    if (c->cap_stream) {
        AAudioStream_close(c->cap_stream);
        c->cap_stream = NULL;
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
    if (!c) return;

    if (c->audio_running) {
        if (!c->cap_thr) {
            pthread_create(&c->cap_thr, NULL, audio_cap_thread, c);
            LOGI("cap thread started from nativeStartAudio");
        }
        return;
    }

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
    if (c->cap_thr) pthread_join(c->cap_thr, NULL);
    LOGI("audio stopped");
}

/* ====================================================================
 * FD import test: verify that a dma-buf rendered by the container GPU
 * can be imported and displayed by this app.
 *
 * Flow:
 *   1. EGL window surface on the app's ANativeWindow
 *   2. listen on unix socket (app-private dir, SELinux-safe)
 *   3. container connects and sends the rendered dma-buf fd (SCM_RIGHTS)
 *   4. import via EGL_EXT_image_dma_buf_import, sample as GL texture
 *   5. fullscreen quad -> eglSwapBuffers
 *
 * Screen tells the story:
 *   RED   = container GPU content displayed   (pipeline OK)
 *   GREEN = import failed / no fd (fallback)  (pipeline broken)
 * ==================================================================== */

#define TEST_SOCK "/data/data/com.vdrm.consumer/vdrm_fd.sock"
#define TEST_W 64
#define TEST_H 64
#define TEST_STRIDE (TEST_W * 4)

struct fdtest_arg {
    ANativeWindow *win;
};

static int fdtest_recv_fd(int sock, int *out_fd)
{
    char cbuf[CMSG_SPACE(sizeof(int))];
    char b = 0;
    struct iovec iov = { .iov_base = &b, .iov_len = 1 };
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    if (recvmsg(sock, &msg, 0) < 1) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) return -1;
    memcpy(out_fd, CMSG_DATA(c), sizeof(int));
    return 0;
}

static GLuint fdtest_build_program(const char *vs, const char *fs)
{
    GLint ok;
    char log[512];

    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, NULL);
    glCompileShader(v);
    glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(v, sizeof(log), NULL, log); LOGE("test: vs: %s", log); return 0; }

    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, NULL);
    glCompileShader(f);
    glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(f, sizeof(log), NULL, log); LOGE("test: fs: %s", log); return 0; }

    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { glGetProgramInfoLog(p, sizeof(log), NULL, log); LOGE("test: link: %s", log); return 0; }

    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

static void *fdtest_thread(void *arg)
{
    struct fdtest_arg *t = arg;
    ANativeWindow *win = t->win;
    LOGI("test: fd import test started");

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { LOGE("test: no display"); return NULL; }
    if (!eglInitialize(dpy, NULL, NULL)) { LOGE("test: no init"); return NULL; }

    EGLint cfg_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n < 1) { LOGE("test: no cfg"); return NULL; }

    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)win, NULL);
    if (surf == EGL_NO_SURFACE) { LOGE("test: no win surface 0x%x", eglGetError()); return NULL; }
    EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) { LOGE("test: no ctx"); return NULL; }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) { LOGE("test: no current"); return NULL; }
    LOGI("test: EGL ready");

    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    LOGI("test: EGL display ext: %s", exts ? exts : "(null)");
    int has_dmabuf = exts && strstr(exts, "EGL_EXT_image_dma_buf_import") != NULL;

    unlink(TEST_SOCK);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) { LOGE("test: socket: %s", strerror(errno)); return NULL; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, TEST_SOCK);
    chmod(TEST_SOCK, 0777);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("test: bind %s: %s", TEST_SOCK, strerror(errno));
        return NULL;
    }
    listen(lfd, 1);
    LOGI("test: waiting for container fd on %s (dmabuf import supported=%d)",
         TEST_SOCK, has_dmabuf);
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) { LOGE("test: accept: %s", strerror(errno)); return NULL; }

    int dmabuf_fd = -1;
    fdtest_recv_fd(cfd, &dmabuf_fd);
    LOGI("test: received fd=%d", dmabuf_fd);

    GLuint tex = 0;
    GLuint prog = 0;
    int imported = 0;

    if (has_dmabuf && dmabuf_fd >= 0) {
        PFNEGLCREATEIMAGEKHRPROC pCreateImage =
            (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        void (*pTarget)(GLenum, void *) =
            (void (*)(GLenum, void *))eglGetProcAddress("glEGLImageTargetTexture2DOES");

        if (pCreateImage && pTarget) {
            EGLint img_attrs[] = {
                EGL_WIDTH, TEST_W,
                EGL_HEIGHT, TEST_H,
                EGL_LINUX_DRM_FOURCC_EXT, 0x34325241, /* AR24 (little-endian: B G R A) */
                EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                EGL_DMA_BUF_PLANE0_PITCH_EXT, TEST_STRIDE,
                EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0, /* LINEAR */
                EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
                EGL_NONE
            };
            EGLImage img = pCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs);
            if (img != EGL_NO_IMAGE) {
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                pTarget(GL_TEXTURE_2D, img);
                imported = 1;
                LOGI("test: dma-buf imported as texture");
            } else {
                LOGE("test: EGLImage import failed 0x%x", eglGetError());
            }
        } else {
            LOGE("test: missing import procs create=%p target=%p", pCreateImage, pTarget);
        }
    }

    static const char vs[] =
        "attribute vec2 a; void main(){ gl_Position=vec4(a,0.,1.); }";
    if (imported) {
        const char *fs = "precision mediump float;uniform sampler2D s;"
                         "void main(){gl_FragColor=texture2D(s,gl_FragCoord.xy*vec2(0.015625,0.015625));}";
        prog = fdtest_build_program(vs, fs);
        GLint loc = glGetUniformLocation(prog, "s");
        glUseProgram(prog);
        glUniform1i(loc, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    LOGI("test: import result: %s", imported ? "IMPORTED (red expected)" : "NO IMPORT (green fallback)");

    int swap_ok = 0;
    for (int i = 0; i < 600; i++) {
        if (imported && prog) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            const GLfloat quad[] = {
                -1.0f, -1.0f,   1.0f, -1.0f,   -1.0f, 1.0f,
                 1.0f, -1.0f,   1.0f,  1.0f,   -1.0f, 1.0f,
            };
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
            glEnableVertexAttribArray(0);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        } else {
            glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        eglSwapBuffers(dpy, surf);
        if (!swap_ok) { swap_ok = 1; LOGI("test: first swap OK"); }
        usleep(16000);
    }

    if (cfd >= 0) { send(cfd, "OK", 2, 0); close(cfd); }
    close(lfd);
    unlink(TEST_SOCK);
    if (dmabuf_fd >= 0) close(dmabuf_fd);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
    eglTerminate(dpy);
    ANativeWindow_release(win);
    free(t);
    LOGI("test: fd import test done");
    return NULL;
}

JNIEXPORT void JNICALL
Java_com_vdrm_consumer_Native_nativeTestFd(JNIEnv *env, jclass clazz, jobject surface)
{
    (void)clazz;
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
    if (!win) { LOGE("test: ANativeWindow_fromSurface failed"); return; }
    struct fdtest_arg *t = malloc(sizeof(*t));
    if (!t) { ANativeWindow_release(win); return; }
    t->win = win;
    pthread_t thr;
    if (pthread_create(&thr, NULL, fdtest_thread, t) != 0) {
        LOGE("test: thread create failed");
        ANativeWindow_release(win);
        free(t);
    }
    pthread_detach(thr);
    LOGI("test: thread started");
}
