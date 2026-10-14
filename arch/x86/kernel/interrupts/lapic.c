#include <interrupts/lapic.h>
#include <mm/vmem_io.h>
#include <drivers/timer/hpet.h>
#include <tsc.h>
#include <panic.h>
#include <io.h>
#include <stdint.h>

static volatile uint8_t *apic_mmio;
static uint32_t timer_count;

static uint64_t msr_read(unsigned n) {
    uint32_t a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(n));
    return a | ((uint64_t)d << 32);
}

static void msr_write(unsigned n, uint64_t v) {
    __asm__ volatile("wrmsr" ::"a"((uint32_t)v), "d"((uint32_t)(v >> 32)), "c"(n) : "memory");
}

static unsigned byte_offset(int index) {
    return index < 0x20 ? index * 16 : index;
}

void apic_write(int index, unsigned value) {
    unsigned offset = byte_offset(index);
    if (msr_read(0x1b) & 0x400)
        msr_write(0x800 + offset / 16, value);
    else
        *(volatile uint32_t *)(apic_mmio + offset) = value;
}

unsigned apic_read(int index) {
    unsigned offset = byte_offset(index);
    if (msr_read(0x1b) & 0x400)
        return msr_read(0x800 + offset / 16);
    return *(volatile uint32_t *)(apic_mmio + offset);
}

void apic_init_cpu(void) {
    uint64_t base = msr_read(0x1b);
    msr_write(0x1b, base | 0x800);
    apic_write(0x80, 0);
    apic_write(0xf0, 0x1ff);
    apic_write(0x350, 0x10000);
    apic_write(0x360, 0x10000);
    apic_write(0x370, 0x10000);
    apic_write(0x320, 0x10000);
}

void apic_init(void) {
    outb(0x21, 255);
    outb(0xa1, 255);
    uint64_t base = msr_read(0x1b);
    if (!(base & 0x400)) {
        apic_mmio = vmem_map_mmio(base & 0x000ffffffffff000ULL, 4096);
        if (!apic_mmio)
            panic("cannot map LAPIC");
    }
    apic_init_cpu();
    apic_write(0x3e0, 3); /* divide by 16 */
    apic_write(0x380, 0xffffffff);
    uint64_t start = tsc_uptime_us();
    while (tsc_uptime_us() - start < 10000)
        __asm__ volatile("pause");
    timer_count = 0xffffffff - apic_read(0x390);
    apic_write(0x380, 0);
    if (timer_count < 100)
        panic("LAPIC timer did not advance");
}

void apic_timer_start(void) {
    apic_write(0x3e0, 3);
    /* Input and display waits need millisecond wakeups. The old 10 ms tick
       rounded every short sleep up and regularly missed display deadlines. */
    apic_write(0x320, (1u << 17) | 0xe0);
    apic_write(0x380, max(timer_count / 10, 1u));
}

void apic_send_eoi_if_necessary(unsigned char vector) {
    if (vector < 32 || vector == 255)
        return;
    if (apic_read(0x100 + (vector / 32) * 16) & (1u << (vector % 32)))
        apic_write(0xb0, 0);
}
