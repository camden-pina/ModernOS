#include <fs/unix_socket.h>
#include <sched.h>
#include <mm/pmm.h>
#include <sandbox.h>
#include <string.h>

#define SOCKET_LIMIT 128
#define BYTE_LIMIT 65536
#define QUEUE_LIMIT 32
#define RIGHTS_LIMIT 64
#define SOCK_NONBLOCK O_NONBLOCK
#define SOCK_CLOEXEC O_CLOEXEC
#define MSG_PEEK 2
#define MSG_CTRUNC 8
#define MSG_TRUNC 32
#define MSG_DONTWAIT 64
#define MSG_EOR 128
#define MSG_WAITALL 256
#define MSG_NOSIGNAL 0x4000
#define MSG_CMSG_CLOEXEC 0x40000000

struct unix_message {
    struct unix_message *next;
    unsigned size, offset, rights_count;
    struct file *rights[RIGHTS_LIMIT];
    uint8_t bytes[];
};

struct unix_queue {
    struct unix_message *first, *last;
    unsigned bytes, count;
};

struct unix_socket {
    struct unix_pair *pair;
    unsigned side, renderer;
};

struct unix_pair {
    struct unix_socket sockets[2];
    struct unix_queue queues[2];
    bool open[2], read_shutdown[2], write_shutdown[2];
    unsigned type, creator, domain;
};

struct message_header {
    uint64_t name;
    uint32_t name_length, padding;
    uint64_t vectors, vector_count, control, control_length;
    uint32_t flags, padding2;
};

struct control_header {
    uint64_t length;
    uint32_t level, type;
};

static unsigned pair_count;

static void release_message(struct unix_message *m) {
    for (unsigned i = 0; i < m->rights_count; i++)
        vfs_close(m->rights[i]);
    kfree(m);
}

static void clear_queue(struct unix_queue *q) {
    struct unix_message *m = q->first;
    *q = (struct unix_queue){0};
    while (m) {
        struct unix_message *next = m->next;
        release_message(m);
        m = next;
    }
}

void unix_close(struct file *f) {
    struct unix_socket *s = f->unix_socket;
    struct unix_pair *p = s->pair;
    unsigned side = s->side;
    f->unix_socket = NULL;
    p->open[side] = false;
    clear_queue(&p->queues[side]);
    if (!p->open[!side]) {
        pair_count--;
        kfree(p);
    }
}

unsigned unix_domain(struct file *f) {
    return f->unix_socket->pair->domain;
}

int unix_bind_domain(struct file *f, unsigned domain) {
    if (!f->unix_socket)
        return -E_NOTSOCK;
    struct unix_pair *p = f->unix_socket->pair;
    if (p->domain && p->domain != domain)
        return -E_PERM;
    p->domain = domain;
    return 0;
}

bool unix_authorize(struct file *f, unsigned domain, unsigned renderer) {
    struct unix_socket *s = f->unix_socket;
    if (s->pair->domain != domain || (s->renderer && s->renderer != renderer))
        return false;
    s->renderer = renderer;
    return true;
}

long unix_socketpair(struct task *t, void *registers) {
    register_t *r = registers;
    if (r->rdi != 1)
        return -E_AFNOSUPPORT;
    if (r->rdx)
        return -E_PROTONOSUPPORT;

    unsigned type = r->rsi & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (type != 1 && type != 2 && type != 5)
        return -E_PROTONOSUPPORT;
    if (!user_access(&t->space, r->r10, 2 * sizeof(int), true))
        return -E_FAULT;

    mutex_lock(&t->context->syscall_lock);
    mutex_lock(&vfs_mutex);
    int fds[2] = {-1, -1};
    long error = 0;
    for (unsigned i = 0; i < OS_FD_MAX; i++)
        if (!t->fds[i]) {
            if (fds[0] < 0)
                fds[0] = i;
            else {
                fds[1] = i;
                break;
            }
        }

    struct unix_pair *p = NULL;
    if (fds[1] < 0)
        error = -E_MFILE;
    else if (pair_count == SOCKET_LIMIT)
        error = -E_NOBUFS;
    else if (!(p = kmallocz(sizeof(*p))))
        error = -E_NOMEM;

    if (!error) {
        p->type = type;
        p->creator = t->tgid;
        p->domain = t->context->web_domain;
        pair_count++;
        for (unsigned i = 0; i < 2; i++) {
            p->sockets[i] = (struct unix_socket){.pair = p, .side = i};
            p->sockets[i].renderer = t->context->web_role == OS_WEB_RENDERER ? t->tgid : 0;
            error = vfs_unix_socket(&p->sockets[i], r->rsi, &t->fds[fds[i]]);
            if (error)
                break;
            p->open[i] = true;
            t->fd_flags[fds[i]] = (r->rsi & SOCK_CLOEXEC) ? FD_CLOEXEC : 0;
        }

        if (!error && !user_copy_out(&t->space, r->r10, fds, sizeof(fds)))
            error = -E_FAULT;

        if (error) {
            /* The first close can free the pair if the second allocation failed. */
            bool any = p->open[0] || p->open[1];
            for (unsigned i = 0; i < 2; i++)
                if (t->fds[fds[i]]) {
                    vfs_close(t->fds[fds[i]]);
                    t->fds[fds[i]] = NULL;
                    t->fd_flags[fds[i]] = 0;
                }
            if (!any) {
                pair_count--;
                kfree(p);
            }
        }
    }

    mutex_unlock(&vfs_mutex);
    mutex_unlock(&t->context->syscall_lock);
    return error;
}

unsigned unix_poll_locked(struct file *f, unsigned events) {
    struct unix_socket *s = f->unix_socket;
    struct unix_pair *p = s->pair;
    unsigned side = s->side, ready = 0;
    if (p->queues[side].first || p->read_shutdown[side] || !p->open[!side] ||
        p->write_shutdown[!side])
        ready |= events & POLLIN;
    if (!p->open[!side] || p->read_shutdown[!side] || p->write_shutdown[side])
        ready |= POLLERR;
    else if (p->queues[!side].bytes < BYTE_LIMIT && p->queues[!side].count < QUEUE_LIMIT)
        ready |= events & POLLOUT;
    if (!p->open[!side] || (p->read_shutdown[side] && p->write_shutdown[!side]))
        ready |= POLLHUP;
    if (!p->open[!side] || p->write_shutdown[!side])
        ready |= events & POLLRDHUP;
    return ready;
}

unsigned unix_poll(struct file *f, unsigned events) {
    mutex_lock(&vfs_mutex);
    unsigned ready = unix_poll_locked(f, events);
    mutex_unlock(&vfs_mutex);
    return ready;
}

long unix_ioctl(struct file *f, unsigned long request, int *value) {
    if (request == 0x5421) { /* FIONBIO */
        if (*value)
            f->flags |= O_NONBLOCK;
        else
            f->flags &= ~O_NONBLOCK;
        return 0;
    }
    if (request != 0x541b)
        return -E_NOTTY;
    mutex_lock(&vfs_mutex);
    struct unix_socket *s = f->unix_socket;
    struct unix_queue *q = &s->pair->queues[s->side];
    *value = s->pair->type == 1 ? q->bytes : q->first ? q->first->size - q->first->offset : 0;
    mutex_unlock(&vfs_mutex);
    return 0;
}

/* Socket rights are real references. Reject an edge that would create a
   reference cycle, so malicious queued sockets cannot leak entire graphs. */
static bool cyclic(struct unix_pair *from, struct unix_pair *target) {
    struct unix_pair *visited[SOCKET_LIMIT];
    unsigned count = 1;
    visited[0] = from;
    for (unsigned at = 0; at < count; at++) {
        struct unix_pair *p = visited[at];
        if (p == target)
            return true;
        for (unsigned side = 0; side < 2; side++)
            for (struct unix_message *m = p->queues[side].first; m; m = m->next)
                for (unsigned i = 0; i < m->rights_count; i++)
                    if (m->rights[i]->unix_socket) {
                        struct unix_pair *next = m->rights[i]->unix_socket->pair;
                        unsigned n = 0;
                        while (n < count && visited[n] != next)
                            n++;
                        if (n == count) {
                            if (count == SOCKET_LIMIT)
                                return true;
                            visited[count++] = next;
                        }
                    }
    }
    return false;
}

static long transfer(struct task *t, register_t *r, struct file *f, bool send, unsigned vector) {
    unsigned flags = vector == 2 ? 0 : vector ? r->rdx : r->r10;
    if (flags &
        ~(MSG_PEEK | MSG_TRUNC | MSG_DONTWAIT | MSG_WAITALL | MSG_NOSIGNAL | MSG_CMSG_CLOEXEC))
        return -E_NOTSUP;
    if (send && (flags & (MSG_PEEK | MSG_TRUNC | MSG_WAITALL | MSG_CMSG_CLOEXEC)))
        return -E_INVAL;
    if (flags & MSG_WAITALL)
        return -E_NOTSUP;

    struct message_header h = {0};
    struct os_iovec v[16];
    unsigned count = 1;
    struct file *rights[RIGHTS_LIMIT];
    unsigned right_count = 0;
    size_t total = 0;

    if (vector == 1) {
        if (!user_copy_in(&t->space, &h, r->rsi, sizeof(h)))
            return -E_FAULT;
        if (h.name && send)
            return -E_ISCONN;
        if (h.vector_count > 16 ||
            h.control_length > (send ? 16 + RIGHTS_LIMIT * sizeof(int) + 8 : 16384))
            return -E_MSGSIZE;
        if (!send && (!user_access(&t->space, r->rsi, sizeof(h), true) ||
                      !user_access(&t->space, h.control, h.control_length, true)))
            return -E_FAULT;
        count = h.vector_count;
        if (!user_copy_in(&t->space, v, h.vectors, count * sizeof(*v)))
            return -E_FAULT;

        if (send && h.control_length) {
            uint8_t control[16 + RIGHTS_LIMIT * sizeof(int) + 8];
            if (!user_copy_in(&t->space, control, h.control, h.control_length))
                return -E_FAULT;
            for (size_t at = 0; at < h.control_length;) {
                if (h.control_length - at < sizeof(struct control_header))
                    return -E_INVAL;
                struct control_header c;
                memcpy(&c, control + at, sizeof(c));
                if (c.length < sizeof(c) || c.length > h.control_length - at ||
                    (c.length - sizeof(c)) % 4)
                    return -E_INVAL;
                if (c.level != 1 || c.type != 1)
                    return -E_NOTSUP;
                unsigned n = (c.length - sizeof(c)) / 4;
                if (n > RIGHTS_LIMIT - right_count)
                    return -E_MSGSIZE;
                for (unsigned i = 0; i < n; i++) {
                    int fd;
                    memcpy(&fd, control + at + sizeof(c) + i * 4, 4);
                    struct file *right = task_file_pin(t, fd);
                    if (!right)
                        return -E_BADF;
                    rights[right_count++] = right;
                }
                size_t next = align(c.length, 8UL);
                if (next > h.control_length - at) {
                    if (c.length != h.control_length - at)
                        return -E_INVAL;
                    break;
                }
                at += next;
            }
        }
    } else if (vector == 2) {
        if (r->rdx > 16)
            return -E_INVAL;
        count = r->rdx;
        if (!user_copy_in(&t->space, v, r->rsi, count * sizeof(*v)))
            return -E_FAULT;
    } else {
        if (send && r->r8)
            return -E_ISCONN;
        if (!send && r->r8 && !user_access(&t->space, r->r9, 4, true))
            return -E_FAULT;
        v[0] = (struct os_iovec){r->rsi, r->rdx};
    }

    for (unsigned i = 0; i < count; i++) {
        if (v[i].len > 16 * 1024 * 1024 - total)
            return -E_MSGSIZE;
        if (!user_access(&t->space, v[i].base, v[i].len, !send))
            return -E_FAULT;
        total += v[i].len;
    }

    struct unix_socket *s = f->unix_socket;
    struct unix_pair *p = s->pair;
    unsigned side = s->side;
    if (send && p->type != 1 && total > BYTE_LIMIT)
        return -E_MSGSIZE;
    if (send && !total && right_count)
        return -E_INVAL;
    if (!total && p->type == 1)
        return 0;

    for (;;) {
        if (signal_pending(t) || task_pending_signal(t))
            return -E_INTR;

        mutex_lock(&t->context->syscall_lock);
        mutex_lock(&vfs_mutex);
        struct unix_queue *q = &p->queues[send ? !side : side];
        long result = -E_AGAIN;
        if (send) {
            if (p->write_shutdown[side] || !p->open[!side] || p->read_shutdown[!side])
                result = -E_PIPE;
            else if (q->count < QUEUE_LIMIT &&
                     (p->type == 1 ? q->bytes < BYTE_LIMIT : total <= BYTE_LIMIT - q->bytes)) {
                unsigned length = min(total, (size_t)(BYTE_LIMIT - q->bytes));
                struct unix_message *m = kmallocz(sizeof(*m) + length);
                if (!m)
                    result = -E_NOMEM;
                else {
                    result = length;
                    m->size = length;
                    for (unsigned i = 0; i < right_count; i++)
                        if (rights[i]->epoll) {
                            result = -E_NOTSUP;
                            break;
                        } else if (rights[i]->unix_socket &&
                                   cyclic(rights[i]->unix_socket->pair, p)) {
                            result = -E_LOOP;
                            break;
                        }

                    size_t at = 0;
                    for (unsigned i = 0; i < count && at < length && result >= 0; i++) {
                        size_t n = min(v[i].len, (uint64_t)(length - at));
                        if (!user_copy_in(&t->space, m->bytes + at, v[i].base, n))
                            result = -E_FAULT;
                        at += n;
                    }

                    if (result >= 0) {
                        for (unsigned i = 0; i < right_count; i++) {
                            m->rights[i] = rights[i];
                            rights[i]->references++;
                        }
                        m->rights_count = right_count;
                        if (q->last)
                            q->last->next = m;
                        else
                            q->first = m;
                        q->last = m;
                        q->count++;
                        q->bytes += length;
                    } else
                        kfree(m);
                }
            }
        } else if (p->read_shutdown[side])
            result = 0;
        else if (q->first) {
            struct unix_message *m = q->first;
            unsigned available = m->size - m->offset, length = min(total, (size_t)available);
            uint32_t output_flags = p->type == 5 ? MSG_EOR : 0;
            if (p->type != 1 && total < available)
                output_flags |= MSG_TRUNC;

            unsigned max_rights = vector == 1 && h.control && h.control_length >= 20
                                      ? min((uint64_t)RIGHTS_LIMIT, (h.control_length - 16) / 4)
                                      : 0;
            int received[RIGHTS_LIMIT];
            unsigned installed = 0;
            for (unsigned i = 0; i < m->rights_count; i++) {
                int fd = 0;
                while (fd < OS_FD_MAX && t->fds[fd])
                    fd++;
                struct file *right = m->rights[i];
                if (installed == max_rights || fd == OS_FD_MAX ||
                    sandbox_file(right, SANDBOX_READ)) {
                    output_flags |= MSG_CTRUNC;
                    continue;
                }
                t->fds[fd] = right;
                right->references++;
                t->fd_flags[fd] = (flags & MSG_CMSG_CLOEXEC) ? FD_CLOEXEC : 0;
                received[installed++] = fd;
            }

            result = length;
            size_t at = 0;
            for (unsigned i = 0; i < count && at < length; i++) {
                size_t n = min(v[i].len, (uint64_t)(length - at));
                if (!user_copy_out(&t->space, v[i].base, m->bytes + m->offset + at, n))
                    result = -E_FAULT;
                at += n;
            }

            if (vector == 1) {
                uint8_t control[16 + RIGHTS_LIMIT * sizeof(int) + 8] = {0};
                size_t size = installed ? min(align(16 + installed * 4, 8UL), h.control_length) : 0;
                if (installed) {
                    struct control_header c = {16 + installed * 4, 1, 1};
                    memcpy(control, &c, sizeof(c));
                    memcpy(control + 16, received, installed * 4);
                    if (!user_copy_out(&t->space, h.control, control, size))
                        result = -E_FAULT;
                }
                h.name_length = 0;
                h.control_length = size;
                h.flags = output_flags;
                if (!user_copy_out(&t->space, r->rsi, &h, sizeof(h)))
                    result = -E_FAULT;
            } else if (vector == 0 && r->r8) {
                uint32_t n = 0;
                if (!user_copy_out(&t->space, r->r9, &n, 4))
                    result = -E_FAULT;
            }

            if (result < 0) {
                for (unsigned i = 0; i < installed; i++) {
                    vfs_close(t->fds[received[i]]);
                    t->fds[received[i]] = NULL;
                    t->fd_flags[received[i]] = 0;
                }
            } else if (!(flags & MSG_PEEK)) {
                for (unsigned i = 0; i < m->rights_count; i++)
                    vfs_close(m->rights[i]);
                m->rights_count = 0;
                if (p->type == 1 && length < available) {
                    m->offset += length;
                    q->bytes -= length;
                } else {
                    q->first = m->next;
                    if (!q->first)
                        q->last = NULL;
                    q->count--;
                    q->bytes -= available;
                    kfree(m);
                }
                if ((flags & MSG_TRUNC) && p->type != 1)
                    result = available;
            } else if ((flags & MSG_TRUNC) && p->type != 1)
                result = available;
        } else if (!p->open[!side] || p->write_shutdown[!side])
            result = 0;

        mutex_unlock(&vfs_mutex);
        mutex_unlock(&t->context->syscall_lock);

        if (result != -E_AGAIN || (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) {
            if (result == -E_PIPE && !(flags & MSG_NOSIGNAL))
                task_tgkill(t->tgid, t->pid, 13);
            return result;
        }

        sched_sleep(1000);
    }
}

long unix_file_syscall(struct task *t, void *registers, struct file *f) {
    register_t *r = registers;
    struct unix_socket *s = f->unix_socket;
    struct unix_pair *p = s->pair;
    switch (r->rax) {
    case SYS_sendto:
    case SYS_recvfrom:
    case SYS_sendmsg:
    case SYS_recvmsg:
        return transfer(t, r, f, r->rax == SYS_sendto || r->rax == SYS_sendmsg,
                        r->rax == SYS_sendmsg || r->rax == SYS_recvmsg);

    case SYS_readv:
    case SYS_writev:
        return transfer(t, r, f, r->rax == SYS_writev, 2);

    case SYS_shutdown:
        if (r->rsi > 2)
            return -E_INVAL;
        mutex_lock(&vfs_mutex);
        if (r->rsi != 1) {
            p->read_shutdown[s->side] = true;
            clear_queue(&p->queues[s->side]);
        }
        if (r->rsi != 0)
            p->write_shutdown[s->side] = true;
        mutex_unlock(&vfs_mutex);
        return 0;

    case SYS_getsockname:
    case SYS_getpeername: {
        uint32_t length, actual = 2;
        uint16_t family = 1;
        if (!user_copy_in(&t->space, &length, r->rdx, 4))
            return -E_FAULT;
        return user_copy_out(&t->space, r->rsi, &family, min(length, actual)) &&
                       user_copy_out(&t->space, r->rdx, &actual, 4)
                   ? 0
                   : -E_FAULT;
    }

    case SYS_getsockopt: {
        if (r->rsi != 1)
            return -E_NOPROTOOPT;
        uint32_t length;
        int value;
        if (!user_copy_in(&t->space, &length, r->r8, 4))
            return -E_FAULT;
        if (r->rdx == 3)
            value = p->type;
        else if (r->rdx == 4)
            value = 0;
        else if (r->rdx == 7 || r->rdx == 8)
            value = BYTE_LIMIT;
        else
            return -E_NOPROTOOPT;
        uint32_t actual = 4;
        return user_copy_out(&t->space, r->r10, &value, min(length, actual)) &&
                       user_copy_out(&t->space, r->r8, &actual, 4)
                   ? 0
                   : -E_FAULT;
    }

    case SYS_connect:
        return -E_ISCONN;

    default:
        return -E_NOTSUP;
    }
}
