#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <asm/unistd.h>
struct entry {
    const char *name;
    long number;
};
int main(void) {
    static const struct entry calls[] = {{"sendfile", __NR_sendfile},
                                         {"flock", __NR_flock},
                                         {"link", __NR_link},
                                         {"linkat", __NR_linkat},
                                         {"statfs", __NR_statfs},
                                         {"fstatfs", __NR_fstatfs},
                                         {"fchmodat", __NR_fchmodat},
                                         {"fchmodat2", __NR_fchmodat2},
                                         {"faccessat2", __NR_faccessat2},
                                         {"statx", __NR_statx},
                                         {"clock_nanosleep", __NR_clock_nanosleep},
                                         {"close_range", __NR_close_range},
                                         {"copy_file_range", __NR_copy_file_range},
                                         {"preadv", __NR_preadv},
                                         {"pwritev", __NR_pwritev},
                                         {"preadv2", __NR_preadv2},
                                         {"pwritev2", __NR_pwritev2},
                                         {"chown", __NR_chown},
                                         {"fchown", __NR_fchown},
                                         {"lchown", __NR_lchown},
                                         {"fchownat", __NR_fchownat},
                                         {"mknod", __NR_mknod},
                                         {"mknodat", __NR_mknodat}};
    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        errno = 0;
        long n = syscall(calls[i].number, -1L, 0L, 0L, 0L, 0L, 0L);
        printf("LINUX_ABI_AUDIT name=%s number=%ld result=%ld errno=%d\n", calls[i].name,
               calls[i].number, n, errno);
    }
    return 0;
}
