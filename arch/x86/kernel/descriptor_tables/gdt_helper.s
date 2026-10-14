.text
.globl gdt_load
gdt_load:
    lgdt (%rdi)
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    pushq $0x08
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    mov $0x30, %ax
    ltr %ax
    ret
.globl load_pml4
load_pml4:
    mov %rdi, %cr3
    ret
.section .note.GNU-stack,"",@progbits
