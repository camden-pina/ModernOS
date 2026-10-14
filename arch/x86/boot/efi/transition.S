/* Microsoft x64 EFI ABI: rcx=CR3, rdx=stack, r8=entry, r9=boot_info.
 * The kernel uses SysV: rdi=boot_info and rsp % 16 == 8 at entry. */
.text
.globl efi_enter_kernel
efi_enter_kernel:
    cli
    cld
    movq %cr4, %rax
    andq $-131201, %rax /* Clear PCIDE and PGE before replacing firmware tables. */
    movq %rax, %cr4
    movq %rcx, %cr3
    lgdt efi_gdtr(%rip)
    pushq $0x08
    leaq .Lkernel_segments(%rip), %rax
    pushq %rax
    lretq
.Lkernel_segments:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    movq %cr0, %rax
    andq $-13, %rax /* Clear EM and TS. */
    orq $0x10002, %rax /* MP and WP. */
    movq %rax, %cr0
    movq %cr4, %rax
    orq $0x600, %rax /* OSFXSR and OSXMMEXCPT. */
    movq %rax, %cr4
    movq %rdx, %rsp
    andq $-16, %rsp
    subq $8, %rsp
    movq $0, (%rsp)
    xorq %rbp, %rbp
    movq %r9, %rdi
    jmp *%r8

.section .rdata,"dr"
.p2align 3
efi_gdt:
    .quad 0
    .quad 0x00af9a000000ffff
    .quad 0x00cf92000000ffff
efi_gdtr:
    .word 23
    .quad efi_gdt
