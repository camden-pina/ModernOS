/* Exercise real multi-GiB native mappings and build artifacts on Coast. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "LARGE_BUILD_FAIL line=%d errno=%d: %s\n", __LINE__, errno, #x); return 1; } } while (0)
#define MiB (1024ULL * 1024)
#define GiB (1024ULL * MiB)

int main(void) {
    struct rlimit limit;
    REQUIRE(!getrlimit(RLIMIT_DATA, &limit) && limit.rlim_cur >= 4 * GiB);
    const size_t size = GiB + 4096;
    unsigned char *memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(memory != MAP_FAILED);
    const size_t points[] = {0, 512 * MiB, GiB, size - 1};
    for (unsigned i = 0; i < sizeof(points) / sizeof(*points); i++) {
        REQUIRE(memory[points[i]] == 0);
        memory[points[i]] = (unsigned char)(0xa0 + i);
    }
    errno = 0;
    REQUIRE(mmap(NULL, limit.rlim_cur + 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL);
    errno = 0;
    REQUIRE(mprotect(memory, size, PROT_READ | PROT_WRITE | PROT_EXEC) == -1 && errno == EINVAL);
    for (unsigned i = 0; i < sizeof(points) / sizeof(*points); i++)
        REQUIRE(memory[points[i]] == (unsigned char)(0xa0 + i));
    REQUIRE(!mprotect(memory, size, PROT_READ));
    REQUIRE(!mprotect(memory, size, PROT_READ | PROT_WRITE));
    REQUIRE(!madvise(memory, size, MADV_DONTNEED));
    for (unsigned i = 0; i < sizeof(points) / sizeof(*points); i++)
        REQUIRE(memory[points[i]] == 0);
    unsigned char *guard = mmap(memory + size, 4096, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(guard == memory + size);
    guard[0] = 0x7d;
    errno = 0;
    REQUIRE(mremap(memory, size, size + 4096, 0) == MAP_FAILED && errno == ENOMEM);
    REQUIRE(guard[0] == 0x7d && memory[0] == 0);
    REQUIRE(!munmap(guard, 4096));
    const size_t smaller = 768 * MiB + 4096;
    REQUIRE(mremap(memory, size, smaller, 0) == memory);
    REQUIRE(mremap(memory, smaller, size, 0) == memory);
    REQUIRE(memory[size - 1] == 0);
    REQUIRE(!munmap(memory, size));
    puts("LARGE_MAPPING_PASS 1GiB backing, protections, W^X rejection, discard/refault, remap collision and rollback");

    const char *path = "/tmp/coast-large-artifact";
    int file = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    REQUIRE(file >= 0);
    const off_t length = 4 * GiB + 4096;
    REQUIRE(!ftruncate(file, length));
    const off_t positions[] = {256 * MiB - 8, 2 * GiB - 8, 4 * GiB - 8, length - 8};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(*positions); i++) {
        uint64_t value = UINT64_C(0x434f415354000000) + i, readback = UINT64_MAX;
        REQUIRE(pread(file, &readback, sizeof(readback), positions[i]) == sizeof(readback) && !readback);
        REQUIRE(pwrite(file, &value, sizeof(value), positions[i]) == sizeof(value));
        REQUIRE(pread(file, &readback, sizeof(readback), positions[i]) == sizeof(readback) && readback == value);
    }
    struct stat status;
    REQUIRE(!fstat(file, &status) && status.st_size == length);
    errno = 0;
    REQUIRE(ftruncate(file, 8 * GiB + 1) == -1 && errno == EFBIG);
    REQUIRE(!fstat(file, &status) && status.st_size == length);
    REQUIRE(!ftruncate(file, 0));
    REQUIRE(!fstat(file, &status) && !status.st_size);
    REQUIRE(!close(file) && !unlink(path));
    puts("LARGE_FILE_PASS real 4GiB+4096 RAMFS artifact, zero holes, positional I/O across 32-bit offsets, limit rejection and reclaim");
    puts("LARGE_BUILD_ALL_PASS");
    return 0;
}
