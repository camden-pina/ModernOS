#ifndef MODERNOS_LIBC_H
#define MODERNOS_LIBC_H
#include <stddef.h>

extern char **environ;
#include <stdint.h>
#include <stdarg.h>
#include <uapi/abi.h>

long syscall6(long n, long a, long b, long c, long d, long e, long f);
#define call0(n) syscall6(n, 0, 0, 0, 0, 0, 0)
#define call1(n, a) syscall6(n, (long)(a), 0, 0, 0, 0, 0)
#define call2(n, a, b) syscall6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define call3(n, a, b, c) syscall6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

/* Native file-manager/editor operations intentionally refuse occupied targets. */

static inline long rename_noreplace(const char *oldpath, const char *newpath) {
    long e = syscall6(SYS_renameat2, AT_FDCWD, (long)oldpath, AT_FDCWD, (long)newpath, 1, 0);
    /* Older ModernOS kernels already refuse replacement in SYS_rename. */
    return e == -E_NOSYS ? call2(SYS_rename, oldpath, newpath) : e;
}

long read(int fd, void *p, size_t n);
long write(int fd, const void *p, size_t n);
int open(const char *path, int flags, unsigned mode);
int close(int fd);
int stat(const char *path, struct os_stat *st);
int fstat(int fd, struct os_stat *st);
long lseek(int fd, long offset, int whence);
int spawn(const char *path, char *const argv[], int background);
int waitpid(int pid, int *status, int options);
void exit(int status) __attribute__((noreturn));
void *memset(void *p, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
void *memmove(void *d, const void *s, size_t n);
int write_all(int fd, const void *data, size_t size);
int read_line(int fd, char *buffer, size_t capacity);
int ioctl(int fd, unsigned request, void *data);
int fsync(int fd);
int openat(int fd, const char *path, int flags, unsigned mode);
long number(const char *s, unsigned base);
void printf(const char *fmt, ...);
void puts(const char *text);
const char *error_text(long error);
#endif
