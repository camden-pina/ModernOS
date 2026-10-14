#ifndef MODERNOS_PTY_H
#define MODERNOS_PTY_H
#include <fs/vfs.h>

struct pty;
struct pty *pty_allocate(void);
void pty_close(struct file *);
long pty_read(struct file *, void *, size_t);
long pty_write(struct file *, const void *, size_t);
unsigned pty_poll(struct file *, unsigned);
long pty_ioctl(struct file *, unsigned, uint64_t);
void pty_foreground(struct pty *, unsigned from, unsigned to);
#endif
