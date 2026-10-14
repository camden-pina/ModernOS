/* Enter a boot adapter callback using a stack owned by the scheduler. */
.section .text.boot_ap_enter,"ax",@progbits
.code64
.global boot_ap_enter
.type boot_ap_enter,@function
boot_ap_enter:
    cli
    movq %rdi,%rax
    movq %rsi,%rdi
    movq %rdx,%rsi
    movq %rcx,%rsp
    andq $-16,%rsp
    subq $8,%rsp
    movq $0,(%rsp)
    xorq %rbp,%rbp
    jmp *%rax
.size boot_ap_enter,.-boot_ap_enter

/* This position-independent blob is copied to a reserved SIPI page below
 * 1 MiB. Only its GDT/far pointers and handoff fields require patching. */
.section .rodata.boot_ap_trampoline,"a",@progbits
.balign 16
.global boot_ap_trampoline_start,boot_ap_trampoline_end
.global boot_ap_trampoline_gdt,boot_ap_trampoline_gdt_base
.global boot_ap_trampoline_pm_target,boot_ap_trampoline_pm_entry
.global boot_ap_trampoline_lm_target,boot_ap_trampoline_lm_entry
.global boot_ap_trampoline_cr3,boot_ap_trampoline_stack
.global boot_ap_trampoline_cpu,boot_ap_trampoline_argument,boot_ap_trampoline_entry
boot_ap_trampoline_start:
.code16
    cli
    cld
    xorw %ax,%ax
    movw %ax,%ss
    movw %cs,%ax
    movw %ax,%ds
    movw %ax,%es
    movzwl %ax,%ebp
    shll $4,%ebp
    lgdt (boot_ap_trampoline_gdtr-boot_ap_trampoline_start)
    movl %cr0,%eax
    orl $1,%eax
    movl %eax,%cr0
    ljmpl *(boot_ap_trampoline_pm_target-boot_ap_trampoline_start)
.code32
boot_ap_trampoline_pm_entry:
    movw $0x10,%ax
    movw %ax,%ds
    movw %ax,%es
    movw %ax,%ss
    movl $0x20,%eax
    movl %eax,%cr4
    movl (boot_ap_trampoline_cr3-boot_ap_trampoline_start)(%ebp),%eax
    movl %eax,%cr3
    movl $0xc0000080,%ecx
    rdmsr
    orl $0x900,%eax /* LME and NXE, before using the kernel's NX mappings. */
    wrmsr
    movl %cr0,%eax
    andl $0x9fffffff,%eax /* Enable caching after INIT. */
    orl $0x80010001,%eax /* PG, WP and PE. */
    movl %eax,%cr0
    ljmpl *(boot_ap_trampoline_lm_target-boot_ap_trampoline_start)(%ebp)
.code64
boot_ap_trampoline_lm_entry:
    movw $0x10,%ax
    movw %ax,%ds
    movw %ax,%es
    movw %ax,%ss
    movq (boot_ap_trampoline_stack-boot_ap_trampoline_start)(%rbp),%rsp
    andq $-16,%rsp
    subq $8,%rsp
    movq $0,(%rsp)
    movq (boot_ap_trampoline_cpu-boot_ap_trampoline_start)(%rbp),%rdi
    movq (boot_ap_trampoline_argument-boot_ap_trampoline_start)(%rbp),%rsi
    movq (boot_ap_trampoline_entry-boot_ap_trampoline_start)(%rbp),%rax
    xorq %rbp,%rbp
    jmp *%rax
.balign 8
boot_ap_trampoline_gdt:
    .quad 0
    .quad 0x00cf9a000000ffff /* 32-bit protected-mode code. */
    .quad 0x00cf92000000ffff
    .quad 0x00af9a000000ffff /* 64-bit code. */
boot_ap_trampoline_gdtr:
    .word 31
boot_ap_trampoline_gdt_base:
    .long 0
boot_ap_trampoline_pm_target:
    .long 0
    .word 8
boot_ap_trampoline_lm_target:
    .long 0
    .word 0x18
.balign 8
boot_ap_trampoline_cr3:
    .quad 0
boot_ap_trampoline_stack:
    .quad 0
boot_ap_trampoline_cpu:
    .quad 0
boot_ap_trampoline_argument:
    .quad 0
boot_ap_trampoline_entry:
    .quad 0
boot_ap_trampoline_end:
.section .note.GNU-stack,"",@progbits
