#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static _Thread_local unsigned tls;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static unsigned ready, release_workers, completed, retries;
static char executable[4096];
static volatile sig_atomic_t signalled;

static void fail(const char *message) {
    fprintf(stderr, "GLIBC_CHECK_FAIL %s: %s\n", message, strerror(errno));
    exit(1);
}

static void signal_handler(int number) { signalled = number; }

static void *worker(void *argument) {
    unsigned id = (unsigned)(uintptr_t)argument;
    tls = 0xc000 + id;
    pthread_mutex_lock(&lock);
    ++ready;
    pthread_cond_broadcast(&condition);
    while (!release_workers) pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < 6; ++i) {
        char parent[32];
        snprintf(parent, sizeof(parent), "%ld", (long)getpid());
        char *arguments[] = {executable, "--child", parent, NULL};
        pid_t child;
        unsigned attempt = 0;
        int error;
        do {
            error = posix_spawn(&child, executable, NULL, NULL, arguments, environ);
            if (error != EAGAIN) break;
            __atomic_add_fetch(&retries, 1, __ATOMIC_RELAXED);
            usleep(10000);
        } while (++attempt < 3000);
        if (error) { errno = error; fail("threaded posix_spawn"); }
        int status = -1;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
            fail("spawned child status or parent identity");
        if (tls != 0xc000 + id) fail("parent TLS changed after spawn");
    }
    pthread_mutex_lock(&lock);
    ++completed;
    pthread_mutex_unlock(&lock);
    return (void *)(uintptr_t)tls;
}

int main(int count, char **arguments) {
    if (count == 3 && !strcmp(arguments[1], "--child"))
        return getppid() == strtol(arguments[2], NULL, 10) && tls == 0 ? 0 : 2;
    if (count != 1) return 2;
    if (!realpath(arguments[0], executable)) fail("executable path");
    tls = 0x12345678;
    pthread_t workers[4];
    for (unsigned i = 0; i < 4; ++i) {
        int error = pthread_create(&workers[i], NULL, worker, (void *)(uintptr_t)i);
        if (error) { errno = error; fail("pthread_create"); }
    }
    pthread_mutex_lock(&lock);
    while (ready != 4) pthread_cond_wait(&condition, &lock);
    release_workers = 1;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < 4; ++i) {
        void *result;
        if (pthread_join(workers[i], &result) || (uintptr_t)result != 0xc000 + i)
            fail("pthread join or thread-local value");
    }
    if (completed != 4 || tls != 0x12345678) fail("thread completion or initial TLS");
    printf("GLIBC_THREADS_PASS workers=4 children=24 retries=%u\n", retries);

    size_t size = 64 * 1024 * 1024;
    unsigned char *memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) fail("mmap");
    for (size_t i = 0; i < size; i += 4096) memory[i] = (unsigned char)(i / 4096);
    for (size_t i = 0; i < size; i += 4096)
        if (memory[i] != (unsigned char)(i / 4096)) fail("anonymous mapping bytes");
    if (mprotect(memory, size, PROT_READ) || munmap(memory, size)) fail("mprotect/munmap");
    void *allocation = calloc(32768, 16);
    if (!allocation) fail("calloc");
    for (size_t i = 0; i < 32768 * 16; ++i)
        if (((unsigned char *)allocation)[i]) fail("calloc zero bytes");
    free(allocation);
    puts("GLIBC_MEMORY_PASS");

    void *library = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); fail("dlopen"); }
    double (*cosine)(double) = (double (*)(double))dlsym(library, "cos");
    if (!cosine || fabs(cosine(0.0) - 1.0) > 1e-12 || dlclose(library)) fail("dlsym cosine");
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *addresses;
    int error = getaddrinfo("localhost", "80", &hints, &addresses);
    if (error) { fprintf(stderr, "%s\n", gai_strerror(error)); fail("NSS localhost lookup"); }
    freeaddrinfo(addresses);
    struct sigaction action = {.sa_handler = signal_handler};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) || raise(SIGUSR1) || signalled != SIGUSR1)
        fail("signal handler");
    puts("GLIBC_LOADER_NSS_SIGNALS_PASS");
    puts("GLIBC_ALL_PASS");
    return 0;
}
