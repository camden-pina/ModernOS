#include <descriptor_tables/idt.h>
#include <descriptor_tables/gdt.h>

struct gate {
    uint16_t lo, selector;
    uint8_t ist, flags;
    uint16_t mid;
    uint32_t hi, reserved;
} __attribute__((packed));
struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) IDTR;
static struct gate gates[256] __attribute__((aligned(16)));
extern void *isr_table[256];

void idt_load(void) {
    __asm__ volatile("lidt %0" ::"m"(IDTR) : "memory");
}

void idt_init(void) {
    for (unsigned i = 0; i < 256; i++) {
        uintptr_t addr = (uintptr_t)isr_table[i];
        gates[i] = (struct gate){
            addr, KERNEL_CS, i == 8 ? 1 : (i == 2 ? 2 : 0), 0x8e, addr >> 16, addr >> 32, 0};
    }
    IDTR.limit = sizeof(gates) - 1;
    IDTR.base = (uintptr_t)gates;
    idt_load();
}
