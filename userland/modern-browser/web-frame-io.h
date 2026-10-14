#ifndef MODERNOS_WEB_FRAME_IO_H
#define MODERNOS_WEB_FRAME_IO_H
#include "../include/web_frame_protocol.h"
#include "web-resource-io.h"

static inline int web_frame_valid(const struct web_frame_packet *p) {
    return p->magic == WEB_FRAME_MAGIC && p->version == WEB_FRAME_VERSION &&
           p->type >= WEB_FRAME_HELLO && p->type <= WEB_FRAME_TREE && p->frame &&
           p->metadata_bytes <= WEB_FRAME_MAX_METADATA && p->body_bytes <= WEB_FRAME_MAX_BODY &&
           !p->reserved[0] && !p->reserved[1];
}

static inline int web_frame_send(int channel, struct web_frame_packet packet, const void *metadata,
                                 int body_fd) {
    packet.magic = WEB_FRAME_MAGIC;
    packet.version = WEB_FRAME_VERSION;
    if (!web_frame_valid(&packet) || (body_fd >= 0) != (packet.body_bytes > 0)) {
        errno = EINVAL;
        return -1;
    }
    struct iovec vectors[2] = {{&packet, sizeof(packet)},
                               {(void *)metadata, packet.metadata_bytes}};
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};
    struct msghdr message = {0};
    message.msg_iov = vectors;
    message.msg_iovlen = 2;
    if (body_fd >= 0) {
        message.msg_control = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        struct cmsghdr *rights = CMSG_FIRSTHDR(&message);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(rights), &body_fd, sizeof(body_fd));
    }

    ssize_t sent;
    do {
        sent = sendmsg(channel, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent != (ssize_t)(sizeof(packet) + packet.metadata_bytes)) {
        if (sent >= 0)
            errno = EPROTO;
        return -1;
    }
    return 0;
}

static inline int web_frame_receive(int channel, struct web_frame_packet *packet, void *metadata,
                                    int *body_fd, int flags) {
    struct iovec vectors[2] = {{packet, sizeof(*packet)}, {metadata, WEB_FRAME_MAX_METADATA}};
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(8 * sizeof(int))];
    } control = {0};
    struct msghdr message = {0};
    message.msg_iov = vectors;
    message.msg_iovlen = 2;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    *body_fd = -1;
    ssize_t received;
    do {
        received = recvmsg(channel, &message, flags | MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    if (received <= 0)
        return received;
    int rights[8], count = 0, invalid = !!(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
            c->cmsg_len < CMSG_LEN(0)) {
            invalid = 1;
            continue;
        }
        size_t size = c->cmsg_len - CMSG_LEN(0);
        if (size % sizeof(int))
            invalid = 1;
        for (size_t i = 0; i < size / sizeof(int); i++) {
            int fd;
            memcpy(&fd, (char *)CMSG_DATA(c) + i * sizeof(fd), sizeof(fd));
            if (count < 8)
                rights[count++] = fd;
            else {
                close(fd);
                invalid = 1;
            }
        }
    }

    invalid |= received < (ssize_t)sizeof(*packet) || !web_frame_valid(packet) ||
               received != (ssize_t)(sizeof(*packet) + packet->metadata_bytes) ||
               count != (packet->body_bytes ? 1 : 0);
    if (!invalid && count) {
        struct stat status;
        int seals = fcntl(rights[0], F_GET_SEALS);
        unsigned required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
        invalid = fstat(rights[0], &status) < 0 || !S_ISREG(status.st_mode) ||
                  status.st_size != (off_t)packet->body_bytes || seals < 0 ||
                  ((unsigned)seals & required) != required;
    }

    if (invalid) {
        for (int i = 0; i < count; i++)
            close(rights[i]);
        errno = EPROTO;
        return -1;
    }

    if (count)
        *body_fd = rights[0];
    return 1;
}

#endif
