#include <libc.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    puts("Attempting a supervisor-memory write from ring 3");
    *(volatile uint64_t *)0xffffffff80000000ULL = 0;
    return 1;
}
