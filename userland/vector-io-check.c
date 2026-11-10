#include <libc.h>
#define CHECK(x) do { if (!(x)) { printf("VECTOR IO FAIL line=%d %s\n", __LINE__, #x); return 1; } } while (0)
static long vector(unsigned operation, int fd, struct os_iovec *v, unsigned count, int64_t offset) {
    return syscall6(operation, fd, (long)v, count, offset, 0, 0);
}
int main(void) {
    const char *path = "/tmp/vector-io-check";
    call1(SYS_unlink, path);
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && write(fd, "0123456789", 10) == 10);
    CHECK(lseek(fd, 3, 0) == 3);
    struct os_iovec v[2] = {{(uintptr_t)"HEAD", 4}, {(uintptr_t)"TAIL", 4}};
    CHECK(vector(SYS_pwritev, fd, v, 2, 1) == 8);
    CHECK(lseek(fd, 0, 1) == 3);
    CHECK(syscall6(SYS_fallocate,fd,0,0,4096,0,0)==-E_NOTSUP);
    CHECK(syscall6(SYS_fallocate,fd,0,-1,4096,0,0)==-E_INVAL);
    CHECK(syscall6(SYS_fallocate,fd,0,0,0,0,0)==-E_INVAL);
    CHECK(syscall6(SYS_fallocate,fd,0,INT64_MAX,1,0,0)==-E_INVAL);
    CHECK(syscall6(SYS_fallocate,-1,0,0,4096,0,0)==-E_BADF);
    char first[4], second[8];
    v[0] = (struct os_iovec){(uintptr_t)first, 4};
    v[1] = (struct os_iovec){(uintptr_t)second, 8};
    CHECK(vector(SYS_preadv, fd, v, 2, 4) == 6);
    CHECK(!memcmp(first, "DTAI", 4) && !memcmp(second, "L9", 2));
    CHECK(lseek(fd, 0, 1) == 3);
    CHECK(vector(SYS_preadv, fd, v, 0, 0) == 0);
    CHECK(vector(SYS_pwritev, fd, v, 17, 0) == -E_INVAL);
    CHECK(vector(SYS_pwritev, fd, v, 2, -1) == -E_INVAL);
    CHECK(vector(SYS_pwritev, fd, v, 2, INT64_MAX - 1) == -E_INVAL);
    v[1] = (struct os_iovec){0xffff800000000000ULL, 8};
    CHECK(vector(SYS_pwritev, fd, v, 2, 0) == -E_FAULT);
    v[1].len = 16 * 1024 * 1024;
    CHECK(vector(SYS_pwritev, fd, v, 2, 0) == -E_2BIG);
    char bytes[10];
    CHECK(syscall6(SYS_pread64, fd, (long)bytes, 10, 0, 0, 0) == 10 && !memcmp(bytes, "0HEADTAIL9", 10));
    int input = open(path, O_RDONLY, 0);
    CHECK(input >= 0 && vector(SYS_pwritev, input, v, 1, 0) == -E_BADF);
    CHECK(syscall6(SYS_fallocate,input,0,0,4096,0,0)==-E_BADF);
    close(input);
    int pipe[2]; CHECK(!call2(SYS_pipe2, pipe, 0));
    CHECK(vector(SYS_preadv, pipe[0], v, 1, 0) == -E_SPIPE);
    CHECK(syscall6(SYS_fallocate,pipe[1],0,0,4096,0,0)==-E_SPIPE);
    close(pipe[0]); close(pipe[1]); close(fd); call1(SYS_unlink, path);
    int memory=call2(SYS_memfd_create,"cursor-pool",3);
    CHECK(memory>=0 && syscall6(SYS_fallocate,memory,0,0,4096,0,0)==-E_NOTSUP);
    CHECK(!call2(SYS_ftruncate,memory,4096));
    CHECK(lseek(memory,4095,0)==4095 && write(memory,"x",1)==1);
    CHECK(!call2(SYS_ftruncate,memory,8192));close(memory);
    puts("CURSOR SHM PASS unsupported extent reservation permits ftruncate fallback and pool growth");
    puts("VECTOR IO PASS offset-preservation/vectors/short-EOF/permissions/bounds/fault-before-write/pipe");
    return 0;
}
