#ifndef VDR2_PROTO_H
#define VDR2_PROTO_H

#include <stdint.h>

/* Wire protocol between the app (untrusted_app) and vdr2d (u:r:ksu:s0).
 *
 * Transport: AF_UNIX SOCK_SEQPACKET socketpair (preserves message
 * boundaries, so one recvmsg == one request / one response).
 *
 * Request (app -> daemon):
 *   struct vdr2p_msg { magic, cmd, ret=0, arglen, nfds }
 *   + arglen bytes of ioctl arg data
 *   + nfds file descriptors via SCM_RIGHTS (installed in the daemon)
 *
 * Response (daemon -> app):
 *   struct vdr2p_msg { magic, cmd=0, ret, arglen, nfds }
 *   + arglen bytes of ioctl out-arg data
 *   + nfds file descriptors via SCM_RIGHTS (fd produced by the ioctl)
 */

#define VDR2P_MAGIC_REQ 0x51523256u /* 'V2RQ' */
#define VDR2P_MAGIC_RSP 0x50523256u /* 'V2RP' */
#define VDR2P_MAX_ARG   128
#define VDR2P_MAX_FD    2

struct vdr2p_msg {
    uint32_t magic;
    uint32_t cmd;
    int32_t  ret;
    uint32_t arglen;
    uint32_t nfds;
};

#endif
