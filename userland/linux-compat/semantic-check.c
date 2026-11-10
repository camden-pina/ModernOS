#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL linux semantics:%d %s errno=%d\n", __LINE__, #x, errno);         \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define ERROR(expr, err)                                                                           \
    do {                                                                                           \
        errno = 0;                                                                                 \
        CHECK((expr) == -1 && errno == (err));                                                     \
    } while (0)
static int host;
static char dir[128];
static int fd, other;
static atomic_int entered, done;
static volatile sig_atomic_t signals;
static void handler(int n) {
    (void)n;
    signals++;
}
static void pause_ms(unsigned n) {
    struct timespec ts = {n / 1000, (n % 1000) * 1000000};
    while (nanosleep(&ts, &ts) && errno == EINTR)
        ;
}
static void *lock_thread(void *p) {
    atomic_store(&entered, 1);
    int r = flock(other, LOCK_EX);
    if (p)
        CHECK(r == -1 && errno == EINTR);
    else
        CHECK(r == 0);
    atomic_store(&done, 1);
    return NULL;
}
static void *idle_thread(void *p) {
    (void)p;
    atomic_store(&entered, 1);
    while (!atomic_load(&done))
        pause_ms(1);
    return NULL;
}
static void *signal_thread(void *p) {
    pause_ms(20);
    CHECK(!pthread_kill(*(pthread_t *)p, SIGUSR1));
    return NULL;
}
static void links(void) {
    CHECK(!mkdir("a", 0755) && !mkdir("b", 0755));
    fd = open("a/file", O_CREAT | O_RDWR | O_TRUNC, 0640);
    CHECK(fd >= 0 && write(fd, "original", 8) == 8);
    int a = open("a", O_PATH | O_DIRECTORY), b = open("b", O_PATH | O_DIRECTORY);
    CHECK(a >= 0 && b >= 0);
    CHECK(!syscall(SYS_linkat, a, "file", b, "hard", 0));
    struct stat s, t;
    CHECK(!fstat(fd, &s) && !stat("b/hard", &t) && s.st_ino == t.st_ino && s.st_nlink == 2);
    CHECK(!unlink("a/file") && !fstat(fd, &s) && s.st_nlink == 1);
    CHECK(write(fd, "-shared", 7) == 7 && !stat("b/hard", &s) && s.st_size == 15);
    CHECK(!rename("b/hard", "a/renamed") && !unlink("a/renamed") && !fstat(fd, &s) &&
          s.st_nlink == 0);
    char bytes[16] = {0};
    CHECK(pread(fd, bytes, 15, 0) == 15 && !memcmp(bytes, "original-shared", 15));
    ERROR(syscall(SYS_linkat, fd, "", b, "resurrect", AT_EMPTY_PATH), ENOENT);
    CHECK(!close(fd));
    CHECK(!symlink("target", "a/sym") && !link("a/sym", "b/sym-hard"));
    CHECK(!lstat("a/sym", &s) && !lstat("b/sym-hard", &t) && S_ISLNK(t.st_mode) &&
          s.st_ino == t.st_ino && s.st_nlink == 2);
    CHECK(!unlink("a/sym") && !lstat("b/sym-hard", &s) && s.st_nlink == 1);
    CHECK(!unlink("b/sym-hard"));
    ERROR(link("a", "b/directory"), EPERM);
    ERROR(syscall(SYS_linkat, a, "missing", b, "bad", 0), ENOENT);
    ERROR(syscall(SYS_linkat, a, "missing", b, "bad", 1), EINVAL);
    fd = open("a/file", O_CREAT | O_RDWR, 0640);
    CHECK(fd >= 0);
    CHECK(!link("a/file", "b/hard") && !rename("a/file", "b/hard"));
    CHECK(!stat("a/file", &s) && !stat("b/hard", &t) && s.st_nlink == 2 && t.st_nlink == 2);
    CHECK(!unlink("a/file") && !unlink("b/hard") && !close(fd));
    CHECK(!close(a) && !close(b) && !rmdir("a") && !rmdir("b"));
    puts("PASS hard-link identity, directory fds, shared writes, rename aliases, symlink links, "
         "unlinked-open lifetime");
}
static void metadata(void) {
    fd = open("meta", O_CREAT | O_RDWR, 0640);
    CHECK(fd >= 0 && write(fd, "abc", 3) == 3);
    CHECK(!syscall(SYS_fchmodat, AT_FDCWD, "meta", 0600));
    struct statx x;
    memset(&x, 0xa5, sizeof(x));
    CHECK(!syscall(SYS_statx, AT_FDCWD, "meta", 0, STATX_BASIC_STATS, &x));
    CHECK((x.stx_mask & (STATX_TYPE | STATX_MODE | STATX_SIZE | STATX_NLINK | STATX_INO)) ==
          (STATX_TYPE | STATX_MODE | STATX_SIZE | STATX_NLINK | STATX_INO));
    CHECK(x.stx_size == 3 && (x.stx_mode & 0777) == 0600 && x.stx_nlink == 1 && x.stx_ino);
    struct statx y;
    CHECK(!syscall(SYS_statx, fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &y) &&
          y.stx_ino == x.stx_ino);
    ERROR(syscall(SYS_statx, AT_FDCWD, "meta", 0, 0x80000000u, &x), EINVAL);
    ERROR(syscall(SYS_statx, AT_FDCWD, "meta", 0x6000, 0, &x), EINVAL);
    ERROR(syscall(SYS_statx, AT_FDCWD, "meta", 0, 0, (void *)1), EFAULT);
    ERROR(syscall(SYS_statx, 200, "", AT_EMPTY_PATH, 0, &x), EBADF);
    ERROR(syscall(SYS_statx, AT_FDCWD, "", 0, 0, &x), ENOENT);
    int p = open("meta", O_PATH | O_CLOEXEC | O_RDWR | O_TRUNC);
    CHECK(p >= 0);
    char c;
    ERROR(read(p, &c, 1), EBADF);
    ERROR(write(p, "x", 1), EBADF);
    ERROR(lseek(p, 0, SEEK_SET), EBADF);
    ERROR(syscall(SYS_fchmod, p, 0777), EBADF);
    ERROR(fsync(p), EBADF);
    ERROR(flock(p, LOCK_EX), EBADF);
    CHECK(!syscall(SYS_fchmodat2, p, "", 0644, AT_EMPTY_PATH));
    CHECK(!syscall(SYS_faccessat2, AT_FDCWD, "meta", R_OK, AT_EACCESS));
    ERROR(syscall(SYS_faccessat2, AT_FDCWD, "meta", 8, 0), EINVAL);
    ERROR(syscall(SYS_fchmodat2, AT_FDCWD, "meta", 0600, 1), EINVAL);
    CHECK(!symlink("meta", "sym"));
    int sym = open("sym", O_PATH | O_NOFOLLOW);
    CHECK(sym >= 0);
    char target[8] = {0};
    CHECK(readlinkat(sym, "", target, sizeof(target)) == 4 && !memcmp(target, "meta", 4));
    ERROR(syscall(SYS_fchmodat2, sym, "", 0600, AT_EMPTY_PATH), EOPNOTSUPP);
    CHECK(!close(sym));
    ERROR(syscall(SYS_readlinkat, p, "", target, sizeof(target)), ENOENT);

    ERROR(syscall(SYS_fchmodat2, AT_FDCWD, "sym", 0600, AT_SYMLINK_NOFOLLOW), EOPNOTSUPP);
    int d = dup(p);
    CHECK(d >= 0 && (fcntl(d, F_GETFL) & O_PATH) && !close(d));
    struct stat st;
    CHECK(!fstat(fd, &st) && st.st_size == 3 && (st.st_mode & 0777) == 0644);
    struct statfs fs;
    CHECK(!fstatfs(p, &fs) && fs.f_bsize > 0 && fs.f_namelen == 255);
    if (!host)
        CHECK((uint64_t)fs.f_type == 0x858458f6 && fs.f_blocks == 0 && fs.f_bfree == 0);
    CHECK(!statfs(".", &fs));
    ERROR(syscall(SYS_statfs, ".", (void *)1), EFAULT);
    ERROR(fstatfs(200, &fs), EBADF);
    CHECK(!close(p) && !close(fd) && !unlink("meta") && !unlink("sym"));
    puts("PASS statx layout/masks, permissions, O_PATH no-data semantics, empty paths, statfs and "
         "invalid pointers/flags");
}
static void locks(void) {
    fd = open("lock", O_CREAT | O_RDWR, 0600);
    other = open("lock", O_RDONLY);
    CHECK(fd >= 0 && other >= 0);
    CHECK(!flock(fd, LOCK_SH) && !flock(other, LOCK_SH | LOCK_NB));
    ERROR(flock(other, LOCK_EX | LOCK_NB), EWOULDBLOCK);
    CHECK(!flock(fd, LOCK_UN) && !flock(other, LOCK_EX | LOCK_NB));
    ERROR(flock(fd, LOCK_SH | LOCK_NB), EWOULDBLOCK);
    CHECK(!flock(other, LOCK_UN) && !flock(fd, LOCK_EX));
    int d = dup(fd);
    CHECK(d >= 0 && !flock(d, LOCK_EX | LOCK_NB) && !close(fd));
    fd = d;
    ERROR(flock(other, LOCK_EX | LOCK_NB), EWOULDBLOCK);
    int pid = fork(), status;
    CHECK(pid >= 0);
    if (!pid) {
        CHECK(!flock(fd, LOCK_UN));
        _exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(!flock(other, LOCK_EX | LOCK_NB) && !flock(other, LOCK_UN));
    CHECK(!flock(fd, LOCK_EX));
    pthread_t thread;
    atomic_store(&entered, 0);
    atomic_store(&done, 0);
    CHECK(!pthread_create(&thread, NULL, lock_thread, NULL));
    while (!atomic_load(&entered))
        pause_ms(1);
    pause_ms(20);
    CHECK(!atomic_load(&done));
    CHECK(!flock(fd, LOCK_UN));
    CHECK(!pthread_join(thread, NULL) && atomic_load(&done));
    CHECK(!flock(other, LOCK_UN));
    CHECK(!flock(fd, LOCK_EX));
    atomic_store(&entered, 0);
    atomic_store(&done, 0);
    CHECK(!pthread_create(&thread, NULL, lock_thread, (void *)1));
    while (!atomic_load(&entered))
        pause_ms(1);
    pause_ms(20);
    CHECK(!pthread_kill(thread, SIGUSR1));
    CHECK(!pthread_join(thread, NULL) && atomic_load(&done));
    CHECK(!close(fd) && !flock(other, LOCK_EX | LOCK_NB) && !close(other));
    ERROR(flock(200, LOCK_EX), EBADF);
    fd = open("lock", O_RDONLY);
    CHECK(fd >= 0);
    ERROR(flock(fd, LOCK_EX | LOCK_SH), EINVAL);
    CHECK(!close(fd) && !unlink("lock"));
    puts("PASS shared/exclusive/NB/conversion locks, dup/fork ownership, last-close release, "
         "blocking sibling wake and signal interruption");
}
static void ownership(void) {
    uid_t uid = host ? getuid() : 1000;
    gid_t gid = host ? getgid() : 1000;
    fd = open("owner", O_CREAT | O_RDWR, 0600);
    CHECK(fd >= 0 && !fchown(fd, uid, gid));
    struct stat s;
    CHECK(!fstat(fd, &s) && s.st_uid == uid && s.st_gid == gid);
    CHECK(!link("owner", "owner-hard") && !syscall(SYS_chown, "owner-hard", (uid_t)-1, gid));
    CHECK(!symlink("owner", "owner-sym") && !syscall(SYS_lchown, "owner-sym", uid, gid));
    CHECK(!lstat("owner-sym", &s) && S_ISLNK(s.st_mode) && s.st_uid == uid && s.st_gid == gid);
    int p = open("owner", O_PATH);
    CHECK(p >= 0);
    CHECK(!syscall(SYS_fchownat, p, "", uid, gid, AT_EMPTY_PATH));
    ERROR(syscall(SYS_fchown, p, uid, gid), EBADF);
    ERROR(syscall(SYS_fchownat, AT_FDCWD, "owner", uid, gid, 1), EINVAL);
    ERROR(syscall(SYS_chown, (void *)1, uid, gid), EFAULT);
    CHECK(!unlink("owner") && !unlink("owner-hard") && !fstat(fd, &s) && s.st_uid == uid &&
          s.st_nlink == 0);
    CHECK(!close(p) && !close(fd) && !unlink("owner-sym"));
    puts("PASS stored uid/gid across hard links, symlink/no-follow, descriptor empty paths and "
         "unlinked-open ownership");
}
static void clock_and_close(void) {
    struct timespec now, ts = {0, 1000000}, remain = {123, 456};
    CHECK(!syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &ts, &remain));
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &now));
    now.tv_nsec += 2000000;
    if (now.tv_nsec >= 1000000000) {
        now.tv_sec++;
        now.tv_nsec -= 1000000000;
    }
    CHECK(!syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME, &now, (void *)1));
    ERROR(syscall(SYS_clock_nanosleep, 99, 0, &ts, NULL), EINVAL);
    CHECK(!syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 2, &ts, NULL));
    ERROR(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, (void *)1, NULL), EFAULT);
    ts.tv_nsec = 1000000000;
    ERROR(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &ts, NULL), EINVAL);
    ts = (struct timespec){1, 0};
    pthread_t current = pthread_self(), helper;
    CHECK(!pthread_create(&helper, NULL, signal_thread, &current));
    ERROR(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &ts, &remain), EINTR);
    CHECK(remain.tv_sec >= 0 && remain.tv_sec <= 1 && remain.tv_nsec >= 0 &&
          remain.tv_nsec < 1000000000);
    CHECK(!pthread_join(helper, NULL));
    fd = open("range", O_CREAT | O_RDWR, 0600);
    CHECK(fd >= 0);
    int a = fcntl(fd, F_DUPFD, 40), b = fcntl(fd, F_DUPFD, 45);
    CHECK(a == 40 && b == 45);
    CHECK(!syscall(SYS_close_range, 40, 44, 4) && (fcntl(a, F_GETFD) & FD_CLOEXEC) &&
          !(fcntl(b, F_GETFD) & FD_CLOEXEC));
    CHECK(!syscall(SYS_close_range, 40, UINT32_MAX, 0));
    ERROR(fcntl(a, F_GETFD), EBADF);
    ERROR(fcntl(b, F_GETFD), EBADF);
    CHECK(fcntl(fd, F_GETFD) >= 0);
    ERROR(syscall(SYS_close_range, 9, 8, 0), EINVAL);
    ERROR(syscall(SYS_close_range, 9, 10, 1), EINVAL);
    atomic_store(&entered, 0);
    atomic_store(&done, 0);
    CHECK(!pthread_create(&helper, NULL, idle_thread, NULL));
    while (!atomic_load(&entered))
        pause_ms(1);
    if (!host)
        ERROR(syscall(SYS_close_range, 40, 50, 2), EOPNOTSUPP);
    atomic_store(&done, 1);
    CHECK(!pthread_join(helper, NULL));
    CHECK(!syscall(SYS_close_range, 128, UINT32_MAX, 2));
    CHECK(!close(fd) && !unlink("range"));
    puts("PASS relative/absolute clock sleeps, EINTR remainder, bounded close-range/CLOEXEC and "
         "shared-table rejection");
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--completion-command")) {
        FILE *f = fopen("/tmp/completion-executed", "w");
        CHECK(f && fputs("executed\n", f) >= 0 && !fclose(f));
        puts("COMPLETION_EXECUTED_ON_ENTER");
        return 0;
    }
    host = argc > 1 && !strcmp(argv[1], "--host");
    snprintf(dir, sizeof(dir), "/tmp/linux-semantics-%ld", (long)getpid());
    CHECK(!mkdir(dir, 0700) && !chdir(dir));
    struct sigaction sa = {.sa_handler = handler};
    CHECK(!sigemptyset(&sa.sa_mask) && !sigaction(SIGUSR1, &sa, NULL));
    alarm(20);
    links();
    metadata();
    locks();
    ownership();
    clock_and_close();
    CHECK(signals >= 2);
    CHECK(!chdir("/") && !rmdir(dir));
    puts("LINUX_SEMANTICS_PASS");
    return 0;
}
