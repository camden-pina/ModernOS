#include <libc.h>

static unsigned long bss[64];

int main(int argc, char **argv) {
    unsigned cs;
    __asm__ volatile("mov %%cs,%0" : "=r"(cs));
    if ((cs & 3) != 3 || bss[0] || bss[63])
        return 1;
    printf("Hello from ring 3! pid=%ld argc=%d\n", call0(SYS_getpid), argc);
    for (int i = 0; i < argc; i++)
        printf("argv[%d]=%s\n", i, argv[i]);
    return 0;
}
