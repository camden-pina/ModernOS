#ifndef MODERNOS_EPOLL_H
#define MODERNOS_EPOLL_H
#include <fs/vfs.h>

struct task;
struct event_poll;
struct event_poll *epoll_allocate(void);
void epoll_release(struct event_poll *); /* VFS lock held */
void epoll_forget(struct file *);        /* Last external file reference gone. */
unsigned epoll_poll(struct file *, unsigned);
long epoll_control(struct task *, void *registers);
long epoll_wait_call(struct task *, void *registers);
#endif
