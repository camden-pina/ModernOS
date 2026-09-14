/* Constant definitions */
.equ NULL, 0
.equ KERNEL_OFFSET, 0xFFFFFF8000000000
.equ KERNEL_STACK, 0xFFFFFFA000000000
.equ KERNEL_CS, 0x08

.equ PAGE_SIZE, 0x1000

.equ APIC_BASE_VA, 0xFFFFFFFFE0000000
.equ APIC_REG_ID, 0x20
.equ APIC_REG_EOI, 0x0B0

.equ IA32_EFER_MSR, 0x0C0000080
.equ FS_BASE_MSR, 0xC0000100
.equ GS_BASE_MSR, 0xC0000101
.equ KERNEL_GS_BASE_MSR, 0xC0000102

.equ COM1, 0x3F8

/* percpu offsets */
.equ PERCPU_SELF,      0x00
.equ PERCPU_ID,        0x08
.equ PERCPU_APIC_ID,   0x0A
.equ PERCPU_THREAD,    0x10
.equ PERCPU_PROCESS,   0x18
.equ PERCPU_KERNEL_SP, 0x20
.equ PERCPU_USER_SP,   0x28
.equ PERCPU_RFLAGS,    0x30

/* The following are used with a %gs: override in memory references */
.equ CURRENT_THREAD, PERCPU_THREAD
.equ CURRENT_PROCESS, PERCPU_PROCESS
.equ KERNEL_SP, PERCPU_KERNEL_SP
.equ USER_SP, PERCPU_USER_SP

/* process offsets */
.equ PROCESS_PID,  0x00
.equ PROCESS_PPID, 0x04
.equ PROCESS_VM,   0x08

/* thread offsets */
.equ THREAD_ID,        0x00
.equ THREAD_CTX,       0x08
.equ THREAD_PROCESS,   0x10
.equ THREAD_TLS_BLCK,  0x18
.equ THREAD_KERNEL_SP, 0x20
.equ THREAD_USER_SP,   0x28

/* tls offsets */
.equ TLS_BASE_ADDR, 0x00

/* thread context offsets */
.equ CTX_RAX,    0x00
.equ CTX_RBX,    0x08
.equ CTX_RBP,    0x10
.equ CTX_R12,    0x18
.equ CTX_R13,    0x20
.equ CTX_R14,    0x28
.equ CTX_R15,    0x30
.equ CTX_RIP,    0x38
.equ CTX_CS,     0x40
.equ CTX_RFLAGS, 0x48
.equ CTX_RSP,    0x50
.equ CTX_SS,     0x58

/* Macros */

/* pushall: Push all general-purpose registers */
.macro pushall
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax
.endm

/* popall: Pop all general-purpose registers */
.macro popall
    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rdi
    popq %rsi
    popq %rbp
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
.endm

/* pushcaller: Save caller-saved registers */
.macro pushcaller
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rsi
    pushq %rdi
    pushq %rdx
    pushq %rcx
.endm

/* popcaller: Restore caller-saved registers */
.macro popcaller
    popq %rcx
    popq %rdx
    popq %rdi
    popq %rsi
    popq %r8
    popq %r9
    popq %r10
    popq %r11
.endm

/* pushcallee: Save callee-saved registers */
.macro pushcallee
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbp
    pushq %rbx
.endm

/* popcallee: Restore callee-saved registers */
.macro popcallee
    popq %rbx
    popq %rbp
    popq %r12
    popq %r13
    popq %r14
    popq %r15
.endm

/* retz: Conditional return macro.
   If ZF is not set (i.e. jnz takes the jump), then skip the ret. */
.macro retz
    jnz 1f
    ret
1:
.endm
