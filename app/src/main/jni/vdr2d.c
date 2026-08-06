/*
 * vdr2d — root proxy daemon for /dev/vdr2ctl.
 *
 * The app (untrusted_app) cannot open /dev/vdr2ctl (SELinux). The app spawns
 * this daemon with `su -c "<files>/vdr2d <sockfd>"`; the daemon runs in
 * u:r:ksu:s0, holds the device fd and executes the guard-booth ioctls on
 * behalf of the app. fd arguments (eventfds, dma-bufs, audio pipes) travel
 * over SCM_RIGHTS, out-fds (FRAME / AUDIO_*) come back the same way.
 *
 * The daemon is compiled as a shared library (libvdr2d.so) purely so the
 * Gradle build packs it into the APK; the app copies it to its files dir and
 * execs it. It must not depend on any Android library — plain bionic libc
 * only.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "vdr2_proto.h"

#define VDR2CTL_PATH  "/dev/vdr2ctl"
#define VDR2_LOG_PATH "/data/data/com.vdrm.consumer/files/vdr2d.log"

#define VDR2_IOC(cmd) ((cmd) & 0xFF)

#define VDR2_NR_BEGIN          0
#define VDR2_NR_PRESENT        1
#define VDR2_NR_FRAME          2
#define VDR2_NR_REGISTER_BUF   3
#define VDR2_NR_CLEAR_BUFS     4
#define VDR2_NR_EV             5
#define VDR2_NR_AUDIO_PLAY     6
#define VDR2_NR_AUDIO_CAP      7
#define VDR2_NR_AUDIO_PLAY_RECV 8
#define VDR2_NR_AUDIO_CAP_RECV 9

struct vdr2_frame_io { int fd; int pad; unsigned long long seq; };

static int logfd = -1;
static int ctl_fd = -1;

static void log_open(void)
{
    logfd = open(VDR2_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (logfd < 0) {
        /* files dir may not exist on a fresh install; create it and retry */
        const char *slash = strrchr(VDR2_LOG_PATH, '/');
        if (slash) {
            char dir[128];
            size_t n = (size_t)(slash - VDR2_LOG_PATH);
            if (n < sizeof(dir)) {
                memcpy(dir, VDR2_LOG_PATH, n);
                dir[n] = '\0';
                (void)mkdir(dir, 0755);
                logfd = open(VDR2_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            }
        }
    }
}

static void dlog(const char *fmt, ...)
{
    if (logfd < 0) return;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "[vdr2d %d] ", getpid());
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    if (n > 0) (void)write(logfd, buf, (size_t)n);
}

static int send_rsp(int sock, long ret, const void *arg, uint32_t arglen,
                    int out_fd)
{
    struct vdr2p_msg rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.magic = VDR2P_MAGIC_RSP;
    rsp.ret = (int32_t)ret;
    rsp.arglen = arglen;
    rsp.nfds = out_fd >= 0 ? 1 : 0;

    char cbuf[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov[2];
    iov[0].iov_base = &rsp;
    iov[0].iov_len = sizeof(rsp);
    iov[1].iov_base = (void *)arg;
    iov[1].iov_len = arglen;

    struct msghdr mh = {0};
    mh.msg_iov = iov;
    mh.msg_iovlen = arglen ? 2 : 1;
    if (out_fd >= 0) {
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &out_fd, sizeof(int));
    }
    return (int)sendmsg(sock, &mh, 0);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 1;
    int sock = atoi(argv[1]);

    log_open();
    dlog("start sock=%d uid=%d\n", sock, getuid());

    ctl_fd = open(VDR2CTL_PATH, O_RDWR);
    if (ctl_fd < 0) {
        int e = errno;
        dlog("open %s failed: %d\n", VDR2CTL_PATH, e);
        send_rsp(sock, -e, NULL, 0, -1);
        close(sock);
        return 1;
    }
    dlog("open %s ok fd=%d\n", VDR2CTL_PATH, ctl_fd);

    union {
        char b[VDR2P_MAX_ARG];
        uint64_t align;
    } arg;

    for (;;) {
        struct vdr2p_msg req;
        char cbuf[CMSG_SPACE(sizeof(int) * VDR2P_MAX_FD)] = {0};
        struct iovec iov[2];
        iov[0].iov_base = &req;
        iov[0].iov_len = sizeof(req);
        iov[1].iov_base = arg.b;
        iov[1].iov_len = sizeof(arg.b);

        struct msghdr mh = {0};
        mh.msg_iov = iov;
        mh.msg_iovlen = 2;
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);

        ssize_t n = recvmsg(sock, &mh, 0);
        if (n <= 0) {
            dlog("recvmsg end n=%zd errno=%d\n", n, errno);
            break;
        }
        if (req.magic != VDR2P_MAGIC_REQ || req.arglen > VDR2P_MAX_ARG) {
            dlog("bad req magic=0x%x arglen=%u\n", req.magic, req.arglen);
            break;
        }

        int fds[VDR2P_MAX_FD] = {-1, -1};
        int nfds = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c;
             c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int *p = (int *)CMSG_DATA(c);
                int cnt = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                if (cnt > VDR2P_MAX_FD) cnt = VDR2P_MAX_FD;
                for (int i = 0; i < cnt; i++) fds[nfds++] = p[i];
            }
        }
        if (nfds != (int)req.nfds)
            dlog("fd count mismatch: header %u got %d\n", req.nfds, nfds);

        unsigned int nr = VDR2_IOC(req.cmd);
        long ret = 0;
        uint32_t outlen = 0;
        int out_fd = -1;

        switch (nr) {
        case VDR2_NR_PRESENT:
            /* arg is an int: the eventfd to ring on flips */
            memcpy(arg.b, &fds[0], sizeof(int));
            ret = ioctl(ctl_fd, (unsigned long)req.cmd, arg.b);
            break;
        case VDR2_NR_REGISTER_BUF: {
            /* struct vdr2_reg_io { int fd; int slot; int efd; unsigned stride; } */
            memcpy(arg.b, &fds[0], sizeof(int));
            memcpy(arg.b + 8, &fds[1], sizeof(int));
            ret = ioctl(ctl_fd, (unsigned long)req.cmd, arg.b);
            break;
        }
        case VDR2_NR_FRAME:
            /* out: struct vdr2_frame_io { fd, pad, seq } — fd back via SCM_RIGHTS */
            ret = ioctl(ctl_fd, (unsigned long)req.cmd, arg.b);
            if (ret == 0) {
                memcpy(&out_fd, arg.b, sizeof(int));
                outlen = sizeof(struct vdr2_frame_io);
            }
            break;
        case VDR2_NR_AUDIO_PLAY:
        case VDR2_NR_AUDIO_CAP:
            /* out: int fd of the pipe end — back via SCM_RIGHTS */
            ret = ioctl(ctl_fd, (unsigned long)req.cmd, arg.b);
            if (ret == 0) {
                memcpy(&out_fd, arg.b, sizeof(int));
                outlen = sizeof(int);
            }
            break;
        case VDR2_NR_AUDIO_PLAY_RECV:
        case VDR2_NR_AUDIO_CAP_RECV:
            ret = -EPERM; /* container-side commands, never proxied */
            break;
        case VDR2_NR_BEGIN:
        case VDR2_NR_CLEAR_BUFS:
            ret = ioctl(ctl_fd, (unsigned long)req.cmd, NULL);
            break;
        default:
            ret = ioctl(ctl_fd, (unsigned long)req.cmd, arg.b);
            break;
        }
        if (ret < 0) ret = -errno;

        dlog("cmd=0x%x nr=%u ret=%ld outlen=%u out_fd=%d\n",
             req.cmd, nr, ret, outlen, out_fd);

        /* SCM_RIGHTS installs fresh fds in this process; the ioctl has
         * consumed them (eventfd_ctx_fdget / dma_buf_get hold their own
         * refs), so drop our copies now. */
        for (int i = 0; i < nfds; i++)
            if (fds[i] >= 0) close(fds[i]);

        if (send_rsp(sock, ret, outlen ? arg.b : NULL, outlen, out_fd) < 0) {
            dlog("sendmsg failed errno=%d\n", errno);
            break;
        }
        if (out_fd >= 0) close(out_fd);
    }

    dlog("exit\n");
    if (ctl_fd >= 0) close(ctl_fd);
    close(sock);
    if (logfd >= 0) close(logfd);
    return 0;
}
