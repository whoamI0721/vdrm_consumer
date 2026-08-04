#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <errno.h>

int main(int argc, char **argv)
{
    if (argc < 2) return 1;
    int sock_fd = atoi(argv[1]);
    if (sock_fd < 0) return 1;
    int fd = open("/dev/vdr2ctl", O_RDWR);
    if (fd < 0) return 1;
    struct iovec iov;
    char buf = 0;
    iov.iov_base = &buf;
    iov.iov_len = 1;
    char cmsg[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg;
    msg.msg_controllen = sizeof(cmsg);
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    int fd_copy = fd;
    memcpy(CMSG_DATA(c), &fd_copy, sizeof(int));
    if (sendmsg(sock_fd, &msg, 0) < 0) {
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}