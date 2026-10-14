/* x86-64 SSE2 is mandatory. Preserve every SIMD register used so interrupts,
   preemption and user FPU state stay safe. The kernel otherwise avoids SIMD.
   Exact spans: no overread or overwrite. Caller fences before presenting. */
.text
.global video_copy
.type video_copy,@function
video_copy:
    sub $64,%rsp
    movdqu %xmm0,0(%rsp)
    movdqu %xmm1,16(%rsp)
    movdqu %xmm2,32(%rsp)
    movdqu %xmm3,48(%rsp)
    mov %rdx,%r8
    mov %rdi,%rcx
    neg %rcx
    and $63,%ecx
    cmp %rdx,%rcx
    cmova %rdx,%rcx
    sub %rcx,%r8
    rep movsb
    cmp $64,%r8
    jb 2f
1:
    movdqu 0(%rsi),%xmm0
    movdqu 16(%rsi),%xmm1
    movdqu 32(%rsi),%xmm2
    movdqu 48(%rsi),%xmm3
    movntdq %xmm0,0(%rdi)
    movntdq %xmm1,16(%rdi)
    movntdq %xmm2,32(%rdi)
    movntdq %xmm3,48(%rdi)
    add $64,%rsi
    add $64,%rdi
    sub $64,%r8
    cmp $64,%r8
    jae 1b
2:
    mov %r8,%rcx
    shr $3,%rcx
    rep movsq
    mov %r8d,%ecx
    and $7,%ecx
    rep movsb
    movdqu 0(%rsp),%xmm0
    movdqu 16(%rsp),%xmm1
    movdqu 32(%rsp),%xmm2
    movdqu 48(%rsp),%xmm3
    add $64,%rsp
    ret
.size video_copy,.-video_copy
.section .note.GNU-stack,"",@progbits
