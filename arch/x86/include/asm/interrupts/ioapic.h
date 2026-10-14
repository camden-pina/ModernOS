#ifndef _IOAPIC_H
#define _IOAPIC_H 1

void ioapic_map(unsigned char irq_index, unsigned char idt_index);
_Bool load_ioapic_address(void);

#endif
