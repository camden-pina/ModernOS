.altmacro
.text
.macro ISR num
.globl isr\num
isr\num:
    .if !(\num == 8 || (\num >= 10 && \num <= 14) || \num == 17 || \num == 21 || \num == 29 || \num == 30)
    pushq $0
    .endif
    pushq $\num
    jmp interrupt_entry
.endm
.set n, 0
.rept 256
ISR %n
.set n, n+1
.endr
.macro SAVE
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
.endm
.macro RESTORE
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax
.endm
interrupt_entry:
    cld
    SAVE
    /* NMI may arrive in a swapgs window. User GS is zero in this ABI. */
    xor %r12d, %r12d
    mov $0xc0000101, %ecx
    rdmsr
    test %edx, %edx
    js 1f
    swapgs
    mov $1, %r12d
1:
    lfence
    mov %rsp, %rdi
    mov %rsp, %rbx
    andq $-16, %rsp
    call interrupt_dispatch
    /* IST interrupts never schedule: restore their exact incoming GS. */
    cmpq $2, 120(%rbx)
    je 2f
    cmpq $8, 120(%rbx)
    je 2f
    mov %rax, %rsp
    cmpq $0, %gs:24
    je 4f
    mov %rsp, %rbx
    andq $-16, %rsp
    call sched_finish_switch
    mov %rbx, %rsp
4:
    testb $3, 144(%rsp)
    jz 3f
    swapgs
    jmp 3f
2:
    mov %rax, %rsp
    test %r12d, %r12d
    jz 3f
    swapgs
3:
    RESTORE
    addq $16, %rsp
    iretq
.globl syscall_entry
syscall_entry:
    swapgs
    lfence
    mov %rsp, %gs:16
    mov %gs:8, %rsp
    pushq $0x23
    pushq %gs:16
    push %r11
    pushq $0x2b
    push %rcx
    pushq $0
    pushq $256
    SAVE
    cld
    mov %rsp, %rdi
    call syscall_dispatch
    cli
    /* rt_sigreturn restores an interrupted user's RCX/R11 as well as RIP and
       flags. SYSRET consumes RCX/R11, so that return must use the IRET frame. */
    cmpq $257, 120(%rsp)
    je syscall_iret_return
    RESTORE
    addq $16, %rsp
    mov 0(%rsp), %rcx
    mov 16(%rsp), %r11
    mov 24(%rsp), %rsp
    swapgs
    sysretq
syscall_iret_return:
    RESTORE
    addq $16, %rsp
    swapgs
    iretq
.section .rodata
.globl isr_table
isr_table:
.macro PTR num
    .quad isr\num
.endm
.set n, 0
.rept 256
PTR %n
.set n, n+1
.endr
.section .note.GNU-stack,"",@progbits
