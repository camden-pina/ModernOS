/* A real BIOS boot sector: serial output, arithmetic, RAM, and clean exit. */
.code16
.global _start
_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7c00, %sp
    movw $0x3fb, %dx
    movb $0x80, %al
    outb %al, %dx
    movw $0x3f8, %dx
    movb $1, %al
    outb %al, %dx
    incw %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3fb, %dx
    movb $3, %al
    outb %al, %dx
    movw $0x3f9, %dx
    xorb %al, %al
    outb %al, %dx
    movw $6, %ax
    movw $7, %bx
    mulw %bx
    cmpw $42, %ax
    jne fail
    movw %ax, 0x600
    cmpw $42, 0x600
    jne fail
    /* Identity-map the first 2 MiB and enter the emulated 64-bit CPU mode. */
    cld
    movl $0x1000, %edi
    movl $3072, %ecx
    xorl %eax, %eax
    rep stosl
    movl $0x2003, 0x1000
    movl $0x3003, 0x2000
    movl $0x0083, 0x3000
    lgdt gdt_pointer
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4
    movl $0x1000, %eax
    movl %eax, %cr3
    movl $0xc0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr
    movl %cr0, %eax
    orl $0x80000001, %eax
    movl %eax, %cr0
    ljmpl $8, $long_mode
halt:
    hlt
    jmp halt
fail:
    movw $failure, %si
    call puts
    movw $0xf4, %dx
    movb $0x11, %al
    outb %al, %dx
    jmp halt
puts:
    lodsb
    testb %al, %al
    jz done
    movb %al, %bl
poll:
    movw $0x3fd, %dx
    inb %dx, %al
    testb $0x20, %al
    jz poll
    movw $0x3f8, %dx
    movb %bl, %al
    outb %al, %dx
    jmp puts
done:
    ret
failure:
    .asciz "COAST_QEMU_GUEST_FAIL\r\n"
.balign 8
gdt:
    .quad 0, 0x00af9a000000ffff, 0x00cf92000000ffff
gdt_end:
gdt_pointer:
    .word gdt_end-gdt-1
    .long gdt
.code64
long_mode:
    movw $16, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $0x7c00, %esp
    movabsq $0x123456789abcdef0, %rax
    movq %rax, 0x600
    movq 0x600, %rbx
    cmpq %rax, %rbx
    jne long_fail
    shrq $32, %rbx
    cmpq $0x12345678, %rbx
    jne long_fail
    movl $message, %esi
    movb $0x10, %cl
    jmp long_puts
long_fail:
    movl $failure, %esi
    movb $0x11, %cl
long_puts:
    lodsb
    testb %al, %al
    jz long_exit
    movb %al, %bl
long_poll:
    movw $0x3fd, %dx
    inb %dx, %al
    testb $0x20, %al
    jz long_poll
    movw $0x3f8, %dx
    movb %bl, %al
    outb %al, %dx
    jmp long_puts
long_exit:
    movw $0xf4, %dx
    movb %cl, %al
    outb %al, %dx
long_halt:
    hlt
    jmp long_halt
message:
    .asciz "COAST_QEMU_GUEST_PASS 42 LONG64\r\n"
.org 510
.word 0xaa55
