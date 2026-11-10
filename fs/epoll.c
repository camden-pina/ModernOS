#include <fs/epoll.h>
#include <fs/unix_socket.h>
#include <sched.h>
#include <mm/pmm.h>
#include <tsc.h>
#include <string.h>

#define WATCH_LIMIT 128
#define EPOLL_ONESHOT (1u << 30)
#define NEST_LIMIT 4
#define GRAPH_VISIT_LIMIT 128

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

/* VFS lock held throughout topology changes and readiness walks. Limit both
   depth and repeated DAG visits, so duplicate paths cannot amplify a poll
   into an unbounded traversal. Check every root when adding an epoll edge:
   adding below an existing parent must obey the same limits. */
static bool graph_valid(struct event_poll *p, struct event_poll **path,
                        unsigned depth, unsigned *visits) {
    if(depth>NEST_LIMIT || ++*visits>GRAPH_VISIT_LIMIT)return false;
    for(unsigned i=0;i<depth;i++)if(path[i]==p)return false;
    path[depth]=p;
    for(unsigned i=0;i<WATCH_LIMIT;i++) {
        struct file *f=p->watches[i].file;
        if(f && f->epoll && !graph_valid(f->epoll,path,depth+1,visits))return false;
    }
    return true;
}

static bool topology_valid(void) {
    struct event_poll *path[NEST_LIMIT+1];
    for(struct event_poll *p=polls;p;p=p->next) {
        unsigned visits=0;
        if(!graph_valid(p,path,0,&visits))return false;
    }
    return true;
}

static unsigned readiness(struct watch *w, unsigned depth) {
    if (!w->file || w->disabled)
        return 0;
    unsigned events = w->event.events & 0x201f;
    unsigned ready=0;
    if(w->file->epoll) {
        if(depth<NEST_LIMIT)
            for(unsigned i=0;i<WATCH_LIMIT;i++)
                if(readiness(&w->file->epoll->watches[i],depth+1)) {
                    ready=events&POLLIN;break;
                }
    } else
        ready=w->file->unix_socket?unix_poll_locked(w->file,events):vfs_poll(w->file,events);
    return ready & (events | POLLERR | POLLHUP);
}

unsigned epoll_poll(struct file *f, unsigned events) {
    mutex_lock(&vfs_mutex);
    unsigned ready = 0;
    for (unsigned i = 0; i < WATCH_LIMIT; i++)
        if (readiness(&f->epoll->watches[i],0)) {
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
    if (ep == f)
        return -E_INVAL;
    if (r->rsi < 1 || r->rsi > 3)
        return -E_INVAL;
    if (!f->socket && !f->unix_socket && !f->event && !f->pipe && !f->pty && !f->tty && !f->epoll && !f->inotify)
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
            if(f->epoll && !topology_valid()) {
                *empty=(struct watch){0};error=-E_LOOP;
            } else {
                f->references++;
                f->watcher_references++;
            }
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
    /* maxevents is the caller's output capacity, not our watch capacity.
       Accept Linux-sized buffers without growing the bounded watch table or
       kernel stack. At most WATCH_LIMIT records can be produced in one call. */
    if (!r->rdx || r->rdx > 0x7fffffffULL / sizeof(struct poll_event) || (int)r->r10 < -1)
        return -E_INVAL;
    uint64_t capacity = min(r->rdx, (uint64_t)WATCH_LIMIT);
    if (!user_access(&t->space, r->rsi, capacity * sizeof(struct poll_event), true))
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
        for (unsigned i = 0; i < WATCH_LIMIT && count < capacity; i++) {
            unsigned index = (first + i) % WATCH_LIMIT;
            struct watch *w = &p->watches[index];
            unsigned ready = readiness(w,0);
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
