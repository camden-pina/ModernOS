#ifndef COAST_UNIX_SOCKET_H
#define COAST_UNIX_SOCKET_H
#include <fs/vfs.h>

struct task;
struct unix_socket;
long unix_socketpair(struct task *, void *registers);
long unix_stream_socket(struct task *, void *registers);
bool unix_private_call(struct task *, const void *registers);
unsigned unix_vector_limit(struct task *, struct file *);
long unix_file_syscall(struct task *, void *registers, struct file *);
unsigned unix_poll(struct file *, unsigned events);
unsigned unix_poll_locked(struct file *, unsigned events);
void unix_close(struct file *); /* vfs_mutex held */
long unix_ioctl(struct file *, unsigned long request, int *);
unsigned unix_domain(struct file *);
int unix_bind_domain(struct file *, unsigned);
bool unix_authorize(struct file *, unsigned domain, unsigned renderer);
#endif
