#include <kernel.h>
#include <descriptor_tables/gdt.h>
#include <descriptor_tables/idt.h>
#include <descriptor_tables/isr.h>
#include <interrupts/lapic.h>
#include <acpi/acpi.h>
#include <mm/pmm.h>
#include <mm/vmem.h>
#include <printf.h>

#include "ps2_mouse.h"

#include <stdarg.h>

#include <io.h>
#include <string.h>
#include <8250.h>

static unsigned char SCAN_CODE_MAPPING[] = "\x00""\x1B""1234567890-=""\x08""\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 \0\0\0\0\0\0\0\0\0\0\0\0\0-456+1230.\0\0\0\0\0";

static void keyboard_isr(void)
{
	unsigned char scancode = inb(0x60);	// Read Scancode

	// Reset Keyboard Controller
	unsigned char a = inb(0x61);
	a |= 0x82;
	outb(0x61, a);
	a &= 0x7f;
	outb(0x61, a);

	if (scancode & 0x80)
	{
		scancode = scancode & ~0x80;

		unsigned char pressed_char = SCAN_CODE_MAPPING[scancode];

		kprintf("%c", pressed_char);
	}
}

static void ps2_keyboard_init(void)
{
	kprintf("Initializing Keyboard\n\r");
	register_interrupt_handler(33, keyboard_isr);
	ioapic_map(1, 33);
}

extern uint64_t kernel_start;
extern uint64_t kernel_end;

// Inline functions to perform port I/O operations.
static inline void serial_outb(uint16_t port, uint8_t data) {
    __asm__ volatile ("outb %0, %1" : : "a"(data), "Nd"(port));
}

static inline uint8_t serial_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

boot_info_v2_t __boot_data *boot_info_v2;

void kern_main(boot_info_v2_t* boot_hdr)
{
	serial_port_init(COM1_PORT);

	kprintf_early_init();

	pmm_init(boot_info_v2->mem_map.map, boot_info_v2->mem_map.size, sizeof(memory_map_entry_t));

	__asm__ volatile("cli");
	acpi_init((long long unsigned int *)boot_info_v2->acpi_ptr);
	
	gdt_init();
	apic_init();
	idt_init();

	ps2_keyboard_init();
	ps2_mouse_init();
	__asm__ volatile("sti");
	// drawRect(0, 0, boot_hdr->fb->px_width, boot_hdr->fb->px_height, 0x00000000);
	// krnl_printf_reset_x();
	// krnl_printf_reset_y();
	
	kprintf("ModernOS (C)\n\n\r");
	kprintf("Copyright (C) Ideal Technologies Inc.\n\r");

	char *str = kmalloc(1);
	str = "cat\0";
	kprintf("%s", str);
	
	while (1)
	{

	}
}
