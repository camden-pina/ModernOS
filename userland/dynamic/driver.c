#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROOT "/usr/lib/coast/dynamic/"
#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "DYNAMIC DRIVER FAIL line %d: %s (errno=%d)\n", __LINE__, #test, errno); \
    exit(1); } } while (0)

static void rejected(const char *path, int expected) {
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        char *args[] = {(char *)path, NULL};
        execv(path, args);
        _exit(errno == expected ? 0 : 2);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void executed_as(const char *path, const char *argzero, int expected) {
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        char *args[] = {(char *)argzero, NULL};
        execv(path, args);
        _exit(120);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == expected);
}

static void executed(const char *path, int expected) { executed_as(path, path, expected); }

int main(void) {
    rejected(ROOT "missing-interpreter", ENOENT);
    rejected(ROOT "bad-interpreter", ENOEXEC);
    rejected(ROOT "bad-wx", ENOEXEC);
    rejected(ROOT "bad-stack", ENOEXEC);
    puts("PASS exec rejects missing/nested interpreters, writable text and executable stacks");
    executed(ROOT "missing-library", 127);
    puts("PASS startup dependency failure exits with a real loader error");
    executed(ROOT "dynamic-transitive", 0);
    executed_as(ROOT "dynamic-transitive", "unrelated-argv0", 0);
    executed("/usr/bin/dynamic-transitive-link", 0);
    executed(ROOT "dynamic-check", 0);
    executed(ROOT "dynamic-exec-check", 0);
    puts("DYNAMIC DRIVER CHECK PASS");
    return 0;
}
