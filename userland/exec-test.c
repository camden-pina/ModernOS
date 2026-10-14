#include <libc.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char *args[] = {"/bin/hello", "execve", NULL};
    long e = call3(SYS_execve, args[0], args, 0);
    printf("execve failed: %ld\n", e);
    return 1;
}
