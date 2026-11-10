/* Native ABI checks for foreground groups and scoped terminal ownership. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define REQUIRE(c)                                                                                 \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "foreground-check:%d: %s (errno=%d)\n", __LINE__, #c, errno);          \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static int terminal;
static volatile sig_atomic_t group_signal_hits;
static void group_signal_handler(int sig) {
    (void)sig;
    group_signal_hits++;
}
static void inherited_group(void) {
    REQUIRE(signal(SIGUSR1, group_signal_handler) != SIG_ERR);
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    REQUIRE(sigprocmask(SIG_BLOCK, &blocked, &previous) == 0);
    int ready[2];
    REQUIRE(pipe(ready) == 0);
    pid_t child = fork();
    REQUIRE(child >= 0);
    if (!child) {
        close(0);
        close(ready[0]);
        REQUIRE(write(ready[1], "R", 1) == 1);
        while (!group_signal_hits)
            sigsuspend(&previous);
        _exit(group_signal_hits == 1 ? 0 : 1);
    }
    close(ready[1]);
    char byte;
    REQUIRE(read(ready[0], &byte, 1) == 1);
    close(ready[0]);
    REQUIRE(getpgid(child) == getpgrp());
    REQUIRE(kill(-getpgrp(), SIGUSR1) == 0);
    REQUIRE(sigprocmask(SIG_SETMASK, &previous, NULL) == 0);
    int status;
    REQUIRE(waitpid(0, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    REQUIRE(group_signal_hits == 1 && signal(SIGUSR1, SIG_DFL) != SIG_ERR);
    puts("FOREGROUND INHERITED GROUP SIGNAL WAIT PASS");
}

static void transfer(int fd, char *byte, int writing) {
    REQUIRE((writing ? write(fd, byte, 1) : read(fd, byte, 1)) == 1);
}
static void wait_ok(pid_t child) {
    int status;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void permissions(void) {
    int ready[2], go[2];
    REQUIRE(pipe(ready) == 0 && pipe(go) == 0);
    struct termios saved;
    REQUIRE(tcgetattr(terminal, &saved) == 0);
    pid_t parent = getpid(), group = getpgrp();
    pid_t child = fork();
    REQUIRE(child >= 0);
    if (!child) {
        close(ready[0]);
        close(go[1]);
        REQUIRE(getpgrp() == group && getsid(0) == getsid(parent));
        REQUIRE(setpgid(0, 0) == 0);
        char byte = 'R';
        transfer(ready[1], &byte, 1);
        transfer(go[0], &byte, 0);
        REQUIRE(tcgetpgrp(terminal) == group);
        errno = 0;
        REQUIRE(tcsetpgrp(terminal, getpgrp()) == -1 && errno == EPERM);
        errno = 0;
        REQUIRE(setpgid(0, group) == -1 && errno == EPERM);
        errno = 0;
        REQUIRE(tcsetattr(terminal, TCSANOW, &saved) == -1 && errno == EPERM);
        int n;
        errno = 0;
        REQUIRE(ioctl(terminal, FIONREAD, &n) == -1 && errno == EPERM);
        int flags = fcntl(terminal, F_GETFL);
        REQUIRE(flags >= 0);
        REQUIRE(fcntl(terminal, F_SETFL, flags | O_NONBLOCK) == 0);
        errno = 0;
        REQUIRE(read(terminal, &byte, 1) == -1 && (errno == EPERM || errno == EIO));
        REQUIRE(fcntl(terminal, F_SETFL, flags) == 0);
        char clipboard_info[16];
        errno = 0;
        REQUIRE(syscall(414, 0, clipboard_info, sizeof(clipboard_info), 0) == -1 && errno == EPERM);
        puts("FOREGROUND BACKGROUND REFUSAL PASS");
        exit(0);
    }
    close(ready[1]);
    close(go[0]);
    char byte;
    transfer(ready[0], &byte, 0);
    REQUIRE(setpgid(child, child) == 0); /* idempotent parent/child assignment */
    REQUIRE(getpgid(child) == child && getsid(child) == getsid(0));
    transfer(go[1], &byte, 1);
    wait_ok(child);
    REQUIRE(tcgetpgrp(terminal) == group);
    struct termios after;
    REQUIRE(tcgetattr(terminal, &after) == 0);
    REQUIRE(saved.c_iflag == after.c_iflag && saved.c_oflag == after.c_oflag &&
            saved.c_cflag == after.c_cflag && saved.c_lflag == after.c_lflag &&
            memcmp(saved.c_cc, after.c_cc, 19) == 0);
    errno = 0;
    REQUIRE(getpgid(child) == -1 && errno == ESRCH);
    errno = 0;
    REQUIRE(setpgid(-1, 0) == -1 && errno == EINVAL);
    close(ready[0]);
    close(go[1]);
    puts("FOREGROUND GROUP ABI PASS");
}
static void native_spawn_return(void) {
    pid_t member = fork();
    REQUIRE(member >= 0);
    if (!member) {
        REQUIRE(getpgrp() != getpid());
        char *arguments[] = {"foreground-check", "--close-native-input", NULL};
        pid_t child = syscall(400, "/bin/foreground-check", arguments, 0);
        REQUIRE(child > 0);
        int status;
        REQUIRE(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
        REQUIRE(tcgetpgrp(terminal) == getpgrp());
        _exit(0);
    }
    wait_ok(member);
    REQUIRE(tcgetpgrp(terminal) == getpgrp());
    puts("FOREGROUND NATIVE SPAWN RETURN PASS");
}

static void controller_exit(void) {
    int ready[2], go[2], orphan[2];
    REQUIRE(pipe(ready) == 0 && pipe(go) == 0 && pipe(orphan) == 0);
    pid_t owner = getpgrp();
    struct termios saved;
    REQUIRE(tcgetattr(terminal, &saved) == 0);
    pid_t child = fork();
    REQUIRE(child >= 0);
    if (!child) {
        close(ready[0]);
        close(go[1]);
        REQUIRE(setpgid(0, 0) == 0);
        char byte = 'R';
        transfer(ready[1], &byte, 1);
        transfer(go[0], &byte, 0);
        pid_t grandchild = fork();
        REQUIRE(grandchild >= 0);
        if (!grandchild) {
            close(orphan[0]);
            REQUIRE(setpgid(0, 0) == 0);
            unsigned attempt;
            for (attempt = 0; attempt < 10000 && tcgetpgrp(terminal) != getpgrp(); attempt++)
                usleep(1000);
            REQUIRE(attempt < 10000);
            struct termios raw = saved;
            raw.c_lflag &= ~(ICANON | ECHO);
            REQUIRE(tcsetattr(terminal, TCSANOW, &raw) == 0);
            transfer(orphan[1], &byte, 1);
            for (;;)
                pause();
        }
        REQUIRE(setpgid(grandchild, grandchild) == 0);
        REQUIRE(tcsetpgrp(terminal, grandchild) == 0);
        close(orphan[1]);
        transfer(orphan[0], &byte, 0);
        struct termios raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        /* The group reader can change mode; its controller cannot. */
        errno = 0;
        REQUIRE(tcsetattr(terminal, TCSANOW, &raw) == -1 && errno == EPERM);
        _exit(0); /* kernel must restore this controller's parent and mode */
    }
    close(ready[1]);
    close(go[0]);
    close(orphan[1]);
    char byte;
    transfer(ready[0], &byte, 0);
    REQUIRE(setpgid(child, child) == 0 && tcsetpgrp(terminal, child) == 0);
    transfer(go[1], &byte, 1);
    wait_ok(child);
    REQUIRE(tcgetpgrp(terminal) == owner);
    /* Our outer capability is now stale because the dead controller already
       restored us. Reclaim consumes it without granting any new terminal. */
    REQUIRE(tcsetpgrp(terminal, owner) == 0);
    struct termios after;
    REQUIRE(tcgetattr(terminal, &after) == 0);
    REQUIRE(saved.c_iflag == after.c_iflag && saved.c_oflag == after.c_oflag &&
            saved.c_cflag == after.c_cflag && saved.c_lflag == after.c_lflag &&
            memcmp(saved.c_cc, after.c_cc, 19) == 0);
    close(ready[0]);
    close(go[1]);
    puts("FOREGROUND CONTROLLER EXIT PASS");
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--close-native-input")) {
        REQUIRE(tcgetpgrp(0) == getpid());
        REQUIRE(close(0) == 0);
        int reopened = open("/dev/tty", O_RDWR);
        REQUIRE(reopened >= 0 && tcgetpgrp(reopened) == getpgid(getppid()));
        REQUIRE(close(reopened) == 0);
        return 0;
    }
    terminal = open("/dev/tty", O_RDWR);
    REQUIRE(terminal >= 0);
    REQUIRE(tcgetpgrp(terminal) == getpid());
    inherited_group();
    REQUIRE(setpgid(0, 0) == 0);
    permissions();
    native_spawn_return();
    controller_exit();
    REQUIRE(close(terminal) == 0);
    puts("FOREGROUND CHECK PASS");
    return 0;
}
