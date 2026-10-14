#include <libc.h>

char **environ;

long syscall6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory", "cc");
    return result;
}

long read(int fd, void *p, size_t n) {
    return call3(SYS_read, fd, p, n);
}

long write(int fd, const void *p, size_t n) {
    return call3(SYS_write, fd, p, n);
}

int open(const char *path, int flags, unsigned mode) {
    return call3(SYS_open, path, flags, mode);
}

int close(int fd) {
    return call1(SYS_close, fd);
}

int ioctl(int fd, unsigned request, void *data) {
    return call3(SYS_ioctl, fd, request, data);
}

int fsync(int fd) {
    return call1(SYS_fsync, fd);
}

int openat(int fd, const char *path, int flags, unsigned mode) {
    return syscall6(SYS_openat, fd, (long)path, flags, mode, 0, 0);
}

int stat(const char *path, struct os_stat *st) {
    return call2(SYS_stat, path, st);
}

int fstat(int fd, struct os_stat *st) {
    return call2(SYS_fstat, fd, st);
}

long lseek(int fd, long offset, int whence) {
    return call3(SYS_lseek, fd, offset, whence);
}

int spawn(const char *path, char *const argv[], int background) {
    return call3(SYS_spawn, path, argv, background);
}

int waitpid(int pid, int *status, int options) {
    return syscall6(SYS_wait4, pid, (long)status, options, 0, 0, 0);
}

void exit(int status) {
    call1(SYS_exit, status);
    for (;;)
        __asm__ volatile("ud2");
}

void *memset(void *p, int c, size_t n) {
    unsigned char *b = p;
    while (n--)
        *b++ = c;
    return p;
}

void *memcpy(void *d, const void *s, size_t n) {
    void *result = d;
    size_t groups = n / 64, words = (n % 64) / 8, tail = n % 8;
    /* SSE2 is part of the x86-64 user ABI and the scheduler preserves its
       state. Copy cached window rows in 64-byte groups rather than executing
       a REP iteration for every eight bytes under TCG. All loads/stores stay
       inside the supplied range, including unaligned rows and page tails.
       Normal cached stores keep the canvas hot for the following upload. */
    if (groups)
        __asm__ volatile("1:\n\t"
                         "movdqu 0(%1),%%xmm0\n\t"
                         "movdqu 16(%1),%%xmm1\n\t"
                         "movdqu 32(%1),%%xmm2\n\t"
                         "movdqu 48(%1),%%xmm3\n\t"
                         "movdqu %%xmm0,0(%0)\n\t"
                         "movdqu %%xmm1,16(%0)\n\t"
                         "movdqu %%xmm2,32(%0)\n\t"
                         "movdqu %%xmm3,48(%0)\n\t"
                         "add $64,%0\n\tadd $64,%1\n\tdec %2\n\tjnz 1b"
                         : "+D"(d), "+S"(s), "+c"(groups)
                         :
                         : "xmm0", "xmm1", "xmm2", "xmm3", "memory", "cc");

    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(words)::"memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(tail)::"memory");
    return result;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    if ((uintptr_t)p > (uintptr_t)q && (uintptr_t)p - (uintptr_t)q < n)
        while (n) {
            n--;
            p[n] = q[n];
        }
    else
        memcpy(d, s, n);
    return d;
}

int write_all(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        long n = write(fd, p, size);
        if (n <= 0)
            return n < 0 ? n : -E_IO;
        p += n;
        size -= n;
    }

    return 0;
}

/* Drain an overlong line so its tail can never become a new command. */

int read_line(int fd, char *buffer, size_t capacity) {
    size_t used = 0;
    int overflow = 0;

    for (;;) {
        char c;
        long n = read(fd, &c, 1);
        if (n < 0)
            return n;
        if (!n) {
            if (!used)
                return 0;
            break;
        }
        if (c == '\n')
            break;
        if (used + 1 < capacity)
            buffer[used++] = c;
        else
            overflow = 1;
    }

    if (capacity)
        buffer[used] = 0;
    return overflow ? -E_2BIG : (int)used + 1;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return *x - *y;
        x++;
        y++;
    }

    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n--) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            return 0;
        a++;
        b++;
    }

    return 0;
}

char *strcpy(char *d, const char *s) {
    char *p = d;
    do {
        *p++ = *s;
    } while (*s++);
    return d;
}

long number(const char *s, unsigned base) {
    unsigned long n = 0;
    if (!s || !*s || base < 2 || base > 16)
        return -1;
    while (*s) {
        unsigned v = *s >= '0' && *s <= '9'   ? *s - '0'
                     : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10
                                              : 99;
        if (v >= base || n > ((unsigned long)INT64_MAX - v) / base)
            return -1;
        n = n * base + v;
        s++;
    }

    return n;
}

/* Small messages, including job IDs and desktop traces, must be emitted as
   one console write rather than interleaved literal/number fragments. Long
   output still streams in bounded chunks without a process-global buffer. */
struct print_buffer {
    char data[256];
    size_t used;
};

static void print_flush(struct print_buffer *out) {
    if (out->used) {
        write_all(1, out->data, out->used);
        out->used = 0;
    }
}

static void print_bytes(struct print_buffer *out, const char *text, size_t length) {
    while (length) {
        size_t count = sizeof(out->data) - out->used;
        if (count > length)
            count = length;
        memcpy(out->data + out->used, text, count);
        out->used += count;
        text += count;
        length -= count;
        if (out->used == sizeof(out->data))
            print_flush(out);
    }
}

static void digit(struct print_buffer *out, unsigned long n, unsigned base) {
    char buffer[32];
    size_t used = 0;

    do {
        buffer[used++] = "0123456789abcdef"[n % base];
        n /= base;
    } while (n);

    for (size_t i = 0; i < used / 2; i++) {
        char c = buffer[i];
        buffer[i] = buffer[used - 1 - i];
        buffer[used - 1 - i] = c;
    }

    print_bytes(out, buffer, used);
}

void printf(const char *fmt, ...) {
    struct print_buffer out = {.used = 0};
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        const char *first = fmt;
        while (*fmt && *fmt != '%')
            fmt++;
        if (fmt > first)
            print_bytes(&out, first, fmt - first);
        if (!*fmt)
            break;
        fmt++;

        int wide = 0;
        while (*fmt == 'l') {
            wide = 1;
            fmt++;
        }
        if (!*fmt)
            break;

        char code = *fmt++;
        if (code == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            print_bytes(&out, s, strlen(s));
        } else if (code == 'c') {
            char c = va_arg(ap, int);
            print_bytes(&out, &c, 1);
        } else if (code == 'd') {
            long v = wide ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0) {
                print_bytes(&out, "-", 1);
                digit(&out, 0UL - (unsigned long)v, 10);
            } else
                digit(&out, v, 10);
        } else if (code == 'u' || code == 'x' || code == 'o')
            digit(&out, wide ? va_arg(ap, unsigned long) : va_arg(ap, unsigned),
                  code == 'x'   ? 16
                  : code == 'o' ? 8
                                : 10);
        else if (code == '%')
            print_bytes(&out, "%", 1);
    }

    va_end(ap);
    print_flush(&out);
}

void puts(const char *s) {
    struct print_buffer out = {.used = 0};
    print_bytes(&out, s, strlen(s));
    print_bytes(&out, "\n", 1);
    print_flush(&out);
}

const char *error_text(long error) {
    switch (-error) {
    case E_PIPE:
        return "Pipe has no reader";
    case E_INTR:
        return "Interrupted by a signal";
    case E_SRCH:
        return "No such process";
    case E_NOENT:
        return "No such file or directory";
    case E_IO:
        return "I/O error";
    case E_NOEXEC:
        return "Unsupported or invalid ELF executable";
    case E_BADF:
        return "Bad file descriptor";
    case E_NOMEM:
        return "Out of memory";
    case E_ACCES:
        return "Permission denied";
    case E_FAULT:
        return "Invalid address";
    case E_BUSY:
        return "File or mount is busy";
    case E_EXIST:
        return "Destination already exists";
    case E_XDEV:
        return "Different filesystem";
    case E_NODEV:
        return "No compatible device or mapping backend";
    case E_NOTDIR:
        return "Not a directory";
    case E_ISDIR:
        return "Is a directory";
    case E_INVAL:
        return "Invalid argument";
    case E_NOSPC:
        return "No space left";
    case E_ROFS:
        return "Read-only filesystem";
    case E_NOTEMPTY:
        return "Directory is not empty";
    case E_NOTSUP:
        return "Operation not supported";
    case E_NAMETOOLONG:
        return "Name is too long";
    case E_PERM:
        return "Operation not permitted";
    case E_2BIG:
        return "Too many arguments or input too long";
    case E_CHILD:
        return "No matching child process";
    case E_AGAIN:
        return "Try again; no input ready";
    case E_MFILE:
        return "Too many open files";
    case E_FBIG:
        return "File or mapping is too large";
    case E_NOSYS:
        return "System call not implemented";
    case E_NOTTY:
        return "Not a terminal or unsupported ioctl";
    case E_SPIPE:
        return "Device cannot seek";
    case E_LOOP:
        return "Too many symbolic links";
    case E_TIMEDOUT:
        return "Operation timed out";
    case E_NETDOWN:
        return "Network is unavailable";
    case E_NETUNREACH:
        return "Network is unreachable";
    case E_AFNOSUPPORT:
        return "Address family is not supported";
    case E_CONNREFUSED:
        return "Connection refused";
    case E_CONNRESET:
        return "Connection was reset";
    case E_NOTCONN:
        return "Socket is not connected";
    case E_ADDRINUSE:
        return "Network address is already in use";
    case E_MSGSIZE:
        return "Network message is too large";
    case E_NOTSOCK:
        return "Descriptor is not a socket";
    case E_INPROGRESS:
        return "Connection is in progress";
    default:
        return "Operation failed";
    }
}
