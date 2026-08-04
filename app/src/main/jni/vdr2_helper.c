#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/ioctl.h>

__attribute__((visibility("default"))) int main(int argc, char **argv)
{
    if (argc < 2) return 1;
    int sock = atoi(argv[1]);
    int fd = open("/dev/vdr2ctl", O_RDWR);
    if (fd < 0) return 1;
    for (;;) {
        unsigned long req;
        char arg_buf[128];
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
        if (n <= 0) break;
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int nfds = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *fds = (int *)CMSG_DATA(c);
            if (nfds >= 1) {
                memcpy(&arg_buf[0], &fds[0], sizeof(int));
                if (nfds >= 2)
                    memcpy(&arg_buf[8], &fds[1], sizeof(int));
            }
        }
        long ret = ioctl(fd, req, arg_buf);
        if (ret < 0) ret = -errno;
        int out_fd = -1;
        int nr = req & 0xFF;
        if (ret == 0 && (nr == 6 || nr == 7)) {
            memcpy(&out_fd, arg_buf, sizeof(int));
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
        }
        if (sendmsg(sock, &msg_out, 0) < 0) break;
    }
    close(fd);
    close(sock);
    return 0;
}