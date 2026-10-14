#include <drivers/timer/hpet.h>
#include <acpi/tables.h>
#include <mm/vmem_io.h>
#include <printk.h>

// ACPI HPET Table definition
typedef struct acpi_hpet_t {
    _acpi_sdt_hdr_t hdr;
    uint32_t hardware_rev_id : 8;
    uint32_t comparator_count : 5;
    uint32_t counter_size : 1;
    uint32_t reserved : 1;
    uint32_t legacy_replacement : 1;
    uint32_t pci_vendor_id : 16;

    // ACPI Generic Address Structure
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_width;
    uint64_t address;

    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} __attribute__((packed)) acpi_hpet_t;

// HPET Registers (Offsets from base address)
#define HPET_GENERAL_CAPABILITIES 0x00
#define HPET_GENERAL_CONFIG 0x10
#define HPET_MAIN_COUNTER 0xF0

static volatile uint64_t *hpet_base = NULL;
static uint64_t hpet_period = 0; // Length of one tick in femtoseconds

// Helper to read a 64-bit register from the HPET
static inline uint64_t hpet_read(uint64_t reg) {
    return *(volatile uint64_t *)((uintptr_t)hpet_base + reg);
}

// Helper to write a 64-bit register to the HPET
static inline void hpet_write(uint64_t reg, uint64_t value) {
    *(volatile uint64_t *)((uintptr_t)hpet_base + reg) = value;
}

_Bool hpet_init(void) {
    kprintf("Initializing HPET Timer...\n\r");

    // 1. Find the HPET table using your existing ACPI locator
    acpi_hpet_t *hpet_table = (acpi_hpet_t *)acpi_locate_table("HPET");
    if (!hpet_table) {
        kprintf("HPET ACPI table not found!\n\r");
        return 0; // Return false, initialization failed
    }

    // 2. Extract the physical base address
    uintptr_t hpet_phys_addr = (uintptr_t)hpet_table->address;
    kprintf("HPET Physical Address: %llx\n\r", hpet_phys_addr);

    // 3. Map the HPET registers into virtual memory.
    // Ensure vmem_memory_map maps this as Uncacheable (MMIO)
    hpet_base = vmem_map_mmio(hpet_phys_addr, 4096);
    if (!hpet_base)
        return 0;

    // 4. Calculate the period (femtoseconds per tick)
    // The period is stored in the upper 32 bits of the General Capabilities register
    uint64_t capabilities = hpet_read(HPET_GENERAL_CAPABILITIES);
    hpet_period = capabilities >> 32;

    if (hpet_period == 0) {
        kprintf("HPET reported an invalid period of 0!\n\r");
        return 0;
    }

    kprintf("HPET Period: %llu femtoseconds/tick\n\r", hpet_period);

    // 5. Enable the Main Counter
    // Read the general config, set bit 0 (ENABLE_CNF), and write it back
    uint64_t config = hpet_read(HPET_GENERAL_CONFIG);
    config |= 1;
    hpet_write(HPET_GENERAL_CONFIG, config);

    kprintf("HPET Timer enabled and running.\n\r");
    return 1;
}

uint64_t hpet_get_ticks(void) {
    if (!hpet_base)
        return 0;
    return hpet_read(HPET_MAIN_COUNTER);
}

void hpet_sleep_us(uint64_t microseconds) {
    if (!hpet_base || hpet_period == 0)
        return;

    // target_time in femtoseconds = microseconds * 1,000,000,000
    uint64_t target_fs = microseconds * 1000000000ULL;
    uint64_t ticks_to_wait = target_fs / hpet_period;

    uint64_t start_tick = hpet_get_ticks();

    // Busy wait loop
    while ((hpet_get_ticks() - start_tick) < ticks_to_wait) {
        __asm__ volatile("pause");
    }
}

void hpet_sleep_ms(uint64_t milliseconds) {
    // 1 millisecond = 1000 microseconds
    hpet_sleep_us(milliseconds * 1000ULL);
}
