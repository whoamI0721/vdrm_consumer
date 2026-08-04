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
        struct iovec iov[2];
        iov[0].iov_base = &req;
        iov[0].iov_len = sizeof(req);
        iov[1].iov_base = arg_buf;
        iov[1].iov_len = sizeof(arg_buf);
        struct msghdr msg = {0};
        msg.msg_iov = iov;
        msg.msg_iovlen = 2;
        ssize_t n = recvmsg(sock, &msg, 0);
        if (n <= 0) break;
        long ret = ioctl(fd, req, arg_buf);
        if (ret < 0) ret = -errno;
        if (write(sock, &ret, sizeof(ret)) != sizeof(ret)) break;
    }
    close(fd);
    close(sock);
    return 0;
}