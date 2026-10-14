.text
.global _start
_start:
    xor %ebp,%ebp
    mov (%rsp),%rdi
    lea 8(%rsp),%rsi
    lea 8(%rsi,%rdi,8),%rdx
    mov %rdx,environ(%rip)
    andq $-16,%rsp
    call main
    mov %eax,%edi
    call exit
    ud2
.section .note.GNU-stack,"",@progbits
