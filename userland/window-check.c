#define _GNU_SOURCE
#include "../include/uapi/window.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL window-check:%d %s errno=%d\n", __LINE__, #x, errno);            \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define ERROR(expr, e)                                                                             \
    do {                                                                                           \
        errno = 0;                                                                                 \
        CHECK((expr) == -1 && errno == (e));                                                       \
    } while (0)
static long op(unsigned n, void *p, size_t size) { return syscall(OS_WINDOW_SYSCALL, n, p, size); }
static void byte(int fd) {
    char c;
    CHECK(read(fd, &c, 1) == 1 && c == 'Y');
}
static void go(int fd) { CHECK(write(fd, "Y", 1) == 1); }
static void child(int input, int output) {
    ERROR(op(OS_WINDOW_SERVER_START, NULL, 0), EPERM);
    struct os_window_info w = {.version = OS_WINDOW_VERSION, .width = 4, .height = 3};
    strcpy(w.title, "Fixture — α");
    struct os_window_info bad = w;
    bad.width = OS_WINDOW_MAX_WIDTH + 1;
    ERROR(op(OS_WINDOW_CREATE, &bad, sizeof(bad)), EINVAL);
    ERROR(op(OS_WINDOW_CREATE, (void *)1, sizeof(w)), EFAULT);
    CHECK(!op(OS_WINDOW_CREATE, &w, sizeof(w)) && w.id && w.client == (unsigned)getpid());
    bad = (struct os_window_info){.version = OS_WINDOW_VERSION, .width = 1, .height = 1};
    ERROR(op(OS_WINDOW_CREATE, &bad, sizeof(bad)), EBUSY);
    uint32_t pixels[12];
    for (unsigned i = 0; i < 12; i++)
        pixels[i] = 0x223344 + i;
    struct os_window_frame f = {
        .id = w.id, .width = 4, .height = 3, .stride = 4, .pixels = (uintptr_t)pixels};
    CHECK(!op(OS_WINDOW_PRESENT, &f, sizeof(f)));
    CHECK(write(output, &w, sizeof(w)) == sizeof(w));
    byte(input);
    struct os_window_frame invalid = f;
    invalid.stride = 5;
    ERROR(op(OS_WINDOW_PRESENT, &invalid, sizeof(invalid)), EINVAL);
    invalid = f;
    invalid.pixels = 1;
    ERROR(op(OS_WINDOW_PRESENT, &invalid, sizeof(invalid)), EFAULT);
    go(output);
    byte(input);
    struct os_window_event events[64];
    struct os_window_events request = {w.id, 64, (uintptr_t)events};
    long n = op(OS_WINDOW_INPUT, &request, sizeof(request));
    CHECK(n == 3 && events[0].type == OS_WINDOW_FOCUS && events[0].code == 1 &&
          events[1].type == OS_WINDOW_HID_KEY && events[1].code == 26 && events[1].buttons == 1 &&
          events[2].type == OS_WINDOW_POINTER && events[2].dx == 7);
    ERROR(op(OS_WINDOW_INPUT, &request, sizeof(request)), EAGAIN);
    go(output);
    byte(input);
    n = op(OS_WINDOW_INPUT, &request, sizeof(request));
    CHECK(n > 0 && events[0].type == OS_WINDOW_OVERFLOW);
    go(output);
    byte(input);
    n = op(OS_WINDOW_INPUT, &request, sizeof(request));
    CHECK(n == 1 && events[0].type == OS_WINDOW_FOCUS && !events[0].code);
    go(output);
    byte(input);
    n = op(OS_WINDOW_INPUT, &request, sizeof(request));
    CHECK(n == 1 && events[0].type == OS_WINDOW_CLOSE);
    ERROR(op(OS_WINDOW_INPUT, &request, sizeof(request)), EPIPE);
    ERROR(op(OS_WINDOW_PRESENT, &f, sizeof(f)), EPIPE);
    struct os_window_token token = {w.id, 0};
    CHECK(!op(OS_WINDOW_CLIENT_CLOSE, &token, sizeof(token)));
    go(output);
    byte(input);
    CHECK(!op(OS_WINDOW_CREATE, &bad, sizeof(bad))); // Task exit must reclaim this second record.
    _exit(0);
}
int main(void) {
    alarm(20);
    ERROR(op(OS_WINDOW_SERVER_START, NULL, 0), EPERM);
    CHECK(!syscall(402, 1, 0, 0));
    CHECK(!op(OS_WINDOW_SERVER_START, NULL, 0));
    int a[2], b[2];
    CHECK(!pipe(a) && !pipe(b));
    int pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        close(a[1]);
        close(b[0]);
        child(a[0], b[1]);
    }
    close(a[0]);
    close(b[1]);
    struct os_window_info w;
    CHECK(read(b[0], &w, sizeof(w)) == sizeof(w));
    struct os_window_info next;
    CHECK(!op(OS_WINDOW_SERVER_NEXT, &next, sizeof(next)) && next.id == w.id && next.width == 4 &&
          next.height == 3);
    ERROR(op(OS_WINDOW_SERVER_NEXT, &next, sizeof(next)), EAGAIN);
    uint32_t pixels[12] = {0};
    struct os_window_frame f = {w.id, 4, 3, 4, (uintptr_t)pixels, 0};
    ERROR(op(OS_WINDOW_PRESENT, &f, sizeof(f)), EPERM);
    CHECK(!op(OS_WINDOW_SERVER_READ, &f, sizeof(f)) && f.sequence == 1);
    for (unsigned i = 0; i < 12; i++)
        CHECK(pixels[i] == 0x223344 + i);
    ERROR(op(OS_WINDOW_SERVER_READ, &f, sizeof(f)), EAGAIN);
    go(a[1]);
    byte(b[0]);
    f.sequence = 0;
    CHECK(!op(OS_WINDOW_SERVER_READ, &f, sizeof(f)) && f.sequence == 1);
    for (unsigned i = 0; i < 12; i++)
        CHECK(pixels[i] == 0x223344 + i);
    struct os_window_focus focus = {w.id, 1};
    CHECK(!op(OS_WINDOW_SERVER_FOCUS, &focus, sizeof(focus)));
    struct os_window_event events[64] = {{.type = OS_WINDOW_HID_KEY, .code = 26, .buttons = 1},
                                         {.type = OS_WINDOW_POINTER, .dx = 7, .dy = -3}};
    struct os_window_events request = {w.id, 2, (uintptr_t)events};
    CHECK(!op(OS_WINDOW_SERVER_SEND, &request, sizeof(request)));
    go(a[1]);
    byte(b[0]);
    for (unsigned i = 0; i < 64; i++)
        events[i] = (struct os_window_event){.type = OS_WINDOW_KEY, .code = 'a'};
    request.count = 64;
    for (unsigned i = 0; i < 3; i++)
        CHECK(!op(OS_WINDOW_SERVER_SEND, &request, sizeof(request)));
    go(a[1]);
    byte(b[0]);
    focus.active = 0;
    CHECK(!op(OS_WINDOW_SERVER_FOCUS, &focus, sizeof(focus)));
    ERROR(op(OS_WINDOW_SERVER_SEND, &request, sizeof(request)), EAGAIN);
    go(a[1]);
    byte(b[0]);
    struct os_window_token token = {w.id, 0};
    CHECK(!op(OS_WINDOW_SERVER_CLOSE, &token, sizeof(token)));
    ERROR(op(OS_WINDOW_SERVER_READ, &f, sizeof(f)), EPIPE);
    go(a[1]);
    byte(b[0]);
    go(a[1]);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    ERROR(op(OS_WINDOW_SERVER_NEXT, &next, sizeof(next)), EAGAIN);
    CHECK(!close(a[1]) && !close(b[0]));
    CHECK(!syscall(402, 3, 0, 0));
    ERROR(op(OS_WINDOW_CREATE, &next, sizeof(next)), ENODEV);
    CHECK(!syscall(402, 1, 0, 0) && !op(OS_WINDOW_SERVER_START, NULL, 0));
    CHECK(!syscall(402, 3, 0, 0));
    puts("WINDOW_SEMANTICS_PASS ownership, copied frame atomicity, focus, overflow, close and "
         "process-exit cleanup");
    return 0;
}
