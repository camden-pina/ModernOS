#include <fs/epoll.h>
#include <fs/unix_socket.h>
#include <sched.h>
#include <mm/pmm.h>
#include <tsc.h>
#include <string.h>

#define WATCH_LIMIT 128
#define EPOLL_ONESHOT (1u << 30)

struct poll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));
struct watch {
    struct file *file;
    int fd;
    struct poll_event event;
    bool disabled;
};

struct event_poll {
    struct event_poll *next;
    unsigned cursor;
    struct watch watches[WATCH_LIMIT];
};

static struct event_poll *polls;

struct event_poll *epoll_allocate(void) {
    struct event_poll *p = kmallocz(sizeof(*p));
    if (p) {
        p->next = polls;
        polls = p;
    }
    return p;
}

void epoll_forget(struct file *file) {
    for (struct event_poll *p = polls; p; p = p->next)
        for (unsigned i = 0; i < WATCH_LIMIT; i++)
            if (p->watches[i].file == file) {
                p->watches[i].file = NULL;
                file->watcher_references--;
                file->references--;
            }
}

void epoll_release(struct event_poll *p) {
    struct event_poll **at = &polls;
    while (*at && *at != p)
        at = &(*at)->next;
    if (*at)
        *at = p->next;
    for (unsigned i = 0; i < WATCH_LIMIT; i++)
        if (p->watches[i].file) {
            struct file *f = p->watches[i].file;
            f->watcher_references--;
            vfs_close(f);
        }
    kfree(p);
}

static unsigned readiness(struct watch *w) {
    if (!w->file || w->disabled)
        return 0;
    unsigned events = w->event.events & 0x201f;
    unsigned ready =
        w->file->unix_socket ? unix_poll_locked(w->file, events) : vfs_poll(w->file, events);
    return ready & (events | POLLERR | POLLHUP);
}

unsigned epoll_poll(struct file *f, unsigned events) {
    mutex_lock(&vfs_mutex);
    unsigned ready = 0;
    for (unsigned i = 0; i < WATCH_LIMIT; i++)
        if (readiness(&f->epoll->watches[i])) {
            ready = events & POLLIN;
            break;
        }
    mutex_unlock(&vfs_mutex);
    return ready;
}

long epoll_control(struct task *t, void *registers) {
    register_t *r = registers;
    if (r->rdi >= OS_FD_MAX || !t->fds[r->rdi])
        return -E_BADF;
    struct file *ep = t->fds[r->rdi];
    if (!ep->epoll)
        return -E_INVAL;
    if (r->rdx >= OS_FD_MAX || !t->fds[r->rdx])
        return -E_BADF;
    struct file *f = t->fds[r->rdx];
    if (ep == f || f->epoll)
        return -E_INVAL; /* Nested epoll not supported. */
    if (r->rsi < 1 || r->rsi > 3)
        return -E_INVAL;
    if (!f->socket && !f->unix_socket && !f->event && !f->pipe && !f->pty && !f->tty)
        return -E_PERM;

    struct poll_event event = {0};
    if (r->rsi != 2 && !user_copy_in(&t->space, &event, r->r10, sizeof(event)))
        return -E_FAULT;

    /* Level and one-shot notifications. Edge triggering requires event source
       transition hooks; unsupported flags must never pretend to work. */
    if (event.events & ~(0x201fu | EPOLL_ONESHOT))
        return -E_NOTSUP;

    mutex_lock(&vfs_mutex);
    struct watch *found = NULL, *empty = NULL;
    for (unsigned i = 0; i < WATCH_LIMIT; i++) {
        struct watch *w = &ep->epoll->watches[i];
        if (w->file == f && w->fd == (int)r->rdx)
            found = w;
        if (!w->file && !empty)
            empty = w;
    }

    long error = 0;
    if (r->rsi == 1) {
        if (found)
            error = -E_EXIST;
        else if (!empty)
            error = -E_NOSPC;
        else {
            *empty = (struct watch){.file = f, .fd = r->rdx, .event = event};
            f->references++;
            f->watcher_references++;
        }
    } else if (!found)
        error = -E_NOENT;
    else if (r->rsi == 2) {
        found->file = NULL;
        f->watcher_references--;
        vfs_close(f);
    } else {
        found->event = event;
        found->disabled = false;
    }

    mutex_unlock(&vfs_mutex);
    return error;
}

long epoll_wait_call(struct task *t, void *registers) {
    register_t *r = registers;
    if (!r->rdx || r->rdx > WATCH_LIMIT || (int)r->r10 < -1)
        return -E_INVAL;
    if (!user_access(&t->space, r->rsi, r->rdx * sizeof(struct poll_event), true))
        return -E_FAULT;
    struct file *f = task_file_pin(t, r->rdi);
    if (!f)
        return -E_BADF;
    if (!f->epoll)
        return -E_INVAL;
    uint64_t start = tsc_uptime_us();
    for (;;) {
        struct poll_event events[WATCH_LIMIT];
        unsigned count = 0;
        mutex_lock(&vfs_mutex);
        struct event_poll *p = f->epoll;
        unsigned first = p->cursor;
        for (unsigned i = 0; i < WATCH_LIMIT && count < r->rdx; i++) {
            unsigned index = (first + i) % WATCH_LIMIT;
            struct watch *w = &p->watches[index];
            unsigned ready = readiness(w);
            if (!ready)
                continue;
            events[count++] = (struct poll_event){ready, w->event.data};
            p->cursor = (index + 1) % WATCH_LIMIT;
            if (w->event.events & EPOLL_ONESHOT)
                w->disabled = true;
        }
        mutex_unlock(&vfs_mutex);
        if (count)
            return user_copy_out(&t->space, r->rsi, events, count * sizeof(*events)) ? (long)count
                                                                                     : -E_FAULT;
        if ((int)r->r10 >= 0 && tsc_uptime_us() - start >= (uint64_t)(int)r->r10 * 1000)
            return 0;
        if (signal_pending(t) || task_pending_signal(t))
            return -E_INTR;
        sched_sleep(1000);
    }
}
