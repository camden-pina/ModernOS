#ifndef COAST_PTY_H
#define COAST_PTY_H
#include <fs/vfs.h>

struct pty;
struct task;
/* Private Linux PTYs: VFS mutex held, 1 means this path is not handled. */
int pty_private_open(struct task *, const char *, unsigned, struct file **);
bool pty_private_file(const struct task *, const struct file *);
bool pty_private_ioctl(const struct task *, unsigned fd, unsigned request);
struct pty *pty_allocate(void);
void pty_close(struct file *);
long pty_read(struct file *, void *, size_t);
long pty_write(struct file *, const void *, size_t);
unsigned pty_poll(struct file *, unsigned);
long pty_ioctl(struct file *, unsigned, uint64_t);
void pty_foreground(struct pty *, unsigned from, unsigned to);
unsigned pty_job_foreground(struct pty *);
unsigned pty_job_creator(struct pty *);
void pty_job_config(struct pty *, struct os_termios *);
void pty_job_restore(struct pty *, unsigned from, unsigned to, const struct os_termios *);
#endif
