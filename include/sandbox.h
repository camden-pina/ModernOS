#ifndef BROWSER_SANDBOX_H
#define BROWSER_SANDBOX_H
#include <kernel.h>

struct task;
struct file;
enum { SANDBOX_META = 1, SANDBOX_READ = 2, SANDBOX_WRITE = 4, SANDBOX_CREATE = 8 };
/* Paths are normalized and checked again after every symlink expansion. */
int sandbox_path(const char *path, unsigned access);
int sandbox_file(const struct file *, unsigned access);
int sandbox_syscall(struct task *, register_t *);
int sandbox_init(struct task *, const char *home, const char *local);
int sandbox_set_local(struct task *, const char *local);
int sandbox_web_init(struct task *, unsigned role, unsigned domain, const char *origin,
                     const char *home);
bool sandbox_web_origin(const char *);
#endif
