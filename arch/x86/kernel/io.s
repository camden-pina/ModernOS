.globl outb
outb:
    movl    %edi, %edx      # Move port (from edi) to edx
    movl    %esi, %eax      # Move data (from esi) to eax
    outb    %al, %dx        # Output the byte in al to port dx
    ret

.globl inb
inb:
    movl    %edi, %edx      # Move port (from edi) to edx
    inb     %dx, %al        # Input a byte from port dx into al
    ret

.globl outw
outw:
    movl    %edi, %edx      # Move port (from edi) to edx
    movl    %esi, %eax      # Move data (from esi) to eax
    outw    %ax, %dx        # Output the word in ax to port dx
    ret

.globl inw
inw:
    movl    %edi, %edx      # Move port (from edi) to edx
    inw     %dx, %ax        # Input a word from port dx into ax
    ret

.globl outdw
outdw:
    movl    %edi, %edx      # Move port (from edi) to edx
    movl    %esi, %eax      # Move data (from esi) to eax
    outl    %eax, %dx       # Output the double word in eax to port dx
    ret

.globl indw
indw:
    movl    %edi, %edx      # Move port (from edi) to edx
    inl     %dx, %eax       # Input a double word from port dx into eax
    ret

.globl read_msr
read_msr:
    /* index is in %rdi; move it into %rcx for rdmsr */
    movq    %rdi, %rcx      # MSR index -> %rcx
    rdmsr                   # Read MSR: low in %eax, high in %edx
    shl     $32, %rdx       # Shift high dword left 32 bits
    or      %rax, %rdx      # Combine: high<<32 | low (low in %rax)
    movq    %rdx, %rax      # Move result into %rax (return value)
    ret

    /* void write_msr(unsigned long long index, unsigned long long value) */
    .globl write_msr
write_msr:
    /* index is in %rdi and value in %rsi */
    movq    %rdi, %rcx      # Move index to %rcx for wrmsr
    /* Split 'value' into low (to %eax) and high (to %edx) */
    movq    %rsi, %rax      # Copy full value to %rax
    movq    %rsi, %rdx      # Copy full value to %rdx
    shrq    $32, %rdx       # Shift right: %rdx now holds the high dword
    andl    $0xffffffff, %eax  # Mask: %eax now holds the low dword
    wrmsr                   # Write MSR using values in %edx:%eax and index in %ecx
    ret

    /* unsigned char io_read_8(unsigned port) */
    .globl io_read_8
io_read_8:
    /* port is in %edi; move it to %edx for inb */
    movl    %edi, %edx      # Port number -> %edx (16-bit value used)
    inb     %dx, %al        # Read byte from port in %dx into %al
    /* Return value in %al (lower part of %rax) */
    ret

    /* void io_write_8(unsigned port, unsigned char val) */
    .globl io_write_8
io_write_8:
    /* port is in %edi, value in %esi */
    movl    %edi, %edx      # Port number -> %edx
    movl    %esi, %eax      # Value -> %eax (we need it in %al)
    outb    %al, %dx        # Write byte in %al to port in %dx
    ret
