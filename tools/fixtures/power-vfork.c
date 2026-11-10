/* Disposable stress: vfork parent retains its syscall transaction while the
 * dependent child sleeps across freeze. Child uses no C calls/shared stack. */
#include <libc.h>
extern long vfork_wait_child(void);
__asm__(".text\n"
        ".type vfork_wait_child,@function\n"
        "vfork_wait_child:\n"
        "mov $58,%eax; syscall; test %rax,%rax; jnz 1f\n"
        "mov $1,%eax; mov $1,%edi; lea 2f(%rip),%rsi; mov $23,%edx; syscall\n"
        "mov $35,%eax; lea 3f(%rip),%rdi; xor %esi,%esi; syscall\n"
        "mov $60,%eax; xor %edi,%edi; syscall; ud2\n"
        "1: ret\n"
        ".size vfork_wait_child,.-vfork_wait_child\n"
        ".section .rodata\n"
        "2: .ascii \"POWER VFORK CHILD WAIT\\n\"\n"
        ".balign 8\n"
        "3: .quad 3,0\n"
        ".text\n");
int main(void) {
    long child = vfork_wait_child();
    int status = -1;
    if (child <= 0 || waitpid(child, &status, 0) != child || status) {
        printf("POWER VFORK FAIL child=%ld status=%d\n", child, status);
        return 1;
    }
    puts("POWER VFORK PARENT RESUMED");
    return 0;
}
