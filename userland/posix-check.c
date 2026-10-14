#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static _Thread_local int tls_marker = 37;
static volatile sig_atomic_t seen;

static void handle(int signal) {
    if (signal == SIGUSR1) {
        seen++;
    }
}

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            printf("POSIX CHECK FAIL line=%d errno=%d: %s\n", __LINE__, errno, #expr);             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(int argc, char **argv) {
    if (argc == 2 &&
        (!strcmp(argv[1], "--environment-parent") || !strcmp(argv[1], "--environment-child"))) {
        if (!environ[0] || environ[0][0] || !getenv("TERM") || strcmp(getenv("TERM"), "vt100")) {
            return 11;
        }
        if (!strcmp(argv[1], "--environment-child")) {
            return 0;
        }
        char *args[] = {"posix-check", "--environment-child", NULL};
        long child = syscall(400, "/bin/posix-check", args, 1);
        int status;
        return child > 1 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                       !WEXITSTATUS(status)
                   ? 0
                   : 12;
    }

    if (argc == 3 && (!strcmp(argv[1], "--signal-child") || !strcmp(argv[1], "--pipe-child"))) {
        struct timespec pause = {0, 50000000};
        nanosleep(&pause, NULL);
        if (kill(atoi(argv[2]), SIGUSR1)) {
            return 2;
        }
        return !strcmp(argv[1], "--pipe-child") && write(1, "pong", 4) != 4 ? 3 : 0;
    }

    CHECK(tls_marker == 37);
    tls_marker = 91;
    CHECK(getenv("TERM") && getenv("PATH"));

    char *env_args[] = {"posix-check", "--environment-parent", NULL},
         *test_env[] = {"", "TERM=vt100", "PATH=/bin", NULL};
    long environment_pid = syscall(405, "/bin/posix-check", env_args, test_env, 1);
    int environment_status;
    CHECK(environment_pid > 1 &&
          waitpid(environment_pid, &environment_status, 0) == environment_pid &&
          WIFEXITED(environment_status) && !WEXITSTATUS(environment_status));

    char *memory = malloc(200000);
    CHECK(memory);
    memset(memory, 0x5a, 200000);
    CHECK(memory[199999] == 0x5a);
    free(memory);

    void *guard = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(guard != MAP_FAILED);
    CHECK(!mprotect(guard, 4096, PROT_READ | PROT_WRITE));
    *(int *)guard = 123;
    CHECK(*(int *)guard == 123);
    CHECK(!munmap(guard, 4096));

    struct sigaction action = {0};
    action.sa_handler = handle;
    CHECK(!sigaction(SIGUSR1, &action, NULL));
    CHECK(!kill(getpid(), SIGUSR1));
    CHECK(seen == 1 && tls_marker == 91);

    sigset_t mask, pending;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    CHECK(!sigprocmask(SIG_BLOCK, &mask, NULL));
    CHECK(!kill(getpid(), SIGUSR1));
    CHECK(seen == 1);
    CHECK(!sigpending(&pending));
    CHECK(sigismember(&pending, SIGUSR1) == 1);
    CHECK(!sigprocmask(SIG_UNBLOCK, &mask, NULL));
    CHECK(seen == 2);

    char parent[24];
    snprintf(parent, sizeof(parent), "%d", getpid());
    char *child_args[] = {"posix-check", "--signal-child", parent, NULL};
    long child = syscall(405, "/bin/posix-check", child_args, environ, 1);
    CHECK(child > 1);
    struct timespec pause = {1, 0}, left = {0};
    errno = 0;
    CHECK(nanosleep(&pause, &left) == -1 && errno == EINTR);
    CHECK(seen == 3 && (left.tv_sec || left.tv_nsec));
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));

    action.sa_flags = SA_RESTART;
    CHECK(!sigaction(SIGUSR1, &action, NULL));
    int restart_pipe[2];
    CHECK(!pipe(restart_pipe));
    struct {
        int32_t fd[3];
        uint32_t reserved;
        uint64_t cwd;
    } streams = {{0, restart_pipe[1], 2}, 0, 0};
    child_args[1] = "--pipe-child";
    child = syscall(404, "/bin/posix-check", child_args, &streams);
    CHECK(child > 1);
    CHECK(!close(restart_pipe[1]));
    char restarted[4];
    CHECK(read(restart_pipe[0], restarted, 4) == 4 && !memcmp(restarted, "pong", 4));
    CHECK(seen == 4);
    CHECK(!close(restart_pipe[0]));
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));

    int fds[2];
    CHECK(!pipe2(fds, O_NONBLOCK | O_CLOEXEC));
    struct pollfd pollfd = {fds[0], POLLIN, 0};
    CHECK(poll(&pollfd, 1, 0) == 0);
    CHECK(write(fds[1], "ping", 4) == 4);
    CHECK(poll(&pollfd, 1, 0) == 1 && (pollfd.revents & POLLIN));

    fd_set readset;
    FD_ZERO(&readset);
    FD_SET(fds[0], &readset);
    struct timeval timeout = {0};
    CHECK(select(fds[0] + 1, &readset, NULL, NULL, &timeout) == 1);
    char buffer[8];
    CHECK(read(fds[0], buffer, 8) == 4 && !memcmp(buffer, "ping", 4));
    struct timespec zero = {0};
    CHECK(ppoll(&pollfd, 1, &zero, NULL) == 0);
    CHECK(!close(fds[1]));
    CHECK(poll(&pollfd, 1, 0) == 1 && (pollfd.revents & POLLHUP));
    CHECK(read(fds[0], buffer, 8) == 0);
    CHECK(!close(fds[0]));

    errno = 0;
    CHECK(syscall(SYS_poll, (void *)1, 1, 0) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(syscall(SYS_poll, &pollfd, sysconf(_SC_OPEN_MAX)+1, 0) == -1 && errno == EINVAL);
    struct timespec overflow = {INT64_MAX / 1000000, 999999999};
    errno = 0;
    CHECK(ppoll(NULL, 0, &overflow, NULL) == -1 && errno == EINVAL);

    errno = 0;
    CHECK(syscall(SYS_rt_sigreturn) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(sigaction(SIGKILL, &action, NULL) == -1 && errno == EINVAL);
    action.sa_flags = SA_NOCLDWAIT;
    errno = 0;
    CHECK(sigaction(SIGUSR1, &action, NULL) == -1 && errno == ENOTSUP);

    mode_t previous = umask(077);
    struct stat metadata;
    CHECK(!mkdir("/tmp/posix-check-mask", 0777));
    CHECK(!stat("/tmp/posix-check-mask", &metadata) && (metadata.st_mode & 0777) == 0700);
    int document = open("/tmp/posix-check-mask/file", O_WRONLY | O_CREAT | O_EXCL, 0666);
    CHECK(document >= 0);
    CHECK(!fstat(document, &metadata) && (metadata.st_mode & 0777) == 0600);
    CHECK(write(document, "discard", 7) == 7);
    CHECK(!ftruncate(document, 0));
    CHECK(!fstat(document, &metadata) && !metadata.st_size);
    CHECK(!close(document));
    CHECK(!unlink("/tmp/posix-check-mask/file"));
    CHECK(!rmdir("/tmp/posix-check-mask"));
    umask(previous);

    CHECK(syscall(406, fds, O_CLOEXEC) == 0);
    CHECK(isatty(fds[1]));
    struct termios mode;
    CHECK(!tcgetattr(fds[0], &mode));
    mode.c_iflag = mode.c_oflag = mode.c_lflag = 0;
    mode.c_cc[VMIN] = 1;

    mode.c_cc[VTIME] = 0;
    CHECK(!tcsetattr(fds[0], TCSANOW, &mode));

    struct winsize size = {29, 93, 651, 493}, actual;
    CHECK(!ioctl(fds[0], TIOCSWINSZ, &size));
    CHECK(!ioctl(fds[1], TIOCGWINSZ, &actual));
    CHECK(actual.ws_row == 29 && actual.ws_col == 93);
    size.ws_col = 241;
    errno = 0;
    CHECK(ioctl(fds[0], TIOCSWINSZ, &size) == -1 && errno == EINVAL);
    mode.c_cc[VTIME] = 1;
    errno = 0;
    CHECK(tcsetattr(fds[0], TCSANOW, &mode) == -1 && errno == ENOTSUP);
    mode.c_cc[VTIME] = 0;
    CHECK(write(fds[0], "raw", 3) == 3);
    CHECK(read(fds[1], buffer, 8) == 3 && !memcmp(buffer, "raw", 3));
    CHECK(write(fds[1], "out", 3) == 3);
    CHECK(read(fds[0], buffer, 8) == 3 && !memcmp(buffer, "out", 3));

    mode.c_lflag = ICANON;
    CHECK(!tcsetattr(fds[0], TCSANOW, &mode));
    char overlong[600];
    memset(overlong, 'x', sizeof(overlong));
    overlong[599] = '\n';
    CHECK(write(fds[0], overlong, sizeof(overlong)) == sizeof(overlong));
    errno = 0;
    CHECK(read(fds[1], buffer, 8) == -1 && errno == E2BIG);
    CHECK(write(fds[0], "safe\n", 5) == 5);
    CHECK(read(fds[1], buffer, 8) == 5 && !memcmp(buffer, "safe\n", 5));

    CHECK(!close(fds[0]));
    CHECK(read(fds[1], buffer, 8) == 0);
    errno = 0;
    CHECK(write(fds[1], "x", 1) == -1 && errno == EIO);
    CHECK(!close(fds[1]));

    puts("POSIX CHECK PASS: static musl, TLS, allocator, guards, signals, poll/select, PTY and "
         "resize");
    return 0;
}
