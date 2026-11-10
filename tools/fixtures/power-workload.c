/* Disposable guest power fixture. Leaves a completed FAT write open/dirty. */
#include <libc.h>
#define CHECK(x) do { if (!(x)) { printf("POWER WORKLOAD FAIL line=%d\n", __LINE__); return 1; } } while (0)
static uint64_t fs_cookie = UINT64_C(0x27e91a84fe7705c3);
static const uint64_t fpu_cookie = UINT64_C(0x913cab77ef2305d1);
static char status[8192];

/* A separate read-only mode for the reboot recovery check. No power or
 * architecture syscalls belong here: reopening must not rewrite the evidence. */
static int verify_file(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        puts("POWER WORKLOAD FAIL verify open");
        return 1;
    }
    unsigned char bytes[4096];
    unsigned offset = 0;
    int result = 1;
    while (offset < 131072) {
        size_t want = 131072 - offset;
        if (want > sizeof(bytes)) want = sizeof(bytes);
        long n = read(fd, bytes, want);
        if (n <= 0 || (size_t)n > want) {
            puts("POWER WORKLOAD FAIL verify short file or read error");
            goto done;
        }
        for (unsigned i = 0; i < (unsigned)n; i++) {
            if (bytes[i] != (unsigned char)(((offset + i) * 31 + 17) & 255)) {
                printf("POWER WORKLOAD FAIL verify byte=%u\n", offset + i);
                goto done;
            }
        }
        offset += (unsigned)n;
    }
    if (read(fd, bytes, 1) != 0) {
        puts("POWER WORKLOAD FAIL verify extra bytes or read error");
        goto done;
    }
    result = 0;
done:
    if (close(fd)) {
        puts("POWER WORKLOAD FAIL verify close");
        result = 1;
    }
    if (!result) puts("POWER WORKLOAD VERIFIED bytes=131072 eof=1");
    return result;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--verify-capability")) {
        puts("POWER WORKLOAD CAPABILITY verify=1 bytes=131072 eof=1");
        return 0;
    }
    CHECK(argc == 3);
    if (!strcmp(argv[2], "verify")) return verify_file(argv[1]);
    unsigned action = !strcmp(argv[2], "reboot") ? OS_POWER_RESET : OS_POWER_OFF;
    int fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    char bytes[4096];
    for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (i * 31 + 17) & 255;
    for (unsigned i = 0; i < 32; i++) CHECK(write_all(fd, bytes, sizeof(bytes)) == 0);
    CHECK(!call2(SYS_arch_prctl, 0x1002, &fs_cookie));
    __asm__ volatile("movq %0, %%xmm15" ::"m"(fpu_cookie) : "xmm15");
    CHECK(!call2(SYS_power, action, 0));
    CHECK(!call2(SYS_power, action, 0));
    CHECK(call2(SYS_power, action == 1 ? 2 : 1, 0) == -E_BUSY);
    printf("POWER WORKLOAD ACCEPTED action=%u open_bytes=131072 duplicate=coalesced conflicting=busy\n", action);
    if (!strcmp(argv[2], "cancel")) CHECK(!call2(SYS_power, OS_POWER_CANCEL, 0));
    for (unsigned i = 0; i < 160; i++) {
        struct os_timespec delay = {0, 100000000};
        CHECK(!call2(SYS_nanosleep, &delay, 0));
        long n = call3(SYS_sysinfo, INFO_POWER, status, sizeof(status));
        CHECK(n > 0 && n < (long)sizeof(status)); status[n] = 0;
        uint64_t fs, xmm;
        __asm__ volatile("movq %%fs:0, %0; movq %%xmm15, %1" : "=r"(fs), "=r"(xmm));
        CHECK(fs == fs_cookie && xmm == fpu_cookie);
        if (!strncmp(status, "power_state=failed\n", 19) ||
            (!strcmp(argv[2], "cancel") && !strncmp(status, "power_state=idle\n", 17))) {
            CHECK(lseek(fd, 0, 0) == 0);
            char readback[4096]; CHECK(read(fd, readback, sizeof(readback)) == sizeof(readback));
            CHECK(!memcmp(readback, bytes, sizeof(bytes)));
            CHECK(!close(fd));
            printf("POWER WORKLOAD RECOVERED FSBASE/FPU/readback intact\n%s", status);
            return 0;
        }
    }
    puts("POWER WORKLOAD FAIL no terminal event or recovered state");
    return 1;
}
