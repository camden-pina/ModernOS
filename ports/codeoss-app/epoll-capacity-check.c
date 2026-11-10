#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static unsigned checks;
#define OK(c) do { if (!(c)) { fprintf(stderr, "EPOLL_CAPACITY_FAIL line=%d errno=%d: %s\n", __LINE__, errno, #c); exit(1); } checks++; } while (0)

int main(int argc, char **argv) {
    struct utsname os;
    if (uname(&os) || strcmp(os.sysname, "Coast")) return 2;
    int baseline = argc > 1 && !strcmp(argv[1], "--baseline");
    int ep = epoll_create1(EPOLL_CLOEXEC), fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    OK(ep >= 0 && fd >= 0);
    struct epoll_event interest = {.events = EPOLLIN, .data.u64 = 0xdecafbad12345678ULL};
    struct epoll_event output[1025];
    memset(output, 0x6b, sizeof(output));
    OK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &interest) == 0);
    uint64_t value = 1;
    OK(write(fd, &value, sizeof(value)) == sizeof(value));
    if (baseline) {
        errno = 0;
        OK(epoll_wait(ep, output, 1024, 0) == -1 && errno == EINVAL);
        puts("EPOLL_CAPACITY_BASELINE_PASS maxevents=1024 rejected EINVAL");
        close(fd); close(ep); return 0;
    }
    for (unsigned i = 0; i < 4; i++) {
        int capacity = (int[]){1, 128, 1024, 4096}[i];
        memset(output, 0x6b, sizeof(output));
        /* Kernel emits at most 128 records, even when a larger capacity is
           advertised. This array is sufficient for every possible result. */
        OK(syscall(SYS_epoll_wait, ep, output, capacity, 0) == 1);
        OK(output[0].events == EPOLLIN && output[0].data.u64 == interest.data.u64);
        const unsigned char *tail = (const unsigned char *)(output + 1);
        for (size_t n = 0; n < sizeof(output) - sizeof(*output); n++) OK(tail[n] == 0x6b);
    }
    sigset_t mask;
    sigemptyset(&mask);
    OK(epoll_pwait(ep, output, 1024, 0, &mask) == 1);
    OK(read(fd, &value, sizeof(value)) == sizeof(value) && value == 1);
    OK(epoll_wait(ep, output, 1024, 0) == 0);
    errno = 0; OK(syscall(SYS_epoll_wait, ep, output, 0, 0) == -1 && errno == EINVAL);
    errno = 0; OK(syscall(SYS_epoll_wait, ep, output, -1, 0) == -1 && errno == EINVAL);
    errno = 0; OK(syscall(SYS_epoll_wait, ep, output, 0x7fffffff, 0) == -1 && errno == EINVAL);
    errno = 0; OK(syscall(SYS_epoll_wait, ep, output, 1024, -2) == -1 && errno == EINVAL);
    errno = 0; OK(syscall(SYS_epoll_pwait, ep, output, 1024, 0, &mask, 16) == -1 && errno == EINVAL);
    void *guard = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    OK(guard != MAP_FAILED);
    errno = 0; OK(syscall(SYS_epoll_wait, ep, guard, 1024, 0) == -1 && errno == EFAULT);
    OK(munmap(guard, 4096) == 0);
    /* Existing unsupported edge triggering stays rejected. */
    interest.events = EPOLLIN | EPOLLET;
    errno = 0; OK(epoll_ctl(ep, EPOLL_CTL_MOD, fd, &interest) == -1 && errno == ENOTSUP);
    OK(close(fd) == 0 && close(ep) == 0);
    printf("EPOLL_CAPACITY_PASS checks=%u\n", checks);
    return 0;
}
