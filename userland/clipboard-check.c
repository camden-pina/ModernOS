/* Real guest ABI, atomic snapshot and foreground-access clipboard checks. */
#include "../include/uapi/clipboard.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define CLIPBOARD 414
#define REQUIRE(c)                                                                                 \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "clipboard-check:%d: %s errno=%d\n", __LINE__, #c, errno);             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static char text[OS_CLIPBOARD_LIMIT], copy[OS_CLIPBOARD_LIMIT];
static long clip(unsigned op, void *p, size_t size, uint64_t generation) {
    return syscall(CLIPBOARD, op, p, size, generation);
}
static void read_exact(const char *value, size_t n) {
    memset(copy, 0x55, sizeof(copy));
    REQUIRE(clip(OS_CLIPBOARD_READ, copy, sizeof(copy), 0) == (long)n && !memcmp(copy, value, n));
}
int main(void) {
    REQUIRE(setpgid(0, getpgrp()) == 0);
    REQUIRE(clip(OS_CLIPBOARD_CLEAR, NULL, 0, 0) == 0);
    REQUIRE(clip(OS_CLIPBOARD_READ, NULL, 0, 0) == 0);
    struct os_clipboard_info info;
    REQUIRE(clip(OS_CLIPBOARD_INFO, &info, sizeof(info), 0) == 0 &&
            info.capacity == OS_CLIPBOARD_LIMIT && !info.bytes && info.generation);
    const char greeting[] = "one\n\xc2\xa3\xe2\x82\xac\xf0\x9f\x98\x80\tend\r\n";
    REQUIRE(clip(OS_CLIPBOARD_WRITE, (void *)greeting, sizeof(greeting) - 1, 0) == (long)sizeof(greeting) - 1);
    read_exact(greeting, sizeof(greeting) - 1);
    REQUIRE(clip(OS_CLIPBOARD_INFO, &info, sizeof(info), 0) == 0 &&
            info.bytes == (long)sizeof(greeting) - 1);
    errno = 0;
    REQUIRE(clip(OS_CLIPBOARD_READ, copy, 1, 0) == -1 && errno == ENOSPC);
    errno = 0;
    REQUIRE(clip(OS_CLIPBOARD_WRITE, (void *)1, 4, 0) == -1 && errno == EFAULT);
    errno = 0;
    REQUIRE(clip(OS_CLIPBOARD_READ, (void *)1, sizeof(copy), 0) == -1 && errno == EFAULT);
    const unsigned char bad[][5] = {
        {0}, {27}, {127}, {0xc0, 0x80}, {0xed, 0xa0, 0x80}, {0xf4, 0x90, 0x80, 0x80}, {0xc2}};
    const size_t sizes[] = {1, 1, 1, 2, 3, 4, 1};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
        errno = 0;
        REQUIRE(clip(OS_CLIPBOARD_WRITE, (void *)bad[i], sizes[i], 0) == -1 && errno == EINVAL);
        read_exact(greeting, sizeof(greeting) - 1);
    }
    errno = 0;
    REQUIRE(clip(OS_CLIPBOARD_WRITE, text, OS_CLIPBOARD_LIMIT + 1, 0) == -1 && errno == E2BIG);
    errno = 0;
    REQUIRE(clip(OS_CLIPBOARD_INFO, &info, 1, 0) == -1 && errno == EINVAL);
    REQUIRE(clip(OS_CLIPBOARD_WRITE, "next", 4, 0) == 4);
    memset(copy, 0x55, sizeof(copy));
    errno = 0;
    REQUIRE(clip(OS_CLIPBOARD_READ, copy, sizeof(copy), info.generation) == -1 && errno == EAGAIN);
    REQUIRE(copy[0] == 0x55);
    errno = 0;
    REQUIRE(clip(OS_CLIPBOARD_WRITE_IF_CURRENT, "stale", 5, info.generation) == -1 && errno == EAGAIN);
    read_exact("next", 4);
    REQUIRE(clip(OS_CLIPBOARD_INFO, &info, sizeof(info), 0) == 0);
    REQUIRE(clip(OS_CLIPBOARD_WRITE_IF_CURRENT, "current", 7, info.generation) == 7);
    read_exact("current", 7);
    errno = 0;
    REQUIRE(clip(OS_CLIPBOARD_WRITE_IF_CURRENT, "stale", 5, info.generation) == -1 && errno == EAGAIN);
    read_exact("current", 7);
    puts("CLIPBOARD CONDITIONAL WRITE PASS");
    puts("CLIPBOARD UTF8 BOUNDS SNAPSHOT ERRORS PASS");
    memset(text, 'L', sizeof(text));
    REQUIRE(clip(OS_CLIPBOARD_WRITE, text, sizeof(text), 0) == (long)sizeof(text));
    int channel[2];
    REQUIRE(pipe(channel) == 0);
    pid_t child = fork();
    REQUIRE(child >= 0);
    if (!child) {
        close(channel[0]);
        for (unsigned i = 0; i < 32; i++) {
            memset(text, i & 1 ? 'M' : 'N', sizeof(text));
            REQUIRE(clip(OS_CLIPBOARD_WRITE, text, sizeof(text), 0) == (long)sizeof(text));
        }
        REQUIRE(write(channel[1], "Y", 1) == 1);
        _exit(0);
    }
    close(channel[1]);
    for (unsigned i = 0; i < 64; i++) {
        REQUIRE(clip(OS_CLIPBOARD_READ, copy, sizeof(copy), 0) == (long)sizeof(copy));
        REQUIRE(copy[0] == 'L' || copy[0] == 'M' || copy[0] == 'N');
        for (size_t j = 1; j < sizeof(copy); j++)
            REQUIRE(copy[j] == copy[0]);
    }
    char done;
    REQUIRE(read(channel[0], &done, 1) == 1 && done == 'Y');
    close(channel[0]);
    int status;
    REQUIRE(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("CLIPBOARD CONCURRENT ATOMIC 64K PASS");
    REQUIRE(clip(OS_CLIPBOARD_CLEAR, NULL, 0, 0) == 0);
    puts("CLIPBOARD CHECK PASS");
    return 0;
}
