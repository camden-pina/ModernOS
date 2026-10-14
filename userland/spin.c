#include <libc.h>

int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))) {
        puts("Usage: spin [ITERATIONS]\nITERATIONS must be a nonnegative decimal integer.");
        return argc > 2 ? 2 : 0;
    }
    long count = argc > 1 ? number(argv[1], 10) : 200000000;
    if (count < 0) {
        puts("spin: ITERATIONS must be a nonnegative decimal integer.\nUsage: spin [ITERATIONS]");
        return 2;
    }
    printf("spin pid=%ld started\n", call0(SYS_getpid));
    volatile uint64_t value = 0;
    /* Deliberately no yield, sleep, or syscall inside this loop. */
    for (long i = 0; i < count; i++)
        value += (uint64_t)i ^ (value >> 3);
    printf("spin pid=%ld finished value=%lu\n", call0(SYS_getpid), (unsigned long)value);
    return 0;
}
