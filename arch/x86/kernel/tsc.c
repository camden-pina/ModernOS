#include <stdint.h>
#include <stdbool.h>
#include <io.h>
#include <printk.h>

static uint64_t tsc_boot_start = 0;
static uint64_t tsc_khz = 0;
static uint64_t tsc_mhz = 0;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Calibrate the CPU TSC against PIT Channel 2 (One-Shot, 10ms gate).
 * Requires zero interrupts; safe to run while CLI is active.
 */
void tsc_init(void) {
    tsc_boot_start = rdtsc();

    /* 1. Gate PIT Channel 2 low, speaker off */
    uint8_t gate = inb(0x61);
    outb(0x61, (gate & ~0x02) | 0x01);

    /* 2. Configure PIT Channel 2: Mode 0 (interrupt on terminal count), binary */
    outb(0x43, 0xB0);

    /*
     * 10ms count calculation:
     * PIT base clock = 1193182 Hz -> 1193182 / 100 = ~11932 ticks for 10ms
     */
    uint16_t pit_ticks = 11932;
    outb(0x42, (uint8_t)(pit_ticks & 0xFF));
    outb(0x42, (uint8_t)((pit_ticks >> 8) & 0xFF));

    /* Reset gate to trigger counter */
    gate = inb(0x61);
    outb(0x61, (gate & ~0x01));
    outb(0x61, (gate | 0x01));

    uint64_t start = rdtsc();

    /* 3. Spin until PIT Channel 2 OUT pin transitions high (bit 5 on port 0x61) */
    while ((inb(0x61) & 0x20) == 0) {
        __asm__ volatile("pause");
    }

    uint64_t end = rdtsc();

    /* Disable PIT Channel 2 gate */
    outb(0x61, inb(0x61) & ~0x01);

    uint64_t elapsed_cycles = end - start;

    /* Cycles in 10ms -> divide by 10 to get cycles per millisecond (kHz) */
    tsc_khz = elapsed_cycles / 10;
    tsc_mhz = tsc_khz / 1000;
    if (tsc_mhz == 0)
        tsc_mhz = 1; /* Safety guard for emulators */
}

/* Monotonic uptime generator called by printk */
uint64_t tsc_uptime_us(void) {
    if (tsc_khz == 0) {
        return 0;
    }
    uint64_t delta = rdtsc() - tsc_boot_start;
    return delta / tsc_mhz;
}
