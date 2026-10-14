#ifndef MODERNOS_WEB_RESOURCE_IO_H
#define MODERNOS_WEB_RESOURCE_IO_H
#include "../include/web_resource_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int web_resource_valid(const struct web_resource_packet *p) {
    uint64_t sum = (uint64_t)p->url_bytes + p->method_bytes + p->headers_bytes + p->error_bytes +
                   p->metrics_bytes;
    return p->magic == WEB_RESOURCE_MAGIC && p->version == WEB_RESOURCE_VERSION && p->id &&
           p->type >= WEB_RESOURCE_REQUEST && p->type <= WEB_SERVICE_RESPONSE &&
           p->metadata_bytes <= WEB_RESOURCE_MAX_METADATA &&
           p->body_bytes <= WEB_RESOURCE_MAX_BODY && sum == p->metadata_bytes && p->url_bytes > 0 &&
           p->url_bytes <= 4096 && p->method_bytes <= 16 && p->error_bytes <= 4096 &&
           !(p->flags & ~(WEB_RESOURCE_MAIN | WEB_RESOURCE_CREDENTIALS | WEB_RESOURCE_DOWNLOAD)) &&
           (!p->metrics_bytes || (p->type == WEB_RESOURCE_RESPONSE &&
                                  p->metrics_bytes == sizeof(struct web_network_metrics))) &&
           !(p->reserved[0] | p->reserved[1] | p->reserved[2]);
}

static inline int web_resource_send(int socket_fd, const struct web_resource_packet *p,
                                    const void *metadata, int body_fd) {
    if (!web_resource_valid(p) || (body_fd >= 0) != (p->body_bytes > 0)) {
        errno = EINVAL;
        return -1;
    }
    struct iovec vectors[2] = {{(void *)p, sizeof(*p)}, {(void *)metadata, p->metadata_bytes}};
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    memset(&control, 0, sizeof(control));
    struct msghdr message = {0};
    message.msg_iov = vectors;
    message.msg_iovlen = 2;
    if (body_fd >= 0) {
        message.msg_control = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        struct cmsghdr *c = CMSG_FIRSTHDR(&message);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &body_fd, sizeof(body_fd));
    }

    ssize_t sent;
    do {
        sent = sendmsg(socket_fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent != (ssize_t)(sizeof(*p) + p->metadata_bytes)) {
        if (sent >= 0)
            errno = EPROTO;
        return -1;
    }
    return 0;
}

/* Return 1 for a validated packet, 0 for EOF, -1 for error. Close every
 * received descriptor when validation fails, including surplus rights. */
static inline int web_resource_receive(int socket_fd, struct web_resource_packet *p, void *metadata,
                                       int *body_fd, int flags) {
    struct iovec vectors[2] = {{p, sizeof(*p)}, {metadata, WEB_RESOURCE_MAX_METADATA}};
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(8 * sizeof(int))];
    } control;
    memset(&control, 0, sizeof(control));
    struct msghdr message = {0};
    message.msg_iov = vectors;
    message.msg_iovlen = 2;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    *body_fd = -1;
    ssize_t received;
    do {
        received = recvmsg(socket_fd, &message, flags | MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    if (received <= 0)
        return (int)received;
    int rights[8], count = 0, invalid = !!(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
            c->cmsg_len < CMSG_LEN(0)) {
            invalid = 1;
            continue;
        }
        size_t bytes = c->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int))
            invalid = 1;
        for (size_t i = 0; i < bytes / sizeof(int); i++) {
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

    invalid |= received < (ssize_t)sizeof(*p) || !web_resource_valid(p) ||
               received != (ssize_t)(sizeof(*p) + p->metadata_bytes) ||
               count != (p->body_bytes ? 1 : 0);
    if (!invalid && count) {
        struct stat status;
        int seals = fcntl(rights[0], F_GET_SEALS);
        unsigned required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
        invalid = fstat(rights[0], &status) < 0 || !S_ISREG(status.st_mode) ||
                  status.st_size != (off_t)p->body_bytes || seals < 0 ||
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

static inline int web_resource_body(const void *data, size_t bytes) {
    if (!bytes)
        return -1;
    if (bytes > WEB_RESOURCE_MAX_BODY) {
        errno = EFBIG;
        return -1;
    }
    int fd = memfd_create("web-resource", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    int saved;
    const char *cursor = (const char *)data;
    size_t remaining = bytes;
    if (ftruncate(fd, (off_t)bytes) < 0)
        goto fail;
    while (remaining) {
        ssize_t n = write(fd, cursor, remaining);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto fail;
        remaining -= (size_t)n;
        cursor += n;
    }

    if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
        goto fail;
    return fd;
fail:
    saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

#endif
