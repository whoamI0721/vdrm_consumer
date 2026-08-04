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
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "anw_hidden.h"

#define TAG "VDRM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define VDR2CTL_PATH "/dev/vdr2ctl"
#define MAX_BUFS 6
#define VDR2_SLOTS 3

/* ---- Guard booth protocol (ioctl on /dev/vdr2ctl) ---- */

#define VDR2_IOC_NONE 0
#define VDR2_IOC_WRITE 1
#define VDR2_IOC_READ 2

struct vdr2_frame_io { int fd; int pad; unsigned long long seq; };
struct vdr2_ev_io    { int type, code, value, x, y; };
struct vdr2_reg_io   { int fd, slot, efd; unsigned stride; };

#define VDR2_IOC(cmd)        ((cmd) & 0xFF)
#define VDR2_IOC_BEGIN          (0x56u << 8 | 0)
#define VDR2_IOC_PRESENT        (VDR2_IOC_WRITE << 30 | 0x56u << 8 | 1 | sizeof(int) << 16)
#define VDR2_IOC_FRAME          (VDR2_IOC_READ  << 30 | 0x56u << 8 | 2 | sizeof(struct vdr2_frame_io) << 16)
#define VDR2_IOC_REGISTER_BUF   (VDR2_IOC_WRITE << 30 | 0x56u << 8 | 3 | sizeof(struct vdr2_reg_io) << 16)
#define VDR2_IOC_CLEAR_BUFS     (0x56u << 8 | 4)
#define VDR2_IOC_EV             (VDR2_IOC_WRITE << 30 | 0x56u << 8 | 5 | sizeof(struct vdr2_ev_io) << 16)
#define VDR2_IOC_AUDIO_PLAY     (VDR2_IOC_READ  << 30 | 0x56u << 8 | 6 | sizeof(int) << 16)
#define VDR2_IOC_AUDIO_CAP      (VDR2_IOC_READ  << 30 | 0x56u << 8 | 7 | sizeof(int) << 16)
#define VDR2_IOC_AUDIO_PLAY_RECV (VDR2_IOC_READ << 30 | 0x56u << 8 | 8 | sizeof(int) << 16)
#define VDR2_IOC_AUDIO_CAP_RECV  (VDR2_IOC_READ << 30 | 0x56u << 8 | 9 | sizeof(int) << 16)

/* Event types (must match KPM) */
#define VDR2_EV_KEY    1
#define VDR2_EV_BUTTON 2
#define VDR2_EV_MOTION 3
#define VDR2_EV_SCROLL 4

static int vdr2_sock = -1;
static pid_t vdr2_proxy_pid;
static int vdr2_bell = -1;

static int vdr2_open(void)
{
    if (vdr2_sock >= 0) return 0;
    LOGI("open: creating socketpair");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { LOGE("open: socketpair fail err=%d", errno); return -errno; }
    LOGI("open: sv=[%d,%d]", sv[0], sv[1]);
    pid_t pid = fork();
    if (pid < 0) { LOGE("open: fork fail err=%d", errno); close(sv[0]); close(sv[1]); return -errno; }
    if (pid == 0) {
        LOGI("open: child started");
        close(sv[0]);
        Dl_info info;
        if (dladdr((void *)vdr2_open, &info) && info.dli_fname) {
            LOGI("open: dli_fname=%s", info.dli_fname);
            char apk_path[512];
            strncpy(apk_path, info.dli_fname, sizeof(apk_path) - 1);
            apk_path[sizeof(apk_path) - 1] = 0;
            char *excl = strstr(apk_path, "!/");
            if (excl) { *excl = 0; LOGI("open: stripped apk_path=%s", apk_path); }
            char fd_str[16];
            snprintf(fd_str, sizeof(fd_str), "%d", sv[1]);
            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                "unzip -o '%s' lib/arm64-v8a/libvdr2_helper.so -d /data/local/tmp/ 2>/dev/null; "
                "cp /data/local/tmp/lib/arm64-v8a/libvdr2_helper.so /data/local/tmp/vdr2_helper 2>/dev/null; "
                "chmod 755 /data/local/tmp/vdr2_helper 2>/dev/null; "
                "rm -rf /data/local/tmp/lib 2>/dev/null; "
                "/data/local/tmp/vdr2_helper %s",
                apk_path, fd_str);
            LOGI("open: cmd=%s", cmd);
            execlp("su", "su", "-c", cmd, NULL);
            LOGE("open: execlp failed");
        }
        LOGI("open: child exiting");
        _exit(1);
    }
    close(sv[1]);
    vdr2_proxy_pid = pid;
    vdr2_sock = sv[0];
    LOGI("open: parent sock=%d pid=%d", vdr2_sock, (int)pid);
    if (vdr2_bell < 0) {
        vdr2_bell = eventfd(0, EFD_NONBLOCK);
        LOGI("open: bell=%d", vdr2_bell);
        if (vdr2_bell < 0) { LOGE("open: eventfd fail err=%d", errno); close(vdr2_sock); vdr2_sock = -1; return -errno; }
    }
    LOGI("open: done");
    return 0;
}

static int vdr2_ioctl_checked_fds(unsigned long req, void *arg, int *fds, int nfds);

static int vdr2_ioctl_checked(unsigned long req, void *arg)
{
    return vdr2_ioctl_checked_fds(req, arg, NULL, 0);
}

static int vdr2_ioctl_checked_fds(unsigned long req, void *arg, int *fds, int nfds)
{
    if (vdr2_open() < 0) return -ENODEV;
    int nr = req & 0xFF;
    char arg_buf[128] = {0};
    if (arg) memcpy(arg_buf, arg, 128);
    LOGI("ioctl: nr=%d nfds=%d arg[0]=%d", nr, nfds, arg?*(int*)arg:-1);
    struct iovec iov[2];
    iov[0].iov_base = &req;
    iov[0].iov_len = sizeof(req);
    iov[1].iov_base = arg_buf;
    iov[1].iov_len = sizeof(arg_buf);
    char cmsg_buf[CMSG_SPACE(sizeof(int) * 3)] = {0};
    struct msghdr msg = {0};
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    if (nfds > 0 && fds) {
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
        LOGI("ioctl: send fds[0]=%d nfds=%d", fds[0], nfds);
    }
    if (sendmsg(vdr2_sock, &msg, 0) < 0) { LOGE("ioctl: sendmsg fail err=%d", errno); return -errno; }
    LOGI("ioctl: sendmsg ok");
    char cmsg_rcv[CMSG_SPACE(sizeof(int) * 3)] = {0};
    struct iovec iov_rcv;
    long ret;
    iov_rcv.iov_base = &ret;
    iov_rcv.iov_len = sizeof(ret);
    struct msghdr msg_rcv = {0};
    msg_rcv.msg_iov = &iov_rcv;
    msg_rcv.msg_iovlen = 1;
    msg_rcv.msg_control = cmsg_rcv;
    msg_rcv.msg_controllen = sizeof(cmsg_rcv);
    if (recvmsg(vdr2_sock, &msg_rcv, 0) < 0) { LOGE("ioctl: recvmsg fail err=%d", errno); return -EIO; }
    LOGI("ioctl: recv ret=%ld controllen=%d", ret, (int)msg_rcv.msg_controllen);
    struct cmsghdr *c_rcv = CMSG_FIRSTHDR(&msg_rcv);
    if (c_rcv) {
        int rcv_fd = -1;
        if (c_rcv->cmsg_level == SOL_SOCKET && c_rcv->cmsg_type == SCM_RIGHTS)
            memcpy(&rcv_fd, CMSG_DATA(c_rcv), sizeof(int));
        LOGI("ioctl: cmsg level=%d type=%d len=%d fd=%d", c_rcv->cmsg_level, c_rcv->cmsg_type, (int)c_rcv->cmsg_len, rcv_fd);
        if (rcv_fd >= 0 && arg && (nr == 6 || nr == 7)) {
            memcpy(arg, &rcv_fd, sizeof(int));
            LOGI("ioctl: audio fd=%d", rcv_fd);
        }
    } else {
        LOGI("ioctl: no cmsg");
    }
    if (ret < 0) { LOGI("ioctl: fail ret=%ld", ret); return (int)ret; }
    if (arg && nr != 6 && nr != 7) memcpy(arg, arg_buf, 128);
    LOGI("ioctl: done nr=%d ret=%ld", nr, ret);
    return 0;
}

/* ---- Event helpers ---- */

static int vdr2_send_key(int code, int down)
{
    struct vdr2_ev_io e = { VDR2_EV_KEY, code, down ? 1 : 0, 0, 0 };
    return vdr2_ioctl_checked(VDR2_IOC_EV, &e);
}

static int vdr2_send_motion(int dx, int dy)
{
    struct vdr2_ev_io e = { VDR2_EV_MOTION, 0, 0, dx, dy };
    return vdr2_ioctl_checked(VDR2_IOC_EV, &e);
}

static int vdr2_send_btn(int btn, int pressed)
{
    struct vdr2_ev_io e = { VDR2_EV_BUTTON, btn, pressed ? 1 : 0, 0, 0 };
    return vdr2_ioctl_checked(VDR2_IOC_EV, &e);
}

static int vdr2_send_scroll(int axis, int val)
{
    struct vdr2_ev_io e = { VDR2_EV_SCROLL, axis, val, 0, 0 };
    return vdr2_ioctl_checked(VDR2_IOC_EV, &e);
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

static int vdrm_submit(int dmabuf_fd, int slot, unsigned stride)
{
    int r = vdr2_open();
    if (r < 0) return r;
    struct vdr2_reg_io reg = { dmabuf_fd, slot, vdr2_bell, stride };
    int fds[2] = { dmabuf_fd, vdr2_bell };
    LOGI("submit: slot=%d dmabuf_fd=%d bell=%d stride=%u", slot, dmabuf_fd, vdr2_bell, stride);
    int ret = vdr2_ioctl_checked_fds(VDR2_IOC_REGISTER_BUF, &reg, fds, 2);
    LOGI("submit: ret=%d", ret);
    return ret;
}

static int vdrm_present(void)
{
    int fds[1] = { vdr2_bell };
    LOGI("present: bell=%d", vdr2_bell);
    int ret = vdr2_ioctl_checked_fds(VDR2_IOC_PRESENT, &vdr2_bell, fds, 1);
    LOGI("present: ret=%d", ret);
    return ret;
}

/* Wait until the kernel rings the bell (a frame flip happened).
 * Blocks up to 5s; returns 0 on ring, -ETIMEDOUT otherwise. */
static int vdr2_wait_fence(void)
{
    struct pollfd pfd = { .fd = vdr2_bell, .events = POLLIN };
    int pr = poll(&pfd, 1, 5000);
    if (pr <= 0) return pr == 0 ? -ETIMEDOUT : -errno;
    unsigned long long cnt;
    if (read(vdr2_bell, &cnt, sizeof(cnt)) != sizeof(cnt) && errno != EAGAIN)
        return -errno;
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

        int ret = vdrm_submit(submit_fd, found % VDR2_SLOTS, anb->stride);
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

        int ret = vdrm_present();
        if (ret < 0) {
            if (ret == -EINTR) {
                LOGI("present interrupted by signal");
                c->running = false;
            }
            api.queueBuffer(c->win, anb, -1);
            continue;
        }

        int fwait = vdr2_wait_fence();
        if (fwait < 0) {
            LOGI("fence wait failed ret=%d", fwait);
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

    int fd = -1;
    int r = vdr2_ioctl_checked(VDR2_IOC_AUDIO_PLAY, &fd);
    LOGI("audio_play: ioctl ret=%d fd=%d", r, fd);
    if (r < 0 || fd < 0) { LOGE("audio play ioctl failed ret=%d", r); return NULL; }
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

    int fd = -1;
    int r = vdr2_ioctl_checked(VDR2_IOC_AUDIO_CAP, &fd);
    if (r < 0 || fd < 0) { LOGE("audio cap ioctl failed ret=%d", r); return NULL; }
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
    return vdr2_send_key((int)code, down ? 1 : 0);
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendMotion(JNIEnv *env, jclass clazz, jint dx, jint dy)
{
    (void)env; (void)clazz;
    return vdr2_send_motion((int)dx, (int)dy);
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendBtn(JNIEnv *env, jclass clazz, jint btn, jboolean pressed)
{
    (void)env; (void)clazz;
    return vdr2_send_btn((int)btn, pressed ? 1 : 0);
}

JNIEXPORT jint JNICALL
Java_com_vdrm_consumer_Native_nativeSendScroll(JNIEnv *env, jclass clazz, jint axis, jint val)
{
    (void)env; (void)clazz;
    return vdr2_send_scroll((int)axis, (int)val);
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
 * FD import test -> gralloc-render test.
 *
 * System EGL on this device has no EGL_EXT_image_dma_buf_import, so
 * importing an arbitrary dma-buf is impossible. Instead we verify the
 * real VDRM pipeline:
 *
 *   1. dequeue an ANativeWindow buffer (gralloc, displayable by design)
 *   2. send its dma-buf fd to the container (unix socket, SCM_RIGHTS)
 *   3. container GPU renders into that buffer
 *   4. container acks, we queueBuffer -> SurfaceFlinger shows it
 *
 * Screen tells the story:
 *   RED   = container GPU content displayed   (pipeline OK)
 *   BLACK = container render failed           (pipeline broken)
 * ==================================================================== */

#define TEST_SOCK "/data/data/com.vdrm.consumer/vdrm_fd.sock"

struct fdtest_arg {
    ANativeWindow *win;
};

static int fdtest_send_msg_fd(int sock, const char *msg, size_t len, int fd)
{
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = (void *)msg, .iov_len = len };
    struct msghdr mh = {0};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fd >= 0) {
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    return sendmsg(sock, &mh, 0);
}

static void *fdtest_thread(void *arg)
{
    struct fdtest_arg *t = arg;
    ANativeWindow *win = t->win;
    LOGI("test: gralloc-render test started");

    if (anw_api_load(&api) < 0) {
        LOGE("test: failed to load ANativeWindow hidden API");
        return NULL;
    }

    int w = ANativeWindow_getWidth(win);
    int h = ANativeWindow_getHeight(win);
    if (w <= 0) w = 1280;
    if (h <= 0) h = 720;
    ANativeWindow_setBuffersGeometry(win, w, h, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    api.setBufferCount(win, 3);
    LOGI("test: window %dx%d", w, h);

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
    LOGI("test: waiting for container on %s", TEST_SOCK);
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) { LOGE("test: accept: %s", strerror(errno)); return NULL; }
    LOGI("test: container connected");

    int frames = 0;
    for (int i = 0; i < 600; i++) {
        ANativeWindowBuffer *anb = NULL;
        int fence = -1;
        int dq_ret = api.dequeueBuffer(win, &anb, &fence);
        if (dq_ret != 0 || !anb) {
            LOGW("test: dequeue failed ret=%d err=%s", dq_ret, strerror(-dq_ret));
            usleep(16000);
            continue;
        }
        if (fence >= 0) close(fence);
        if (!anb->handle || anb->handle->numFds < 1) {
            LOGW("test: buffer has no fd");
            api.cancelBuffer(win, anb, -1);
            continue;
        }

        int fd = dup(anb->handle->data[0]);
        struct {
            char magic[4];
            int w, h, stride;
        } hdr = { {'G','F','D','1'}, anb->width, anb->height, anb->stride };
        int sr = fdtest_send_msg_fd(cfd, (const char *)&hdr, sizeof(hdr), fd);
        close(fd);
        if (sr < 0) { LOGE("test: send fd failed: %s", strerror(errno)); break; }

        char ack[8] = {0};
        ssize_t ar = read(cfd, ack, sizeof(ack));
        if (ar < 0) { LOGE("test: ack failed: %s", strerror(errno)); break; }
        if (ar == 0) { LOGE("test: container closed"); break; }

        if (frames < 5 || frames % 50 == 0)
            LOGI("test: frame %d: buf %dx%d stride=%d ack=%.4s", frames,
                 anb->width, anb->height, anb->stride, ack);
        api.queueBuffer(win, anb, -1);
        frames++;
    }

    if (cfd >= 0) close(cfd);
    close(lfd);
    unlink(TEST_SOCK);
    ANativeWindow_release(win);
    free(t);
    LOGI("test: gralloc-render test done (%d frames)", frames);
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
