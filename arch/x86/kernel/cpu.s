.include "kernel.inc"

# Interrupts

.globl cpu_disable_interrupts
cpu_disable_interrupts:
    cli
    ret

.globl cpu_enable_interrupts
cpu_enable_interrupts:
    sti
    ret

.globl cpu_save_clear_interrupts
cpu_save_clear_interrupts:
    pushfq
    popq %rax
    cli
    ret

.globl cpu_restore_interrupts
cpu_restore_interrupts:
    pushq %rdi
    popfq
    ret

# General Registers

.globl cpu_read_stack_pointer
cpu_read_stack_pointer:
    movq %rsp, %rax
    ret

.globl cpu_write_stack_pointer
cpu_write_stack_pointer:
    movq %rdi, %rsp
    ret

.globl cpu_read_msr
cpu_read_msr:
    movl %edi, %ecx       # move low 32 bits of edi to ecx
    rdmsr
    shlq $32, %rdx        # shift rdx left by 32 bits
    orq %rdx, %rax        # combine edx:eax into rax
    ret

.globl cpu_write_msr
cpu_write_msr:
    movq %rsi, %rax       # prepare value in rax
    movq %rsi, %rdx       # copy to rdx
    shrq $32, %rdx        # shift rdx right by 32 bits
    movl %edi, %ecx       # move msr index into ecx
    wrmsr
    ret

.globl cpu_read_tsc
cpu_read_tsc:
    movl $0x1, %eax       # set eax to 1
    cpuid
    rdtsc
    movb $32, %cl        # load 32 into cl
    shlq %cl, %rdx       # shift rdx left by cl
    orq %rdx, %rax       # combine rdx into rax
    ret

.globl cpu_read_fsbase
cpu_read_fsbase:
    movq %rdi, %rsi
    movq $FS_BASE_MSR, %rdi
    call cpu_read_msr
    ret

.globl cpu_write_fsbase
cpu_write_fsbase:
    movq %rdi, %rsi
    movq $FS_BASE_MSR, %rdi
    call cpu_write_msr
    ret

.globl cpu_read_gsbase
cpu_read_gsbase:
    movq $GS_BASE_MSR, %rdi
    call cpu_read_msr
    ret

.globl cpu_write_gsbase
cpu_write_gsbase:
    movq %rdi, %rsi
    movq $GS_BASE_MSR, %rdi
    call cpu_write_msr
    ret

.globl cpu_read_kernel_gsbase
cpu_read_kernel_gsbase:
    movq $KERNEL_GS_BASE_MSR, %rdi
    call cpu_read_msr
    ret

.globl cpu_write_kernel_gsbase
cpu_write_kernel_gsbase:
    movq %rdi, %rsi
    movq $KERNEL_GS_BASE_MSR, %rdi
    call cpu_write_msr
    ret

# GDT/IDT

.globl cpu_load_gdt
cpu_load_gdt:
    lgdt (%rdi)
    ret

.globl cpu_load_idt
cpu_load_idt:
    lidt (%rdi)
    ret

.globl cpu_load_tr
cpu_load_tr:
    ltr %di              # 16-bit register used for TR
    ret

# void cpu_set_cs(uint16_t cs)
.globl cpu_set_cs
cpu_set_cs:
    leaq .reload(%rip), %rax   # load address of .reload into rax
    pushq %rdi
    pushq %rax
    lretq                   # far return: pops IP and CS
.reload:
    ret

# void cpu_set_ds(uint16_t ds)
.globl cpu_set_ds
cpu_set_ds:
    movw %di, %ss         # set SS to ds (16-bit)
    movw %di, %ds         # set DS to ds
    movw %di, %es         # set ES to ds
    ret

# Control Registers

.globl __read_cr0
__read_cr0:
    movq %cr0, %rax
    ret

.globl __write_cr0
__write_cr0:
    movq %rdi, %cr0
    ret

.globl __read_cr2
__read_cr2:
    movq %cr2, %rax
    ret

.globl __read_cr3
__read_cr3:
    movq %cr3, %rax
    ret

.globl __write_cr3
__write_cr3:
    movq %rdi, %cr3
    ret

.globl __read_cr4
__read_cr4:
    movq %cr4, %rax
    ret

.globl __write_cr4
__write_cr4:
    movq %rdi, %cr4
    ret

.globl __xgetbv
__xgetbv:
    movl %edi, %ecx
    xgetbv
    movb $32, %cl
    shlq %cl, %rdx
    orq %rdx, %rax
    ret

.globl __xsetbv
__xsetbv:
    movl %edi, %ecx
    movq %rsi, %rax
    movq %rsi, %rdx
    movb $32, %cl
    shrq %cl, %rdx
    xsetbv
    ret

.globl __fxsave
__fxsave:
    fxsave64 (%rdi)
    ret

.globl __fxrstor
__fxrstor:
    fxrstor64 (%rdi)
    ret

# Paging/TLB

.globl cpu_flush_tlb
cpu_flush_tlb:
    movq %cr3, %rax
    movq %rax, %cr3
    ret

# Syscalls

.globl syscall
syscall:
    movq %rdi, %rax     # move syscall code from rdi to rax
    syscall
