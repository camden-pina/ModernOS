#ifndef MODERNOS_ABI_H
#define MODERNOS_ABI_H
#include <stdint.h>

#define OS_PATH_MAX 512
#define OS_NAME_MAX 255
#define OS_ARG_MAX 4096
#define OS_ENV_MAX 512
#define OS_ARG_STRING_MAX 32768
#define OS_EXEC_BYTES_MAX (128 * 1024)
#define OS_FD_MAX 128

/* x86-64 Linux syscall numbers; see Documentation/kernel-userland.md for the subset. */
enum {
    SYS_read = 0,
    SYS_write = 1,
    SYS_open = 2,
    SYS_close = 3,
    SYS_stat = 4,
    SYS_fstat = 5,
    SYS_lseek = 8,
    SYS_mmap = 9,
    SYS_munmap = 11,
    SYS_brk = 12,
    SYS_sched_yield = 24,
    SYS_mremap = 25,
    SYS_clone = 56,
    SYS_fork = 57,
    SYS_vfork = 58,
    SYS_nanosleep = 35,
    SYS_getitimer = 36,
    SYS_alarm = 37,
    SYS_setitimer = 38,
    SYS_getpid = 39,
    SYS_execve = 59,
    SYS_exit = 60,
    SYS_wait4 = 61,
    SYS_kill = 62,
    SYS_getcwd = 79,
    SYS_chdir = 80,
    SYS_rename = 82,
    SYS_renameat2 = 316,
    SYS_mkdir = 83,
    SYS_rmdir = 84,
    SYS_unlink = 87,
    SYS_chmod = 90,
    SYS_getppid = 110,
    SYS_sync = 162,
    SYS_getdents64 = 217,
    SYS_clock_gettime = 228,
    SYS_exit_group = 231,
    SYS_openat = 257,
    SYS_ioctl = 16,
    SYS_pread64 = 17,
    SYS_pwrite64 = 18,
    SYS_readv = 19,
    SYS_writev = 20,
    SYS_access = 21,
    SYS_dup = 32,
    SYS_dup2 = 33,
    SYS_uname = 63,
    SYS_fcntl = 72,
    SYS_fsync = 74,
    SYS_fdatasync = 75,
    SYS_fchmod = 91,
    SYS_getuid = 102,
    SYS_getgid = 104,
    SYS_geteuid = 107,
    SYS_getegid = 108,
    SYS_arch_prctl = 158,
    SYS_mount = 165,
    SYS_umount2 = 166,
    SYS_gettid = 186,
    SYS_futex = 202,
    SYS_set_tid_address = 218,
    SYS_mprotect = 10,
    SYS_mkdirat = 258,
    SYS_newfstatat = 262,
    SYS_unlinkat = 263,
    SYS_renameat = 264,
    SYS_faccessat = 269,
    SYS_dup3 = 292,
    SYS_pipe2 = 293,

    SYS_spawn = 400,
    SYS_sysinfo = 401,
    SYS_display = 402,
    SYS_input = 403,
    SYS_spawn_io = 404,
    SYS_spawnve = 405,
    SYS_pty = 406,
    SYS_spawn_browser = 407,
    SYS_browser_grant = 408,
    SYS_spawn_web = 409,
    SYS_web_identity = 410,
    SYS_memory_usage = 411,
    SYS_audio = 412,

    SYS_lstat = 6,
    SYS_poll = 7,
    SYS_rt_sigaction = 13,
    SYS_rt_sigprocmask = 14,
    SYS_rt_sigreturn = 15,
    SYS_pause = 34,
    SYS_pipe = 22,
    SYS_select = 23,
    SYS_ftruncate = 77,
    SYS_truncate = 76,
    SYS_symlink = 88,
    SYS_fchdir = 81,
    SYS_readlink = 89,
    SYS_umask = 95,
    SYS_gettimeofday = 96,
    SYS_getrlimit = 97,
    SYS_rt_sigpending = 127,
    SYS_rt_sigsuspend = 130,
    SYS_utime = 132,
    SYS_tkill = 200,
    SYS_time = 201,
    SYS_clock_getres = 229,
    SYS_tgkill = 234,
    SYS_utimes = 235,
    SYS_futimesat = 261,
    SYS_readlinkat = 267,
    SYS_symlinkat = 266,
    SYS_utimensat = 280,
    SYS_pselect6 = 270,
    SYS_ppoll = 271,
    SYS_getrandom = 318,
    SYS_getrusage = 98,
    SYS_times = 100,
    SYS_prlimit64 = 302,

    SYS_socket = 41,
    SYS_connect = 42,
    SYS_accept = 43,
    SYS_sendto = 44,
    SYS_recvfrom = 45,
    SYS_sendmsg = 46,
    SYS_recvmsg = 47,
    SYS_shutdown = 48,
    SYS_bind = 49,
    SYS_listen = 50,
    SYS_getsockname = 51,
    SYS_getpeername = 52,
    SYS_socketpair = 53,
    SYS_setsockopt = 54,
    SYS_getsockopt = 55,
    SYS_accept4 = 288,

    SYS_eventfd = 284,
    SYS_eventfd2 = 290,
    SYS_memfd_create = 319,

    SYS_epoll_create = 213,
    SYS_epoll_create1 = 291,
    SYS_epoll_ctl = 233,
    SYS_epoll_wait = 232,
    SYS_epoll_pwait = 281
};

enum {
    E_PERM = 1,
    E_NOENT = 2,
    E_IO = 5,
    E_2BIG = 7,
    E_NOEXEC = 8,
    E_BADF = 9,
    E_CHILD = 10,
    E_AGAIN = 11,
    E_NOMEM = 12,
    E_ACCES = 13,
    E_FAULT = 14,
    E_BUSY = 16,
    E_INTR = 4,
    E_SRCH = 3,
    E_EXIST = 17,
    E_XDEV = 18,
    E_NODEV = 19,
    E_NOTDIR = 20,
    E_ISDIR = 21,
    E_INVAL = 22,
    E_NOTTY = 25,
    E_SPIPE = 29,
    E_MFILE = 24,
    E_FBIG = 27,
    E_NOSPC = 28,
    E_ROFS = 30,
    E_NAMETOOLONG = 36,
    E_NOSYS = 38,
    E_NOTEMPTY = 39,
    E_LOOP = 40,
    E_PIPE = 32,
    E_NOTSUP = 95,
    E_TIMEDOUT = 110,
    E_NOTSOCK = 88,
    E_DESTADDRREQ = 89,
    E_MSGSIZE = 90,
    E_PROTOTYPE = 91,
    E_NOPROTOOPT = 92,
    E_PROTONOSUPPORT = 93,
    E_AFNOSUPPORT = 97,
    E_ADDRINUSE = 98,
    E_ADDRNOTAVAIL = 99,
    E_NETDOWN = 100,
    E_NETUNREACH = 101,
    E_CONNABORTED = 103,
    E_CONNRESET = 104,
    E_NOBUFS = 105,
    E_ISCONN = 106,
    E_NOTCONN = 107,
    E_CONNREFUSED = 111,
    E_HOSTUNREACH = 113,
    E_ALREADY = 114,
    E_INPROGRESS = 115
};

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_DIRECTORY 0200000
#define O_CLOEXEC 02000000
#define O_NOCTTY 0400
#define O_LARGEFILE 0100000
#define O_NOFOLLOW 0400000
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200
#define AT_EMPTY_PATH 0x1000
#define FD_CLOEXEC 1
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030
#define F_SETPIPE_SZ 1031
#define F_GETPIPE_SZ 1032
#define MS_RDONLY 1
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFLNK 0120000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#define S_IFSOCK 0140000

enum { OS_WEB_NONE, OS_WEB_BROKER, OS_WEB_RENDERER };
/* Explicit descriptors only. A renderer has immutable canonical origin and
   a broker domain. Ordinary fork/exec/spawn never acquire this capability. */
struct os_spawn_web {
    uint32_t role, reserved;
    int32_t ipc_fd, stdout_fd, stderr_fd, reserved2;
    uint64_t origin, home;
};

struct os_web_identity {
    uint32_t role, process, domain, reserved;
    char origin[256];
};

struct os_stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    int64_t atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec;
    int64_t reserved[3];
};

struct os_dirent {
    uint64_t ino;
    int64_t off;
    uint16_t reclen;
    uint8_t type;
    char name[];
};

struct os_timespec {
    int64_t sec, nsec;
};

struct os_timeval {
    int64_t sec, usec;
};

struct os_rusage {
    struct os_timeval user, system;
    int64_t other[14]; /* other[0] is peak RSS in KiB; untracked counters are zero. */
};

struct os_memory_usage {
    uint64_t mapped_bytes, peak_mapped_bytes, limit_bytes;
};

struct os_itimerval {
    struct os_timeval interval, value;
};

struct os_pollfd {
    int32_t fd;
    int16_t events, revents;
};

#define POLLIN 1
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define POLLNVAL 32
#define POLLRDHUP 0x2000

struct os_sigaction {
    uint64_t handler, flags, restorer, mask;
};

#define SA_SIGINFO 4
#define SA_RESTORER 0x04000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000

struct os_iovec {
    uint64_t base, len;
};

struct os_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

/* Linux x86-64 kernel termios layout (36 bytes), not libc's larger layout. */
struct os_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line, cc[19];
};

struct os_winsize {
    uint16_t rows, cols, xpixel, ypixel;
};

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCGWINSZ 0x5413
#define FIONREAD 0x541b
#define TIOCSWINSZ 0x5414
#define TCFLSH 0x540b
#define OS_PTY_GET_FOREGROUND 0x4d500001
#define OPOST 1
#define ONLCR 4
#define ISIG 1
#define ICANON 2
#define ECHO 8
#define NOFLSH 128
#define ICRNL 0x100
#define VINTR 0
#define VERASE 2
#define VEOF 4
#define VTIME 5
#define VMIN 6

enum { INFO_CPUS, INFO_PROCESSES, INFO_DISKS, INFO_MOUNTS, INFO_LSBLK, INFO_NETWORK };
/* v3 adds capability discovery and buffered presentation; v1/v2 remain valid.
   Surfaces are copied, never hardware mappings. */
#define OS_DISPLAY_VERSION 3
#define OS_DISPLAY_MAX_PIXELS (64 * 1024)
#define OS_DISPLAY_MAX_RECTS 128
#define OS_DISPLAY_LEGACY_BATCH_SIZE 528

enum {
    DISPLAY_INFO,
    DISPLAY_ACQUIRE,
    DISPLAY_PRESENT,
    DISPLAY_RELEASE,
    DISPLAY_PRESENT_BATCH,
    DISPLAY_CAPS,
    DISPLAY_PRESENT_SWAP
};

#define DISPLAY_CAP_PAGE_FLIP 1
#define DISPLAY_CAP_WRITE_COMBINING 2
#define DISPLAY_CAP_LARGE_PAGES 4
#define DISPLAY_BACKEND_LINEAR 0
#define DISPLAY_BACKEND_QEMU_VBE 1

struct os_display_caps {
    uint32_t flags, backend, buffers, reserved;
};

struct os_display_info {
    uint32_t version, width, height, format, max_pixels, reserved[3];
};

struct os_display_rect {
    uint32_t x, y, width, height;
    uint64_t pixels;
};

struct os_damage_rect {
    uint32_t x, y, width, height;
};

struct os_display_batch {
    uint64_t pixels;
    uint32_t stride, count; /* stride in pixels; at most OS_DISPLAY_MAX_RECTS */
    struct os_damage_rect rects[OS_DISPLAY_MAX_RECTS];
};

/* Spawn a background child with these inherited standard streams and cwd.
   Descriptors >=3 are not inherited. The ordinary spawn inherits standard
   streams from pipe/PTY-based shells, retaining foreground behavior for the tty. */
struct os_spawn_io {
    int32_t fd[3];
    uint32_t reserved;
    uint64_t cwd;
};

/* Apply the browser policy before the child becomes runnable. local_file is
   an optional, desktop-selected document; the child cannot enlarge the grant. */
struct os_spawn_browser {
    struct os_spawn_io io;
    uint64_t local_file;
};

enum { INPUT_KEY = 1, INPUT_POINTER = 2, INPUT_OVERFLOW = 3, INPUT_HID_KEY = 4 };
/* HID_KEY: code=USB usage, dx=ASCII, dy=modifier bits, buttons=pressed.
   Legacy KEY events from a USB keyboard have buttons=1; serial events=0. */
struct os_input_event {
    uint64_t sequence;
    uint32_t type, code;
    int32_t dx, dy, wheel;
    uint32_t buttons;
};

_Static_assert(sizeof(struct os_termios) == 36, "termios ABI");
_Static_assert(sizeof(struct os_input_event) == 32, "input ABI");
_Static_assert(sizeof(struct os_display_batch) == 2064, "display v3 batch ABI");
_Static_assert(sizeof(struct os_spawn_io) == 24, "spawn stream ABI");
_Static_assert(sizeof(struct os_spawn_browser) == 32, "browser spawn ABI");
_Static_assert(sizeof(struct os_sigaction) == 32, "signal action ABI");
_Static_assert(sizeof(struct os_pollfd) == 8, "poll descriptor ABI");
_Static_assert(sizeof(struct os_rusage) == 144, "resource usage ABI");
#endif
