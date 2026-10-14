.text

.extern IDTR

.globl idt_flush
idt_flush:
    /* Same idea: use RIP-relative addressing. */
    lidt IDTR(%rip)
    ret
