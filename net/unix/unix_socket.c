#include <pidns.h>
#include <credentials.h>
#include <userns.h>
#include <fs/unix_socket.h>
#include <sched.h>
#include <mm/pmm.h>
#include <sandbox.h>
#include <string.h>
#include <printk.h>
#include <filesystem.h>
#if CONFIG_SOCKET_CREDENTIALS_VERIFY
#include <printk.h>
#include <panic.h>
static bool c3_inject_allocation_failure;
static void verify_metadata(void);
#endif

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
#define CREDENTIAL_CONTROL_SIZE 32
#define RIGHTS_CONTROL_SIZE (16 + RIGHTS_LIMIT * sizeof(int))
#if CONFIG_SOCKET_CREDENTIALS_TEST
#define CONTROL_SIZE (RIGHTS_CONTROL_SIZE + CREDENTIAL_CONTROL_SIZE)
#else
#define CONTROL_SIZE (RIGHTS_CONTROL_SIZE + 8)
#endif

struct unix_peer_credentials {
    int32_t pid;
    uint32_t uid, gid;
};
_Static_assert(sizeof(struct unix_peer_credentials) == 12, "Linux ucred ABI");

struct unix_message {
    struct unix_message *next;
    unsigned size, offset, rights_count;
#if CONFIG_SOCKET_CREDENTIALS_TEST
    /* Actual sender, never pair creator or a later PID lookup. */
    struct unix_peer_credentials sender;
#if CONFIG_PID_NAMESPACE_TEST
    void *pid_identity;
#endif
    struct user_budget *metadata_budget;
#endif
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

/* Linux socketpair credentials are captured at creation, not replaced by the
   identity of a later forked/execed holder of either endpoint. */
struct unix_pair {
    struct unix_socket sockets[2];
    struct unix_queue queues[2];
    bool open[2], read_shutdown[2], write_shutdown[2];
#if CONFIG_SOCKET_CREDENTIALS_TEST
    bool passcred[2]; /* Fits original pair alignment padding. */
#endif
    unsigned type, domain;
    struct unix_peer_credentials peer;
#if CONFIG_PID_NAMESPACE_TEST
    bool pid_reference; /* Original tail padding; optional retained trailer. */
#endif
};
#if CONFIG_SOCKET_CREDENTIALS_TEST
_Static_assert(sizeof(struct unix_message)-536 <= 32,"C3 message growth exceeds charged bound");
_Static_assert(sizeof(struct unix_pair)==112,"C3 endpoint option must fit existing pair allocation");
#endif

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

static void *pair_pid_identity(struct unix_pair *p) {
#if CONFIG_PID_NAMESPACE_TEST
    return p->pid_reference?*(void **)(p+1):NULL;
#else
    (void)p;return NULL;
#endif
}
static void private_pair_release(struct unix_pair *);
static void free_pair(struct unix_pair *p) {
    private_pair_release(p);
    pidns_peer_put(pair_pid_identity(p));kfree(p);
}
static unsigned pair_count;

static struct unix_message *message_allocate(struct task *t, unsigned bytes) {
#if CONFIG_SOCKET_CREDENTIALS_TEST
    struct user_budget *budget = t->space.vm->budget;
    /* A whole charged page conservatively covers the <=32-byte rounded
       allocation increment. Existing socket data/queue bounds are unchanged. */
    if (budget && !user_budget_charge(budget, 1))
        return NULL;
#endif
    struct unix_message *m =
#if CONFIG_SOCKET_CREDENTIALS_VERIFY
        c3_inject_allocation_failure ? NULL :
#endif
        kmallocz(sizeof(*m) + bytes);
#if CONFIG_SOCKET_CREDENTIALS_TEST
    if (!m) {
        if (budget) user_budget_uncharge(budget, 1);
        return NULL;
    }
    user_budget_get(budget);
    m->metadata_budget = budget;
    m->sender = (struct unix_peer_credentials){(int32_t)t->tgid, credentials_uid(t), credentials_gid(t)};
#if CONFIG_PID_NAMESPACE_TEST
    m->pid_identity=pidns_peer_get(t);
#endif
#else
    (void)t;
#endif
    return m;
}

static void release_message(struct unix_message *m) {
#if CONFIG_PID_NAMESPACE_TEST
    pidns_peer_put(m->pid_identity);
#endif
    for (unsigned i = 0; i < m->rights_count; i++)
        vfs_close(m->rights[i]);
#if CONFIG_SOCKET_CREDENTIALS_TEST
    if (m->metadata_budget) {
        user_budget_uncharge(m->metadata_budget, 1);
        user_budget_put(m->metadata_budget);
    }
#endif
    kfree(m);
}
#if CONFIG_SOCKET_CREDENTIALS_VERIFY
extern void sched_pid_sequence_selftest(void);
static void verify_metadata(void) {
    /* Separate verification image only; synthetic task records, no user
       fault control or changes to published IDs, credentials or queues. */
    struct task *t=kmallocz(sizeof(*t));
    struct task_context *context=kmallocz(sizeof(*context));
    kassert(t && context);
    struct user_vm vm={0};t->space.vm=&vm;t->context=context;t->user=true;t->tgid=42;
    context->uid=context->gid=1000;
    struct user_budget *budget=user_budget_create(1);kassert(budget);vm.budget=budget;
    struct unix_message *m=message_allocate(t,0);kassert(m && user_budget_pages(budget)==1);
    kassert(m->sender.pid==42 && m->sender.uid==1000 && m->sender.gid==1000);
    kassert(!message_allocate(t,0) && user_budget_pages(budget)==1);
    release_message(m);kassert(!user_budget_pages(budget));
    c3_inject_allocation_failure=true;
    kassert(!message_allocate(t,0) && !user_budget_pages(budget));
    c3_inject_allocation_failure=false;
    m=message_allocate(t,0);kassert(m && user_budget_pages(budget)==1);
    vm.budget=NULL;user_budget_put(budget);release_message(m);
    kfree(context);kfree(t);sched_pid_sequence_selftest();
    pr_info("C3_SELFTEST_PASS metadata_limit allocation_failure_refund retained_account_final_cleanup signed_PID_exhaustion\n");
}
#endif

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
    struct task *t=sched_ready()?this_cpu()->current:NULL;
    unsigned trace=credentials_zygote_record(t);
    if(trace)pr_info("G1_SOCKET_CLOSE seq=%u pid=%u tid=%u slot=%llu side=%u type=%u queued=%u peer_queued=%u peer_open=%u\n",
        trace,t->tgid,t->pid,(unsigned long long)f->node.aux[3],side,p->type,p->queues[side].count,p->queues[!side].count,p->open[!side]);
    f->unix_socket = NULL;
    p->open[side] = false;
    clear_queue(&p->queues[side]);
    if (!p->open[!side]) {
        pair_count--;
        free_pair(p);
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
#if CONFIG_SOCKET_CREDENTIALS_VERIFY
    static bool verified;
    if(!verified){verify_metadata();verified=true;}
#endif
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
    else {
        void *identity=pidns_peer_get(t);
        p=kmallocz(sizeof(*p)+(identity?sizeof(void *):0));
        if(!p){pidns_peer_put(identity);error=-E_NOMEM;}
#if CONFIG_PID_NAMESPACE_TEST
        else if(identity){p->pid_reference=true;*(void **)(p+1)=identity;}
#endif
    }

    if (!error) {
        p->type = type;
        /* Creation-time actual credentials: established root by default,
           or the explicitly enabled C1 private test principal. Never update
           this snapshot when an inherited descriptor's process later drops. */
        p->peer = (struct unix_peer_credentials){.pid = (int32_t)t->tgid, .uid = credentials_uid(t), .gid = credentials_gid(t)};
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
        if(!error) {
            unsigned trace=credentials_zygote_record(t);
            if(trace)pr_info("G1_PAIR seq=%u pid=%u tid=%u type=%u fd0=%d slot0=%llu fd1=%d slot1=%llu\n",
                trace,t->tgid,t->pid,type,fds[0],(unsigned long long)t->fds[fds[0]]->node.aux[3],
                fds[1],(unsigned long long)t->fds[fds[1]]->node.aux[3]);
        }

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
                free_pair(p);
            }
        }
    }

    mutex_unlock(&vfs_mutex);
    mutex_unlock(&t->context->syscall_lock);
    return error;
}

#include "private_stream.inc"

unsigned unix_poll_locked(struct file *f, unsigned events) {
    struct unix_socket *s = f->unix_socket;
    struct unix_pair *p = s->pair;
    struct private_stream *named=private_find(p);
    if(named && !private_owner(this_cpu()->current,named))return POLLERR;
    if(named && named->state!=3)
        return named->state==2?(named->pending_count?events&POLLIN:0):(events&POLLOUT)|POLLHUP;
    unsigned side = s->side, ready = 0;
    bool read_closed=p->read_shutdown[side] || p->write_shutdown[!side] || !p->open[!side];
    bool write_closed=p->write_shutdown[side] || p->read_shutdown[!side] || !p->open[!side];
    if (p->queues[side].first || read_closed)
        ready |= events & POLLIN;
    /* Half shutdown is not an asynchronous socket error. Readiness means
       an operation can complete now, including an immediate EOF/EPIPE.
       In particular Chromium deliberately uses a read-only broker endpoint. */
    if (write_closed || (p->queues[!side].bytes < BYTE_LIMIT && p->queues[!side].count < QUEUE_LIMIT))
        ready |= events & POLLOUT;
    if (!p->open[!side] || (read_closed && write_closed))
        ready |= POLLHUP;
    if (read_closed)
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

static long transfer_vectors(struct task *t, register_t *r, struct file *f, bool send,
                             unsigned vector,struct os_iovec *v,unsigned vector_limit) {
    mutex_lock(&vfs_mutex);
    struct private_stream *named=private_find(f->unix_socket->pair);
    int named_error=named?(!private_owner(t,named)?-E_PERM:named->state!=3?-E_NOTCONN:0):0;
    mutex_unlock(&vfs_mutex);
    if(named_error)return named_error;
    unsigned flags = vector == 2 ? 0 : vector ? r->rdx : r->r10;
    if (flags &
        ~(MSG_PEEK | MSG_TRUNC | MSG_DONTWAIT | MSG_WAITALL | MSG_NOSIGNAL | MSG_CMSG_CLOEXEC))
        return -E_NOTSUP;
    if (send && (flags & (MSG_PEEK | MSG_TRUNC | MSG_WAITALL | MSG_CMSG_CLOEXEC)))
        return -E_INVAL;
    if (flags & MSG_WAITALL)
        return -E_NOTSUP;

    struct message_header h = {0};
    unsigned count = 1;
    struct file *rights[RIGHTS_LIMIT];
    unsigned right_count = 0;
#if CONFIG_SOCKET_CREDENTIALS_TEST
    bool credentials_seen = false;
#endif
    size_t total = 0;

    if (vector == 1) {
        if (!user_copy_in(&t->space, &h, r->rsi, sizeof(h)))
            return -E_FAULT;
        if(named && named->family==2 && h.control_length)return -E_NOTSUP;
        if (h.name && send)
            return -E_ISCONN;
        if (h.vector_count > 16 ||
            h.control_length > (send ? CONTROL_SIZE : 16384))
            return -E_MSGSIZE;
        if (!send && (!user_access(&t->space, r->rsi, sizeof(h), true) ||
                      !user_access(&t->space, h.control, h.control_length, true)))
            return -E_FAULT;
        count = h.vector_count;
        if (!user_copy_in(&t->space, v, h.vectors, count * sizeof(*v)))
            return -E_FAULT;

        if (send && h.control_length) {
            uint8_t control[CONTROL_SIZE];
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
                if (c.level != 1)
                    return -E_NOTSUP;
#if CONFIG_SOCKET_CREDENTIALS_TEST
                if (c.type == 2) {
                    if (f->unix_socket->pair->type == 1)
                        return -E_NOTSUP;
                    /* No privileged impersonation extension: exact actual
                       self identity only, even for namespace capabilities. */
                    if (c.length != sizeof(c) + sizeof(struct unix_peer_credentials))
                        return -E_INVAL;
                    if (credentials_seen)
                        return -E_INVAL;
                    struct unix_peer_credentials claim;
                    memcpy(&claim, control + at + sizeof(c), sizeof(claim));
                    unsigned uid=userns_uid_view(t,credentials_uid(t));
                    unsigned gid=userns_gid_view(t,credentials_gid(t));
                    if (claim.pid != (int32_t)pidns_view(t,t->tgid) || claim.uid != uid || claim.gid != gid ||
                        (userns_enabled(t) && (uid == 65534 || gid == 65534)))
                        return -E_PERM;
                    credentials_seen = true;
                    size_t step=align(c.length,8UL);
                    if (step > h.control_length-at) {
                        if (c.length != h.control_length-at) return -E_INVAL;
                        break;
                    }
                    at += step;
                    continue;
                }
#endif
                if (c.type != 1)
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
        if (r->rdx > vector_limit)
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

    unsigned trace=credentials_zygote_record(t);
    if(trace)pr_info("G1_XFER_BEGIN seq=%u pid=%u tid=%u fd=%llu slot=%llu send=%u vector=%u flags=%x requested=%llu\n",
        trace,t->tgid,t->pid,(unsigned long long)r->rdi,(unsigned long long)f->node.aux[3],send,vector,flags,(unsigned long long)total);

    for (;;) {
        if (task_pending_signal(t))
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
                struct unix_message *m = message_allocate(t, length);
                if (!m)
                    result = -E_NOMEM;
                else {
                    result = length;
                    m->size = length;
#if CONFIG_SOCKET_CREDENTIALS_TEST
                    /* Linux records provenance when either endpoint enabled
                       PASSCRED at send time, or sender explicitly supplied it.
                       Enabling only after enqueue must not invent a sender. */
                    if(!p->passcred[side] && !p->passcred[!side] && !credentials_seen)
                        m->sender=(struct unix_peer_credentials){0,UINT32_MAX,UINT32_MAX};
#endif
                    for (unsigned i = 0; i < right_count; i++)
                        if (rights[i]->epoll) {
                            result = -E_NOTSUP;
                            break;
                        } else if (rights[i]->unix_socket && private_find(rights[i]->unix_socket->pair) &&
                                   private_find(rights[i]->unix_socket->pair)->state!=3) {
                            /* Listener -> pending endpoint edges are not part
                               of the socketpair rights graph. Keep listeners
                               and unconnected endpoints out of that graph. */
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
                        unsigned trace=credentials_zygote_record(t);
                        if(trace)pr_info("G1_MSG_TX seq=%u pid=%u tid=%u slot=%llu type=%u length=%u rights=%u queued=%u credential_source_pid=%d\n",
                            trace,t->tgid,t->pid,(unsigned long long)f->node.aux[3],p->type,length,right_count,q->count,
#if CONFIG_SOCKET_CREDENTIALS_TEST
                            m->sender.pid
#else
                            0
#endif
                        );
                    } else
                        release_message(m);
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

            bool credential = false;
#if CONFIG_SOCKET_CREDENTIALS_TEST
            credential = p->passcred[side];
            if (credential) {
                int e=sandbox_file(f,SANDBOX_META);
                if(e) {
                    mutex_unlock(&vfs_mutex);
                    mutex_unlock(&t->context->syscall_lock);
                    return e;
                }
            }
#endif
            size_t credential_space = credential ? min(h.control_length,(uint64_t)CREDENTIAL_CONTROL_SIZE) : 0;
            size_t right_space = h.control_length - credential_space;
            unsigned max_rights = vector == 1 && h.control && right_space >= 20
                                      ? min((uint64_t)RIGHTS_LIMIT, (right_space - 16) / 4)
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
                uint8_t control[CONTROL_SIZE] = {0};
                size_t size=0;
                uint64_t capacity=h.control_length;
#if CONFIG_SOCKET_CREDENTIALS_TEST
                if (credential) {
                    size_t available=h.control ? h.control_length : 0;
                    size_t payload=available>=16 ? min(available-16,(uint64_t)sizeof(m->sender)) : 0;
                    if (available>=16) {
                        struct control_header c={16+payload,1,2};
                        struct unix_peer_credentials sender=m->sender;
                        sender.uid=sender.pid?userns_uid_view(t,sender.uid):65534;
                        sender.gid=sender.pid?userns_gid_view(t,sender.gid):65534;
#if CONFIG_PID_NAMESPACE_TEST
                        sender.pid=pidns_peer_view(t,m->pid_identity,sender.pid);
#endif
                        memcpy(control,&c,sizeof(c));memcpy(control+16,&sender,payload);
                        size=min(available,align(16+payload,8UL));
                    }
                    if(payload<sizeof(m->sender))output_flags|=MSG_CTRUNC;
                }
#endif
                if (installed) {
                    struct control_header c = {16 + installed * 4, 1, 1};
                    size_t n=min(align(c.length,8UL),h.control_length-size);
                    memcpy(control+size,&c,sizeof(c));
                    memcpy(control+size+16,received,installed*4);size+=n;
                }
                if(size && !user_copy_out(&t->space,h.control,control,size))result=-E_FAULT;
                h.name_length = 0;
                h.control_length = size;
                h.flags = output_flags;
                if (!user_copy_out(&t->space, r->rsi, &h, sizeof(h)))
                    result = -E_FAULT;
                unsigned trace=credentials_zygote_record(t);
                if(trace)pr_info("G1_MSG_RX seq=%u pid=%u tid=%u slot=%llu available=%u copied=%u capacity=%llu control=%llu flags=%x rights=%u credential=%u source_pid=%d result=%ld\n",
                    trace,t->tgid,t->pid,(unsigned long long)f->node.aux[3],available,length,
                    (unsigned long long)capacity,(unsigned long long)size,output_flags,installed,credential,
#if CONFIG_SOCKET_CREDENTIALS_TEST
                    m->sender.pid,
#else
                    0,
#endif
                    result);
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
                    release_message(m);
                }
                if ((flags & MSG_TRUNC) && p->type != 1)
                    result = available;
            } else if ((flags & MSG_TRUNC) && p->type != 1)
                result = available;
        } else if (!p->open[!side] || p->write_shutdown[!side])
            result = 0;

        if(result!=-E_AGAIN) {
            unsigned trace=credentials_zygote_record(t);
            if(trace)pr_info("G1_XFER_END seq=%u pid=%u tid=%u slot=%llu result=%ld queued=%u peer_open=%u\n",
                trace,t->tgid,t->pid,(unsigned long long)f->node.aux[3],result,q->count,p->open[!side]);
        }
        mutex_unlock(&vfs_mutex);
        mutex_unlock(&t->context->syscall_lock);

        if (result != -E_AGAIN || (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) {
            if (result == -E_PIPE && !(flags & MSG_NOSIGNAL))
                task_tgkill(t->tgid, t->pid, 13);
            return result;
        }

        /* A handled signal interrupts a blocking wait, not an operation
           which completed immediately (or a nonblocking EAGAIN). In
           particular a pending SIGCHLD must not turn a ready nonblocking
           Mojo send into EINTR and permanently disable its write channel.
           The syscall return checkpoint delivers the handler normally. */
        if (signal_pending(t))
            return -E_INTR;
        sched_sleep(1000);
    }
}

static long transfer(struct task *t,register_t *r,struct file *f,bool send,unsigned vector) {
    struct os_iovec small[16];
    if(vector!=2 || r->rdx<=16)return transfer_vectors(t,r,f,send,vector,small,16);
    /* Debugger/CDP writes coalesce many fragments. Only connected streams in
       the caller's admitted app budget accept larger vectors. Snapshot all
       descriptors and validate their buffers before touching the old bounded
       queues; retain the existing16MiB aggregate and64KiB queue limits. */
    unsigned limit=unix_vector_limit(t,f);
    if(r->rdx>limit)return -E_INVAL;
    size_t bytes=r->rdx*sizeof(struct os_iovec),pages=(bytes+PAGE_SIZE-1)/PAGE_SIZE;
    struct user_budget *budget=t->space.vm->budget;
    if(!user_budget_charge(budget,pages))return -E_NOMEM;
    user_budget_get(budget);
    struct os_iovec *large=kmalloc(bytes);
    if(!large){user_budget_uncharge(budget,pages);user_budget_put(budget);return -E_NOMEM;}
    long result=transfer_vectors(t,r,f,send,vector,large,limit);
    kfree(large);user_budget_uncharge(budget,pages);user_budget_put(budget);
    return result;
}

long unix_file_syscall(struct task *t, void *registers, struct file *f) {
    register_t *r = registers;
    struct unix_socket *s = f->unix_socket;
    struct unix_pair *p = s->pair;
    mutex_lock(&vfs_mutex);
    struct private_stream *named=private_find(p);
    bool allowed=!named || private_owner(t,named);
    mutex_unlock(&vfs_mutex);
    if(!allowed)return -E_PERM;
    if(named && named->family==2 && (r->rax==SYS_getsockopt || r->rax==SYS_setsockopt))
        return private_inet_option(t,r,s,named);
    if(named && (r->rax==SYS_bind || r->rax==SYS_listen || r->rax==SYS_connect || r->rax==SYS_accept || r->rax==SYS_accept4))
        return private_control(t,r,f);
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
        if(named) {
            mutex_lock(&vfs_mutex);
            long result=r->rax==SYS_getpeername && named->state!=3?-E_NOTCONN:
                named->family==2?private_inet_name(t,r,named->ports[r->rax==SYS_getpeername?!s->side:s->side]):
                                private_name(t,r,named->names[r->rax==SYS_getpeername?!s->side:s->side]);
            mutex_unlock(&vfs_mutex);return result;
        }
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
#if CONFIG_SOCKET_CREDENTIALS_TEST
        if (r->rdx == 16) {
            int e=sandbox_file(f,SANDBOX_META);
            if(e)return e;
            if(p->type==1)return -E_NOTSUP;
            if(length>INT32_MAX)return -E_INVAL;
            if(!user_access(&t->space,r->r8,4,true) ||
               !user_access(&t->space,r->r10,min(length,4u),true))return -E_FAULT;
            mutex_lock(&vfs_mutex);value=p->passcred[s->side];mutex_unlock(&vfs_mutex);
            uint32_t actual=min(length,4u);
            return user_copy_out(&t->space,r->r10,&value,actual) &&
                   user_copy_out(&t->space,r->r8,&actual,4)?0:-E_FAULT;
        }
#endif
        if (r->rdx == 17) { /* SOL_SOCKET / SO_PEERCRED, read-only snapshot */
            int error = sandbox_file(f, SANDBOX_META);
            if (error)
                return error;
            if (length > INT32_MAX)
                return -E_INVAL;
            uint32_t actual = min(length, (uint32_t)sizeof(p->peer));
            struct unix_peer_credentials peer=p->peer;
            if(named) {
                mutex_lock(&vfs_mutex);bool connected=named->state==3;
                peer=named->peers[s->side];mutex_unlock(&vfs_mutex);
                if(!connected)return -E_NOTCONN;
            }
            peer.uid=userns_uid_view(t,peer.uid);peer.gid=userns_gid_view(t,peer.gid);
            peer.pid=pidns_peer_view(t,pair_pid_identity(p),peer.pid);
            if (!user_access(&t->space, r->r8, 4, true) ||
                (actual && !user_copy_out(&t->space, r->r10, &peer, actual)))
                return -E_FAULT;
            return user_copy_out(&t->space, r->r8, &actual, 4) ? 0 : -E_FAULT;
        }
        if(named && r->rdx==30)value=named->state==2;
        else if (r->rdx == 3)
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

#if CONFIG_SOCKET_CREDENTIALS_TEST
    case SYS_setsockopt: {
        if(r->rsi!=1 || r->rdx!=16)return -E_NOPROTOOPT;
        int e=sandbox_file(f,SANDBOX_META),value;
        if(e)return e;
        if(p->type==1)return -E_NOTSUP;
        if(r->r8<4 || r->r8>INT32_MAX)return -E_INVAL;
        if(!user_copy_in(&t->space,&value,r->r10,4))return -E_FAULT;
        mutex_lock(&vfs_mutex);p->passcred[s->side]=value!=0;mutex_unlock(&vfs_mutex);
        return 0;
    }
#endif

    default:
        return -E_NOTSUP;
    }
}
