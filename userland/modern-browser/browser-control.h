#ifndef MODERNOS_BROWSER_CONTROL_H
#define MODERNOS_BROWSER_CONTROL_H
#include "../include/browser_protocol.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>

/* A separate magic identifies input/navigation on the renderer's seqpacket
 * channel. No capabilities may accompany a control packet. */
static inline int browser_control_send(int fd, struct browser_packet packet, const char *text) {
    packet.magic = BROWSER_MAGIC;
    packet.bytes = text ? (uint32_t)strlen(text) : 0;
    if (packet.bytes > BROWSER_MAX_TEXT) {
        errno = EMSGSIZE;
        return -1;
    }
    struct iovec parts[2] = {{&packet, sizeof(packet)}, {(void *)text, packet.bytes}};
    struct msghdr message = {0};
    message.msg_iov = parts;
    message.msg_iovlen = packet.bytes ? 2 : 1;
    ssize_t n;
    do {
        n = sendmsg(fd, &message, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)(sizeof(packet) + packet.bytes)) {
        if (n >= 0)
            errno = EPROTO;
        return -1;
    }
    return 0;
}

static inline int browser_control_receive(int fd, struct browser_packet *packet,
                                          char text[BROWSER_MAX_TEXT + 1]) {
    struct iovec parts[2] = {{packet, sizeof(*packet)}, {text, BROWSER_MAX_TEXT}};
    struct msghdr message = {0};
    message.msg_iov = parts;
    message.msg_iovlen = 2;
    ssize_t n = recvmsg(fd, &message, MSG_DONTWAIT);
    if (n <= 0)
        return (int)n;
    if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC) || n < (ssize_t)sizeof(*packet) ||
        packet->magic != BROWSER_MAGIC || packet->bytes > BROWSER_MAX_TEXT ||
        n != (ssize_t)(sizeof(*packet) + packet->bytes) || memchr(text, 0, packet->bytes)) {
        errno = EPROTO;
        return -1;
    }

    text[packet->bytes] = 0;
    return 1;
}

#endif
