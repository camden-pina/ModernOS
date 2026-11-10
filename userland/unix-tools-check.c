#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Coast native PTY/spawn interfaces, with the public UAPI's exact layout. */
enum { COAST_SPAWN_IO = 404, COAST_PTY = 406 };
struct spawn_io {
    int32_t fd[3];
    uint32_t reserved;
    uint64_t cwd;
};
_Static_assert(sizeof(struct spawn_io) == 24, "spawn_io ABI");

static long long milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int output_until(int fd, const char *text) {
    char output[8192];
    size_t used = 0;
    long long deadline = milliseconds() + 5000;
    while (milliseconds() < deadline) {
        struct pollfd ready = {fd, POLLIN, 0};
        int result = poll(&ready, 1, 100);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            return 0;
        if (!result)
            continue;
        if (used > sizeof(output) / 2) {
            memmove(output, output + used / 2, used - used / 2);
            used -= used / 2;
        }
        ssize_t count = read(fd, output + used, sizeof(output) - used - 1);
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count <= 0)
            return 0;
        used += count;
        output[used] = 0;
        if (strstr(output, text))
            return 1;
    }
    return 0;
}

static int input_ready(int fd) {
    for (int attempt = 0; attempt < 500; attempt++) {
        struct termios mode;
        if (tcgetattr(fd, &mode))
            return 0;
        if (!(mode.c_lflag & (ICANON | ECHO)))
            return 1;
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int pager_case(int piped) {
    int pty[2] = {-1, -1}, input[2] = {-1, -1}, child = -1, result = 1;
    struct termios original = {0}, actual = {0};
    struct winsize size = {10, 60, 0, 0};
    int original_flags = 0;
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            printf("UNIX TOOLS CHECK FAIL piped=%d line=%d errno=%d: %s\n", piped, __LINE__,       \
                   errno, #expr);                                                                  \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)

    CHECK(syscall(COAST_PTY, pty, O_CLOEXEC) == 0);
    CHECK(!tcgetattr(pty[1], &original));
    CHECK(!ioctl(pty[0], TIOCSWINSZ, &size));
    original_flags = fcntl(pty[1], F_GETFL);
    CHECK(original_flags >= 0);
    if (piped) {
        CHECK(!pipe2(input, O_CLOEXEC));
        for (int line = 1; line <= 40; line++) {
            char text[40];
            int length = snprintf(text, sizeof(text), "PAGER_LINE_%02d\n", line);
            CHECK(write(input[1], text, length) == length);
        }
        CHECK(!close(input[1]));
        input[1] = -1;
    }
    struct spawn_io streams = {.fd = {piped ? input[0] : pty[1], pty[1], pty[1]}};
    char *file_args[] = {"less", "-N", "/tmp/unix-tools-less.txt", NULL};
    char *pipe_args[] = {"less", "-N", NULL};
    child = syscall(COAST_SPAWN_IO, "/bin/less", piped ? pipe_args : file_args, &streams);
    CHECK(child > 1);
    if (input[0] >= 0) {
        CHECK(!close(input[0]));
        input[0] = -1;
    }
    CHECK(output_until(pty[0], "PAGER_LINE_01"));
    CHECK(input_ready(pty[1]));
    /* The private PTY creator may recover an exited reader, but cannot read
       or reconfigure the PTY while the foreground reader is still alive. */
    int live_flags = fcntl(pty[1], F_GETFL);
    CHECK(live_flags >= 0);
    CHECK(!fcntl(pty[1], F_SETFL, live_flags | O_NONBLOCK));
    char forbidden;
    errno = 0;
    CHECK(read(pty[1], &forbidden, 1) == -1 && errno == EPERM);
    CHECK(!fcntl(pty[1], F_SETFL, live_flags));
    CHECK(!tcgetattr(pty[1], &actual));
    errno = 0;
    CHECK(tcsetattr(pty[1], TCSANOW, &actual) == -1 && errno == EPERM);
    const char search[] = "/PAGER_LINE_2[05]\r";
    CHECK(write(pty[0], search, sizeof(search) - 1) == (ssize_t)(sizeof(search) - 1));
    CHECK(output_until(pty[0], "PAGER_LINE_20"));
    CHECK(input_ready(pty[1]));
    size.ws_row = 12;
    size.ws_col = 72;
    CHECK(!ioctl(pty[0], TIOCSWINSZ, &size));
    /* The kernel routes resize to the foreground reader even with piped stdin. */
    CHECK(write(pty[0], "G", 1) == 1);
    CHECK(output_until(pty[0], "PAGER_LINE_40"));
    CHECK(input_ready(pty[1]));
    CHECK(write(pty[0], "q", 1) == 1);
    long long deadline = milliseconds() + 5000;
    int status = 0, waited = 0;
    while (milliseconds() < deadline && !(waited = waitpid(child, &status, WNOHANG))) {
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    CHECK(waited == child && WIFEXITED(status) && !WEXITSTATUS(status));
    child = -1;
    CHECK(!tcgetattr(pty[1], &actual));
    CHECK(actual.c_iflag == original.c_iflag && actual.c_oflag == original.c_oflag &&
          actual.c_cflag == original.c_cflag && actual.c_lflag == original.c_lflag &&
          !memcmp(actual.c_cc, original.c_cc, NCCS));
    CHECK(fcntl(pty[1], F_GETFL) == original_flags);
    CHECK(write(pty[0], "restored\n", 9) == 9);
    struct pollfd ready = {pty[1], POLLIN, 0};
    CHECK(poll(&ready, 1, 1000) == 1);
    char restored[16];
    CHECK(read(pty[1], restored, sizeof(restored)) == 9 && !memcmp(restored, "restored\n", 9));
    result = 0;
done:
    if (child > 1) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    for (int i = 0; i < 2; i++) {
        if (input[i] >= 0)
            close(input[i]);
        if (pty[i] >= 0)
            close(pty[i]);
    }
    return result;
#undef CHECK
}

static int pressure_case(void) {
    int pty[2] = {-1, -1}, child = -1, result = 1;
    struct termios original = {0}, actual = {0};
    struct winsize size = {100, 240, 0, 0};
    int original_flags = 0;
    char output[32768], line[225];
    size_t used = 0;
    FILE *file = NULL;
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            printf("UNIX TOOLS PRESSURE FAIL line=%d errno=%d: %s\n", __LINE__, errno, #expr);     \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)

    file = fopen("/tmp/unix-tools-wide.txt", "w");
    CHECK(file);
    for (int number = 1; number <= 90; number++) {
        int prefix = snprintf(line, sizeof(line), "PRESSURE_%02d_", number);
        memset(line + prefix, 'x', 210);
        line[prefix + 210] = '\n';
        CHECK(fwrite(line, 1, prefix + 211, file) == (size_t)(prefix + 211));
    }
    int closed = fclose(file);
    file = NULL;
    CHECK(!closed);
    CHECK(syscall(COAST_PTY, pty, O_CLOEXEC) == 0);
    CHECK(!ioctl(pty[0], TIOCSWINSZ, &size));
    CHECK(!tcgetattr(pty[1], &original));
    CHECK(fcntl(pty[1], F_SETFL, O_RDWR | O_NONBLOCK) == 0);
    original_flags = fcntl(pty[1], F_GETFL);
    CHECK(original_flags >= 0);
    struct spawn_io streams = {.fd = {pty[1], pty[1], pty[1]}};
    char *args[] = {"less", "-F", "-S", "/tmp/unix-tools-wide.txt", NULL};
    child = syscall(COAST_SPAWN_IO, "/bin/less", args, &streams);
    CHECK(child > 1);
    /* Withhold reads until the first screen exceeds the real 16 KiB queue. */
    struct timespec slow_reader = {0, 200000000};
    CHECK(!nanosleep(&slow_reader, NULL));
    int status = 0;
    CHECK(waitpid(child, &status, WNOHANG) == 0);
    output[0] = 0;
    int prefix = snprintf(line, sizeof(line), "PRESSURE_90_");
    memset(line + prefix, 'x', 210);
    memcpy(line + prefix + 210, "\r\n", 3);
    long long deadline = milliseconds() + 5000;
    while (milliseconds() < deadline && !strstr(output, line)) {
        struct pollfd ready = {pty[0], POLLIN, 0};
        int available = poll(&ready, 1, 100);
        if (available < 0 && errno == EINTR)
            continue;
        CHECK(available >= 0);
        if (!available)
            continue;
        CHECK(used < sizeof(output) - 1);
        ssize_t count = read(pty[0], output + used, sizeof(output) - used - 1);
        CHECK(count > 0);
        used += count;
        output[used] = 0;
    }
    char *position = output;
    for (int number = 1; number <= 90; number++) {
        prefix = snprintf(line, sizeof(line), "PRESSURE_%02d_", number);
        memset(line + prefix, 'x', 210);
        memcpy(line + prefix + 210, "\r\n", 3);
        position = strstr(position, line);
        CHECK(position);
        position += strlen(line);
    }
    int rows = 0;
    for (position = output; (position = strstr(position, "PRESSURE_")); position++)
        rows++;
    CHECK(rows == 90 && used > 16384);
    deadline = milliseconds() + 5000;
    int waited = 0;
    while (milliseconds() < deadline && !(waited = waitpid(child, &status, WNOHANG))) {
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    CHECK(waited == child && WIFEXITED(status) && !WEXITSTATUS(status));
    child = -1;
    CHECK(!tcgetattr(pty[1], &actual));
    CHECK(actual.c_iflag == original.c_iflag && actual.c_oflag == original.c_oflag &&
          actual.c_cflag == original.c_cflag && actual.c_lflag == original.c_lflag &&
          !memcmp(actual.c_cc, original.c_cc, NCCS));
    CHECK(fcntl(pty[1], F_GETFL) == original_flags);
    result = 0;
done:
    if (file)
        fclose(file);
    if (child > 1) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    for (int i = 0; i < 2; i++)
        if (pty[i] >= 0)
            close(pty[i]);
    return result;
#undef CHECK
}

int main(void) {
    if (setpgid(0, getpgrp())) {
        puts("UNIX TOOLS CHECK FAIL: cannot enable foreground permission checks");
        return 1;
    }
    FILE *file = fopen("/tmp/unix-tools-less.txt", "w");
    if (!file)
        return 1;
    for (int line = 1; line <= 40; line++)
        fprintf(file, "PAGER_LINE_%02d\n", line);
    if (fclose(file))
        return 1;
    struct termios before = {0}, after = {0};
    if (tcgetattr(0, &before) || pager_case(0) || pager_case(1) || pressure_case() ||
        tcgetattr(0, &after))
        return 1;
    if (before.c_iflag != after.c_iflag || before.c_oflag != after.c_oflag ||
        before.c_lflag != after.c_lflag) {
        puts("UNIX TOOLS CHECK FAIL: console mode changed by PTY pager");
        return 1;
    }
    puts("UNIX TOOLS PTY CHECK PASS: file/pipe, regex, resize, quit, 100x240 backpressure and "
         "restoration");
    return 0;
}
