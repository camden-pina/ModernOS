#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "include/memory_usage.h"

extern char **environ;
#define CHECK(test) do { if (!(test)) { \
    printf("COMPILER ABI FAIL line=%d errno=%d: %s\n", __LINE__, errno, #test); \
    return 1; } } while (0)

static int execute(const char *path, char **args, char **env) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (!pid) {
        execve(path, args, env);
        _exit(errno);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int script(const char *path, const char *text) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0755);
    if (fd < 0)
        return -1;
    size_t size = strlen(text);
    int ok = write(fd, text, size) == (ssize_t)size;
    if (close(fd))
        ok = 0;
    return ok ? 0 : -1;
}

static int fragmentation(void) {
    enum { COUNT = 2500, PAGE = 4096 };
    void **mappings = calloc(COUNT, sizeof(*mappings));
    int fd = open("/tmp/compiler-fragmentation", O_CREAT | O_TRUNC | O_RDWR, 0600);
    CHECK(mappings && fd >= 0);
    unsigned char data[PAGE];
    /* Alternate user pages and persistent RAM file pages. Freeing the user
     * half used to overflow the physical allocator's 2048-extent table. */
    for (unsigned i = 0; i < COUNT; i++) {
        mappings[i] = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(mappings[i] != MAP_FAILED);
        memset(mappings[i], i % 251, PAGE);
        memset(data, i % 251, PAGE);
        CHECK(write(fd, data, PAGE) == PAGE);
    }

    for (unsigned i = 0; i < COUNT; i++)
        CHECK(!munmap(mappings[i], PAGE));
    void *replacement =
        mmap(NULL, COUNT * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(replacement != MAP_FAILED);
    memset(replacement, 0x5c, COUNT * PAGE);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    for (unsigned i = 0; i < COUNT; i++) {
        CHECK(read(fd, data, PAGE) == PAGE);
        for (unsigned j = 0; j < PAGE; j++)
            CHECK(data[j] == i % 251);
    }
    CHECK(!munmap(replacement, COUNT * PAGE) && !close(fd));
    CHECK(!unlink("/tmp/compiler-fragmentation"));
    free(mappings);
    puts("COMPILER MEMORY CHECK PASS: fragmented release/reuse preserves live file pages");
    return 0;
}

static long long cpu_microseconds(const struct rusage *usage) {
    return (usage->ru_utime.tv_sec + usage->ru_stime.tv_sec) * 1000000LL + usage->ru_utime.tv_usec +
           usage->ru_stime.tv_usec;
}

static int accounting(void) {
    struct rusage before, usage, after;
    struct timespec start, now, wall, deadline, resolution;
    struct tms ticks;
    struct os_memory_usage memory_before, memory_after, memory_released;
    CHECK(!syscall(MODERNOS_MEMORY_USAGE,&memory_before));
    CHECK(memory_before.mapped_bytes && memory_before.peak_mapped_bytes>=memory_before.mapped_bytes);
    void *memory =
        mmap(NULL, 4 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory!=MAP_FAILED && !syscall(MODERNOS_MEMORY_USAGE,&memory_after));
    CHECK(memory_after.mapped_bytes>=memory_before.mapped_bytes+4*1024*1024);
    CHECK(!munmap(memory,4*1024*1024) && !syscall(MODERNOS_MEMORY_USAGE,&memory_released));
    CHECK(memory_released.mapped_bytes+4*1024*1024<=memory_after.mapped_bytes);
    CHECK(memory_released.peak_mapped_bytes>=memory_after.peak_mapped_bytes);
    CHECK(!getrusage(RUSAGE_SELF,&usage) && (uint64_t)usage.ru_maxrss*1024==memory_released.peak_mapped_bytes);
    errno = 0;
    CHECK(syscall(MODERNOS_MEMORY_USAGE,(void *)1)==-1 && errno==EFAULT);
    CHECK(!getrusage(RUSAGE_CHILDREN, &before));
    CHECK(!clock_getres(CLOCK_PROCESS_CPUTIME_ID, &resolution) && resolution.tv_nsec == 1000000);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start));
        CHECK(!clock_gettime(CLOCK_MONOTONIC, &wall));
        volatile unsigned long work = 1;
        do {
            for (unsigned i = 0; i < 20000; i++)
                work = work * 1664525 + 1013904223;
            CHECK(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now));
            CHECK(!clock_gettime(CLOCK_MONOTONIC, &deadline));
            CHECK(deadline.tv_sec - wall.tv_sec < 15);
        } while ((now.tv_sec - start.tv_sec) * 1000000000LL + now.tv_nsec - start.tv_nsec <
                 20000000);
        CHECK(!getrusage(RUSAGE_SELF, &usage) && cpu_microseconds(&usage) >= 20000);
        CHECK(usage.ru_maxrss>0);
        CHECK(!getrusage(RUSAGE_THREAD, &after) && cpu_microseconds(&after) >= cpu_microseconds(&usage));
        _exit(23);
    }

    int status;
    CHECK(wait4(child, &status, 0, &usage) == child && WIFEXITED(status) && WEXITSTATUS(status) == 23);
    CHECK(cpu_microseconds(&usage) >= 20000);
    CHECK(usage.ru_maxrss>0);
    CHECK(!getrusage(RUSAGE_CHILDREN, &after));
    CHECK(cpu_microseconds(&after) - cpu_microseconds(&before) >= cpu_microseconds(&usage));
    CHECK(after.ru_maxrss>=usage.ru_maxrss);
    CHECK(times(&ticks) >= 0 && ticks.tms_cutime + ticks.tms_cstime >= cpu_microseconds(&after) / 10000 - 1);
    puts("COMPILER ACCOUNTING CHECK PASS: CPU clocks, getrusage, times and wait4 child accounting");
    return 0;
}

static int private_file_mapping(void) {
    char name[] = "/tmp/compiler-map-XXXXXX";
    int fd = mkstemp(name);
    CHECK(fd>=0 && write(fd,"PRIVATE_MAP",11)==11 && lseek(fd,5,SEEK_SET)==5);
    unsigned char *view = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(view!=MAP_FAILED && !memcmp(view,"PRIVATE_MAP",11));
    CHECK(lseek(fd,0,SEEK_CUR)==5);
    for (unsigned i = 11; i < 4096; i++)
        CHECK(!view[i]);
    view[0] = 'X';
    char first = 0;
    CHECK(pread(fd,&first,1,0)==1 && first=='P');
    CHECK(!munmap(view,4096));
    CHECK(mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,0)==MAP_FAILED && errno==ENOTSUP);
    CHECK(mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fd,1)==MAP_FAILED && errno==EINVAL);
    CHECK(mmap(NULL,8192,PROT_READ,MAP_PRIVATE,fd,0)==MAP_FAILED && errno==EINVAL);
    CHECK(mmap(NULL,4096,PROT_READ,MAP_PRIVATE,-1,0)==MAP_FAILED && errno==EBADF);
    view = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE, fd, 0);
    CHECK(view!=MAP_FAILED && !mprotect(view,4096,PROT_READ) && view[0]=='P');
    CHECK(!close(fd) && !unlink(name));
    CHECK(!memcmp(view,"PRIVATE_MAP",11) && !munmap(view,4096));
    puts(
        "COMPILER FILE MAP CHECK PASS: private file data, zero tail, preserved offsets, lifetime and explicit shared-map errors");
    return 0;
}

static int metadata_growth(void) {
    char root[] = "/tmp/compiler-heap-XXXXXX", first[256], second[256], name[256];
    CHECK(mkdtemp(root));
    for (unsigned a = 0; a < 32; a++) {
        snprintf(first, sizeof(first), "%s/%02u", root, a);
        CHECK(!mkdir(first,0700));
        for (unsigned b = 0; b < 64; b++) {
            snprintf(second, sizeof(second), "%s/%02u/%02u", root, a, b);
            CHECK(!mkdir(second,0700));
            for (unsigned c = 0; c < 160; c++) {
                snprintf(name, sizeof(name), "%s/%02u/%02u/%03u", root, a, b, c);
                int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0600);
                CHECK(fd>=0 && !close(fd));
            }
        }

        char *args[] = {"/bin/true", NULL};
        CHECK(execute(args[0],args,environ)==0);
    }

    /* 327,680 real inodes exceed the original fixed 128 MiB heap, without
       needing a page of file data per inode. Check process launches and
       allocated files before releasing the complete owned test tree. */
    for (unsigned a = 0; a < 32; a++) {
        for (unsigned b = 0; b < 64; b++) {
            for (unsigned c = 0; c < 160; c++) {
                snprintf(name, sizeof(name), "%s/%02u/%02u/%03u", root, a, b, c);
                struct stat info;
                CHECK(!stat(name,&info) && S_ISREG(info.st_mode) && !info.st_size);
                CHECK(!unlink(name));
            }

            snprintf(second, sizeof(second), "%s/%02u/%02u", root, a, b);
            CHECK(!rmdir(second));
        }

        snprintf(first, sizeof(first), "%s/%02u", root, a);
        CHECK(!rmdir(first));
    }

    CHECK(!rmdir(root));
    char *args[] = {"/bin/true", NULL};
    CHECK(execute(args[0],args,environ)==0);
    puts(
        "COMPILER HEAP CHECK PASS: 327680 filesystem inodes, heap growth, process launches and cleanup");
    return 0;
}

static int directory_index(void) {
    enum { COUNT = 8192 };
    char root[] = "/tmp/compiler-directory-XXXXXX", directory[256], destination[256], name[256],
         other[256];
    CHECK(mkdtemp(root));
    snprintf(directory, sizeof(directory), "%s/files", root);
    snprintf(destination, sizeof(destination), "%s/target", root);
    CHECK(!mkdir(directory, 0700) && !mkdir(destination, 0700));
    ino_t *inodes = calloc(COUNT, sizeof(*inodes));
    unsigned char *seen = calloc(COUNT * 2, 1);
    CHECK(inodes && seen);
    for (unsigned i = 0; i < COUNT; i++) {
        snprintf(name, sizeof(name), "%s/files/file-%04u", root, i);
        int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0600);
        struct stat info;
        CHECK(fd >= 0 && write(fd, &i, sizeof(i)) == sizeof(i) && !fstat(fd, &info) && !close(fd));
        inodes[i] = info.st_ino;
    }

    /* Every file is found out of insertion order, including hash collisions. */
    for (unsigned step = 0; step < COUNT; step++) {
        unsigned i = (step * 4051) % COUNT, value = COUNT;
        snprintf(name, sizeof(name), "%s/files/file-%04u", root, i);
        int fd = open(name, O_RDONLY);
        struct stat info;
        CHECK(fd >= 0 && read(fd, &value, sizeof(value)) == sizeof(value) && value == i);
        CHECK(!fstat(fd, &info) && info.st_ino == inodes[i] && !close(fd));
        CHECK(open(name, O_CREAT | O_EXCL | O_WRONLY, 0600) == -1 && errno == EEXIST);
    }

    /* Independent buffered streams and rewinds must not borrow one another's
       cached position or expose inode pointers as directory cookies. */
    DIR *streams[2] = {opendir(directory), opendir(directory)};
    CHECK(streams[0] && streams[1]);
    for (unsigned i = 0; i < COUNT; i++)
        for (unsigned stream = 0; stream < 2; stream++) {
            struct dirent *entry;
            do {
                entry = readdir(streams[stream]);
                CHECK(entry);
            } while (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."));
            unsigned value;
            char suffix;
            CHECK(sscanf(entry->d_name, "file-%u%c", &value, &suffix) == 1 && value < COUNT);
            CHECK(!seen[stream * COUNT + value] && entry->d_ino == inodes[value]);
            seen[stream * COUNT + value] = 1;
        }
    for (unsigned stream = 0; stream < 2; stream++) {
        errno = 0;
        CHECK(!readdir(streams[stream]) && !errno);
        rewinddir(streams[stream]);
        CHECK(readdir(streams[stream]));
        CHECK(!closedir(streams[stream]));
    }

    snprintf(name, sizeof(name), "%s/files/file-0003", root);
    snprintf(other, sizeof(other), "%s/files/renamed", root);
    CHECK(!rename(name, other));
    struct stat info;
    CHECK(stat(name, &info) == -1 && errno == ENOENT);
    CHECK(!stat(other, &info) && info.st_ino == inodes[3] && !rename(other, name));
    snprintf(name, sizeof(name), "%s/files/file-0011", root);
    snprintf(other, sizeof(other), "%s/target/moved", root);
    CHECK(!rename(name, other));
    CHECK(stat(name, &info) == -1 && errno == ENOENT);
    CHECK(!stat(other, &info) && info.st_ino == inodes[11]);
    snprintf(name, sizeof(name), "%s/files/file-0001", root);
    snprintf(other, sizeof(other), "%s/files/file-0002", root);
    CHECK(!rename(name, other));
    CHECK(stat(name, &info) == -1 && errno == ENOENT);
    CHECK(!stat(other, &info) && info.st_ino == inodes[1]);
    snprintf(name, sizeof(name), "%s/files/link", root);
    snprintf(other, sizeof(other), "%s/target/link", root);
    CHECK(!symlink("file-0000", name) && !rename(name, other));
    char link[32];
    CHECK(readlink(other, link, sizeof(link)) == 9 && !memcmp(link, "file-0000", 9));
    CHECK(!unlink(other));
    streams[0] = opendir(directory);
    CHECK(streams[0]);
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(streams[0]))) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            count++;
    }
    CHECK(count == COUNT - 2 && !closedir(streams[0]));
    for (unsigned i = 0; i < COUNT; i++) {
        if (i == 1 || i == 11)
            continue;
        snprintf(name, sizeof(name), "%s/files/file-%04u", root, i);
        CHECK(!unlink(name));
    }

    snprintf(name, sizeof(name), "%s/target/moved", root);
    CHECK(!unlink(name));
    CHECK(!rmdir(directory) && !rmdir(destination) && !rmdir(root));
    free(inodes);
    free(seen);
    puts(
        "COMPILER DIRECTORY CHECK PASS: 8192 real files, lookup/collisions, independent streams, rewinds, rename/replacement, symlinks and cleanup");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--metadata"))
        return metadata_growth();
    if (argc == 2 && !strcmp(argv[1], "--directory"))
        return directory_index();
    if (argc > 1 && !strcmp(argv[1], "--arguments")) {
        CHECK(argc == 1503);
        CHECK(strlen(argv[1502]) == 16384);
        for (int i = 2; i < 1502; i++)
            CHECK(!strcmp(argv[i], "object-file-name.o"));
        CHECK(getenv("LONG_VALUE") && strlen(getenv("LONG_VALUE")) == 8192);
        CHECK(getenv("EMPTY") && !*getenv("EMPTY"));
        return 0;
    }

    if (argc > 1 && !strcmp(argv[1], "--boundary")) {
        CHECK(argc == 3 && strlen(argv[2]) == 4095);
        return 0;
    }
    CHECK(argc == 1);
    CHECK(!fragmentation());
    CHECK(!private_file_mapping());
    CHECK(!accounting());
    struct rlimit limit;
    CHECK(!getrlimit(RLIMIT_NOFILE, &limit) && limit.rlim_cur >= 128);
    CHECK(!getrlimit(RLIMIT_STACK, &limit) && limit.rlim_cur >= 8 * 1024 * 1024);

    char **args = calloc(4098, sizeof(*args));
    char *long_arg = malloc(32769), *long_env = malloc(8210);
    CHECK(args && long_arg && long_env);
    memset(long_arg, 'A', 32768);
    long_arg[16384] = 0;
    memcpy(long_env, "LONG_VALUE=", 11);
    memset(long_env + 11, 'E', 8192);
    long_env[8203] = 0;
    char *env[] = {"PATH=/usr/bin:/bin", long_env, "EMPTY=", NULL};
    args[0] = "/bin/compiler-check";
    args[1] = "--arguments";
    for (int i = 2; i < 1502; i++)
        args[i] = "object-file-name.o";
    args[1502] = long_arg;
    CHECK(execute(args[0], args, env) == 0);

    /* GNU's recursive build machinery exports several hundred configure
     * variables. The previous 128-entry cap rejected real make recipes. */
    char **many_env = calloc(514, sizeof(*many_env));
    CHECK(many_env);
    for (int i = 0; i < 3; i++)
        many_env[i] = env[i];
    for (int i = 3; i < 513; i++) {
        many_env[i] = malloc(32);
        CHECK(many_env[i]);
        snprintf(many_env[i], 32, "BUILD_OPTION_%d=value", i);
    }
    char *saved_env = many_env[300];
    many_env[300] = NULL;
    CHECK(execute(args[0], args, many_env) == 0);
    many_env[300] = saved_env;
    CHECK(execute(args[0], args, many_env) == E2BIG);
    for (int i = 3; i < 513; i++)
        free(many_env[i]);
    free(many_env);

    for (int i = 1; i < 4097; i++)
        args[i] = "o";
    args[4097] = NULL;
    CHECK(execute(args[0], args, env) == E2BIG);
    args[1] = long_arg;
    args[2] = NULL;
    memset(long_arg, 'A', 32768);
    long_arg[32768] = 0;
    CHECK(execute(args[0], args, env) == E2BIG);
    long_arg[32767] = 0;
    for (int i = 1; i <= 5; i++)
        args[i] = long_arg;
    args[6] = NULL;
    CHECK(execute(args[0], args, env) == E2BIG);

    char *guard = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(guard != MAP_FAILED && !mprotect(guard + 4096, 4096, PROT_NONE));
    memset(guard, 'B', 4096);
    guard[4095] = 0;
    args[1] = "--boundary";
    args[2] = guard;
    args[3] = NULL;
    CHECK(execute(args[0], args, env) == 0);
    guard[4095] = 'B';
    CHECK(execute(args[0], args, env) == EFAULT);
    CHECK(!munmap(guard, 8192));
    free(args);
    free(long_arg);
    puts(
        "COMPILER ARGUMENT CHECK PASS: large argv/env, aggregate limits and page-boundary strings");

    int fd = open("/dev/null", O_RDWR), high;
    CHECK(fd >= 0 && (high = fcntl(fd, F_DUPFD_CLOEXEC, 70)) == 70);
    fd_set reads, writes;
    FD_ZERO(&reads);
    FD_ZERO(&writes);
    FD_SET(high, &reads);
    FD_SET(high, &writes);
    struct timeval zero = {0};
    CHECK(select(high + 1, &reads, &writes, NULL, &zero) == 2);
    CHECK(FD_ISSET(high, &reads) && FD_ISSET(high, &writes));
    FD_ZERO(&reads);
    FD_SET(high, &reads);
    struct timespec timeout = {0};
    CHECK(pselect(high + 1, &reads, NULL, NULL, &timeout, NULL) == 1 && FD_ISSET(high, &reads));
    CHECK(!close(high) && !close(fd));
    puts("COMPILER DESCRIPTOR CHECK PASS: select/pselect beyond descriptor 63");

    CHECK(!script("/tmp/compiler-script", "#!/bin/sh -e\r\nprintf SCRIPT_EXEC_OK > /tmp/compiler-script-result\nexit 23\n"));
    char *script_args[] = {"/tmp/compiler-script", NULL};
    CHECK(execute(script_args[0], script_args, env) == 23);
    fd = open("/tmp/compiler-script-result", O_RDONLY);
    char result[32] = {0};
    CHECK(fd >= 0 && read(fd, result, sizeof(result)) == 14 && !strcmp(result, "SCRIPT_EXEC_OK"));
    CHECK(!close(fd));
    CHECK(!script("/tmp/compiler-script", "#!/tmp/compiler-script\n"));
    CHECK(execute(script_args[0], script_args, env) == ELOOP);
    CHECK(!script("/tmp/compiler-script", "#!relative-interpreter\n"));
    CHECK(execute(script_args[0], script_args, env) == ENOEXEC);
    CHECK(!script("/tmp/compiler-script", "#!/tmp/no-such-interpreter\n"));
    CHECK(execute(script_args[0], script_args, env) == ENOENT);
    CHECK(!unlink("/tmp/compiler-script") && !unlink("/tmp/compiler-script-result"));
    free(long_env);
    puts(
        "COMPILER ABI CHECK PASS: executable scripts, real errors, arguments, environments and descriptors");
    return 0;
}
