#ifndef _IDT_H
#define _IDT_H 1

#include <interrupts/ioapic.h>
#include <descriptor_tables/isr.h>
#include <kernel.h>

// Idt functions
void idt_init(void);
void idt_load(void);

#endif
