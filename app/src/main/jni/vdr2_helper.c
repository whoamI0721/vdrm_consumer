#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/ioctl.h>

#define LOG(fmt, ...) fprintf(stderr, "[vdr2p] " fmt "\n", ##__VA_ARGS__)

__attribute__((visibility("default"))) int main(int argc, char **argv)
{
    if (argc < 2) { LOG("no sock arg"); return 1; }
    int sock = atoi(argv[1]);
    LOG("start sock=%d", sock);
    int fd = open("/dev/vdr2ctl", O_RDWR);
    if (fd < 0) { LOG("open fail err=%d", errno); return 1; }
    LOG("open ok fd=%d", fd);
    for (;;) {
        unsigned long req;
        char arg_buf[128];
        memset(arg_buf, 0, sizeof(arg_buf));
        char cmsg_buf[CMSG_SPACE(sizeof(int) * 3)];
        struct iovec iov[2];
        iov[0].iov_base = &req;
        iov[0].iov_len = sizeof(req);
        iov[1].iov_base = arg_buf;
        iov[1].iov_len = sizeof(arg_buf);
        struct msghdr msg = {0};
        msg.msg_iov = iov;
        msg.msg_iovlen = 2;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);
        ssize_t n = recvmsg(sock, &msg, 0);
        if (n <= 0) { LOG("recvmsg end n=%d err=%d", (int)n, errno); break; }
        int nr = req & 0xFF;
        LOG("recv n=%d req=0x%lx nr=%d arg[0]=%d", (int)n, req, nr, *(int*)arg_buf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        if (c) {
            int nfds = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *fds = (int *)CMSG_DATA(c);
            LOG("cmsg level=%d type=%d nfds=%d fds[0]=%d", c->cmsg_level, c->cmsg_type, nfds, nfds>=1?fds[0]:-1);
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                if (nfds >= 1) {
                    memcpy(&arg_buf[0], &fds[0], sizeof(int));
                    if (nfds >= 2)
                        memcpy(&arg_buf[8], &fds[1], sizeof(int));
                }
                LOG("updated arg_buf[0]=%d [8]=%d", *(int*)&arg_buf[0], *(int*)&arg_buf[8]);
            }
        } else {
            LOG("no cmsg");
        }
        long ret = ioctl(fd, req, arg_buf);
        if (ret < 0) ret = -errno;
        LOG("ioctl ret=%ld arg_buf[0]=%d", ret, *(int*)arg_buf);
        int out_fd = -1;
        if (ret == 0 && (nr == 6 || nr == 7)) {
            memcpy(&out_fd, arg_buf, sizeof(int));
            LOG("audio out_fd=%d", out_fd);
        }
        char cmsg_out[CMSG_SPACE(sizeof(int))] = {0};
        struct iovec iov_out;
        iov_out.iov_base = &ret;
        iov_out.iov_len = sizeof(ret);
        struct msghdr msg_out = {0};
        msg_out.msg_iov = &iov_out;
        msg_out.msg_iovlen = 1;
        if (out_fd >= 0) {
            msg_out.msg_control = cmsg_out;
            msg_out.msg_controllen = sizeof(cmsg_out);
            struct cmsghdr *co = CMSG_FIRSTHDR(&msg_out);
            co->cmsg_level = SOL_SOCKET;
            co->cmsg_type = SCM_RIGHTS;
            co->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(co), &out_fd, sizeof(int));
            LOG("send result with fd");
        }
        if (sendmsg(sock, &msg_out, 0) < 0) { LOG("sendmsg fail err=%d", errno); break; }
        LOG("send result ok");
    }
    LOG("exit");
    close(fd);
    close(sock);
    return 0;
}