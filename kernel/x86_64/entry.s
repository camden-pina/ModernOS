.section .text
.global entry
.global ap_entry
.extern kern_main

.set PAGE_SIZE, 0x1000
.set GS_BASE_MSR, 0xC0000101

.set PERCPU_ID_OFF, 0x00
.set PERCPU_SELF_OFF, 0x08

.set TCB_SIZE, 0xa0  # 0x98 aligned to 16 bytes
.set TRAPFRAME_SIZE, 0xc0

# Top of the thread stack sits below the trapframe and tcb
.set STACK_TOP_OFF, (TCB_SIZE + TRAPFRAME_SIZE)

# =======================
#       Kernel Entry
# =======================
entry:
  # Use RIP-relative addressing to load stack pointer
  leaq entry_initial_stack_top(%rip), %rsp
  subq $STACK_TOP_OFF, %rsp  # Adjust for TCB and trapframe

  # Setup the BSP percpu structure using RIP-relative addressing
  leaq entry_initial_cpu_reserved(%rip), %rax  # Load PERCPU area
  movl $0, PERCPU_ID_OFF(%rax)  # Set PERCPU->id = 0
  movq %rax, PERCPU_SELF_OFF(%rax)  # Set PERCPU->self = rax

  movq %rax, %rdx
  movb $32, %cl
  shrq %cl, %rdx
  movl $GS_BASE_MSR, %ecx
  wrmsr

  # Enable percpu and call the kernel
  cld
  cli
  call kern_main

.hang1:
  hlt
  jmp .hang1  # Hang


# =======================
#         Data
# =======================
.section .data
.align 0x1000
.global entry_initial_stack
.global entry_initial_stack_top

entry_initial_stack:
  .skip 4*PAGE_SIZE
entry_initial_stack_top:

entry_initial_cpu_reserved:
  .skip PAGE_SIZE  # cpu0 percpu area
