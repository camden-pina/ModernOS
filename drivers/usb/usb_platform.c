#include <drivers/usb/usb_platform.h>
#include <mm/pmm.h>
#include <mm/vmem_io.h>
#include <acpi/tables.h>
#include <printk.h>
#include <string.h>

#include <boot/info.h>

#define PM_HZ 3579545ull
#define TIMER_STALL_LIMIT 10000000u
#define PCI_MEM 0x0002u
#define PCI_MASTER 0x0004u
#define PCI_INTX_OFF 0x0400u

static struct {
    bool ready;
    uint16_t port;
    volatile uint32_t *mmio;
    uint32_t mask;
    bool tsc_attempted;
    uint64_t tsc_start;
    uint64_t tsc_hz;
} clock_state;

static struct {
    pci_device_header_t *header;
    uint64_t base;
    uint64_t size;
} controller_bar;

void usb_platform_relax(void) {
    __asm__ volatile("pause" ::: "memory");
}

static uint16_t cfg16(pci_device_header_t *header, unsigned offset) {
    return *(volatile uint16_t *)((uintptr_t)header + offset);
}

static uint32_t cfg32(pci_device_header_t *header, unsigned offset) {
    return *(volatile uint32_t *)((uintptr_t)header + offset);
}

static uint8_t cfg8(pci_device_header_t *header, unsigned offset) {
    return *(volatile uint8_t *)((uintptr_t)header + offset);
}

static void cfg_write16(pci_device_header_t *header, unsigned offset, uint16_t value) {
    *(volatile uint16_t *)((uintptr_t)header + offset) = value;
    __asm__ volatile("" ::: "memory");
}

static void cfg_write32(pci_device_header_t *header, unsigned offset, uint32_t value) {
    *(volatile uint32_t *)((uintptr_t)header + offset) = value;
    __asm__ volatile("" ::: "memory");
}

/* FADT byte offsets from ACPI: avoids depending on a second FADT typedef.
 * memcpy also avoids unaligned packed-table loads through typed pointers.
 */
static uint32_t table_u32(const uint8_t *table, unsigned offset) {
    uint32_t value;
    memcpy(&value, table + offset, sizeof(value));
    return value;
}

static uint64_t table_u64(const uint8_t *table, unsigned offset) {
    uint64_t value;
    memcpy(&value, table + offset, sizeof(value));
    return value;
}

static uint32_t pm_read(void) {
    uint32_t value;
    if (clock_state.mmio) {
        value = *clock_state.mmio;
    } else {
        __asm__ volatile("inl %w1, %0" : "=a"(value) : "Nd"(clock_state.port));
    }
    return value & clock_state.mask;
}

static bool pm_ready(void) {
    if (clock_state.ready)
        return true;
    const uint8_t *fadt = (const uint8_t *)acpi_locate_table("FACP");
    if (!fadt || memcmp(fadt, "FACP", 4) != 0)
        return false;
    uint32_t length = table_u32(fadt, 4);
    if (length < 116 || fadt[91] != 4)
        return false;
    uint32_t flags = table_u32(fadt, 112);
    if (flags & (1u << 20)) /* Hardware-reduced ACPI has no fixed PM timer. */
        return false;

    uint8_t space = 1; /* System I/O for the legacy PM_TMR_BLK. */
    uint64_t address = table_u32(fadt, 76);
    if (length >= 220 && fadt[8] >= 2 && table_u64(fadt, 212)) {
        /* X_PM_TMR_BLK is a 12-byte GAS starting at offset 208. */
        space = fadt[208];
        if ((fadt[209] != 24 && fadt[209] != 32) || fadt[210] != 0 ||
            (fadt[211] != 0 && fadt[211] != 3))
            return false;
        address = table_u64(fadt, 212);
    }
    if (!address)
        return false;
    clock_state.mmio = NULL;
    clock_state.port = 0;
    clock_state.mask = (flags & (1u << 8)) ? UINT32_MAX : 0x00ffffffu;
    if (space == 1) {
        if (address > 0xfffcull)
            return false;
        clock_state.port = (uint16_t)address;
    } else if (space == 0) {
        if (address & 3u)
            return false;
        clock_state.mmio = vmem_map_mmio(address, sizeof(uint32_t));
        if (!clock_state.mmio)
            return false;
    } else {
        return false;
    }

    /* A zero-ms readiness check verifies that the hardware advances. */
    uint32_t initial = pm_read();
    for (unsigned i = 0; i < TIMER_STALL_LIMIT; ++i) {
        if (pm_read() != initial) {
            clock_state.ready = true;
            return true;
        }
        usb_platform_relax();
    }
    return false;
}

bool usb_platform_delay_ms(uint32_t ms) {
    if (!pm_ready())
        return false;
    uint64_t remaining = ((uint64_t)ms * PM_HZ + 999ull) / 1000ull;
    uint32_t previous = pm_read();
    unsigned stalled = 0;
    while (remaining) {
        uint32_t now = pm_read();
        uint32_t delta = (now - previous) & clock_state.mask;
        previous = now;
        if (!delta) {
            if (++stalled == TIMER_STALL_LIMIT)
                return false;
        } else {
            stalled = 0;
            if (delta >= remaining)
                break;
            remaining -= delta;
        }
        usb_platform_relax();
    }
    return true;
}

static uint64_t read_tsc(void) {
    uint32_t low, high;
    /* x86-64 baseline includes SSE2/LFENCE; no SIMD registers are used. */
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high)::"memory");
    return low | ((uint64_t)high << 32);
}

static bool invariant_tsc(void) {
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (!(d & (1u << 4)))
        return false;
    a = 0x80000000u;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (a < 0x80000007u)
        return false;
    a = 0x80000007u;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (d & (1u << 8)) != 0;
}

static bool calibrate_tsc(void) {
    if (clock_state.tsc_hz)
        return true;
    if (clock_state.tsc_attempted || !pm_ready())
        return false;
    clock_state.tsc_attempted = true;
    if (!invariant_tsc())
        return false;
    uint32_t initial = pm_read();
    uint64_t start = read_tsc();
    uint32_t elapsed;
    unsigned stalled = 0;
    uint32_t previous = initial;
    do {
        uint32_t now = pm_read();
        elapsed = (now - initial) & clock_state.mask;
        if (now == previous) {
            if (++stalled == TIMER_STALL_LIMIT)
                return false;
        } else {
            stalled = 0;
        }
        previous = now;
        usb_platform_relax();
    } while (elapsed < (PM_HZ + 19ull) / 20ull); /* About 50 ms. */
    uint64_t finish = read_tsc();
    uint64_t cycles = finish - start;
    if (!cycles || cycles > UINT64_MAX / PM_HZ)
        return false;
    uint64_t hz = cycles * PM_HZ / elapsed;
    if (hz < 1000000ull || hz > 10000000000ull)
        return false;
    clock_state.tsc_start = finish;
    clock_state.tsc_hz = hz;
    return true;
}

bool usb_platform_time_ms(uint64_t *ms) {
    if (!ms || !calibrate_tsc())
        return false;
    uint64_t cycles = read_tsc() - clock_state.tsc_start;
    /* Avoid cycles * 1000 overflowing after prolonged uptime. */
    *ms = (cycles / clock_state.tsc_hz) * 1000ull +
          ((cycles % clock_state.tsc_hz) * 1000ull) / clock_state.tsc_hz;
    return true;
}

static bool quiet_capabilities(pci_device_header_t *header) {
    if (!(cfg16(header, 6) & PCI_STATUS_CAPABILITIES_LIST))
        return true;
    bool seen[64] = {false};
    unsigned offset = cfg8(header, 0x34);
    while (offset) {
        if (offset < 0x40 || offset > 0xfc || (offset & 3u) || seen[offset >> 2])
            return false;
        seen[offset >> 2] = true;
        uint8_t id = cfg8(header, offset);
        uint8_t next = cfg8(header, offset + 1);
        uint16_t control = cfg16(header, offset + 2);
        if (id == PCI_CAP_ID_MSI) {
            cfg_write16(header, offset + 2, control & ~1u);
            if (cfg16(header, offset + 2) & 1u)
                return false;
        } else if (id == PCI_CAP_ID_MSIX) {
            cfg_write16(header, offset + 2, (control | (1u << 14)) & ~(1u << 15));
            if (cfg16(header, offset + 2) & (1u << 15))
                return false;
        } else if (id == PCI_CAP_ID_PM) {
            if (offset > 0xf8)
                return false;
            uint16_t pmcsr = cfg16(header, offset + 4);
            if (pmcsr & 3u) {
                /* PME_Status is W1C: write zero to it, request D0. */
                cfg_write16(header, offset + 4, pmcsr & ~(3u | (1u << 15)));
                if (!usb_platform_delay_ms(10) || (cfg16(header, offset + 4) & 3u))
                    return false;
            }
        }
        offset = next;
    }
    return true;
}

bool usb_platform_prepare_pci(pci_device_header_t *header) {
    if (!header || ((uintptr_t)header & 0xfffu) || cfg16(header, 0) == 0xffff ||
        (cfg8(header, 0x0e) & 0x7fu) != 0 || cfg8(header, 0x0b) != 0x0c ||
        cfg8(header, 0x0a) != 3 || cfg8(header, 9) != 0x30 || !usb_platform_delay_ms(0))
        return false;
    if (controller_bar.header)
        return controller_bar.header == header;

    uint16_t original_command = cfg16(header, 4);
    uint16_t quiet = (original_command & ~(PCI_MEM | PCI_MASTER)) | PCI_INTX_OFF;
    cfg_write16(header, 4, quiet);
    if ((cfg16(header, 4) & (PCI_MEM | PCI_MASTER | PCI_INTX_OFF)) != PCI_INTX_OFF ||
        !quiet_capabilities(header))
        return false;

    uint32_t low = cfg32(header, 0x10);
    unsigned type = (low >> 1) & 3u;
    if ((low & 1u) || (type != 0 && type != 2))
        return false;
    uint32_t high = type == 2 ? cfg32(header, 0x14) : 0;
    uint64_t base = (low & ~0xfull) | ((uint64_t)high << 32);

    /* Disable decoding and DMA before probing the BAR aperture. */
    cfg_write32(header, 0x10, UINT32_MAX);
    if (type == 2)
        cfg_write32(header, 0x14, UINT32_MAX);
    uint32_t mask_low = cfg32(header, 0x10);
    uint32_t mask_high = type == 2 ? cfg32(header, 0x14) : 0;
    cfg_write32(header, 0x10, low);
    if (type == 2)
        cfg_write32(header, 0x14, high);

    uint64_t mask = (mask_low & ~0xfull) | ((uint64_t)mask_high << 32);
    uint64_t size = type == 2 ? (~mask + 1ull) : (0x100000000ull - (uint64_t)(mask_low & ~0xfu));
    if ((mask_low & 0xfu) != (low & 0xfu) || !base || !size || (size & (size - 1ull)) ||
        (base & (size - 1ull)) || base >= 0x0000800000000000ull ||
        size > 0x0000800000000000ull - base || (type == 0 && size > 0x100000000ull - base) ||
        cfg32(header, 0x10) != low || (type == 2 && cfg32(header, 0x14) != high)) {
        kprintf("xHCI: invalid or unassigned BAR0 aperture\n\r");
        return false;
    }
    uint16_t command = quiet | PCI_MEM | PCI_MASTER;
    cfg_write16(header, 4, command);
    if ((cfg16(header, 4) & (PCI_MEM | PCI_MASTER | PCI_INTX_OFF)) !=
        (PCI_MEM | PCI_MASTER | PCI_INTX_OFF))
        return false;
    controller_bar.header = header;
    controller_bar.base = base;
    controller_bar.size = size;
    kprintf("xHCI: PCI polling ready; BAR0=0x%llx, bytes=0x%llx\n\r", (unsigned long long)base,
            (unsigned long long)size);
    return true;
}

void *usb_platform_map_mmio(uint64_t phys, size_t bytes) {
    if (!controller_bar.header || !bytes || phys < controller_bar.base)
        return NULL;
    uint64_t offset = phys - controller_bar.base;
    if (offset >= controller_bar.size || (uint64_t)bytes > controller_bar.size - offset) {
        kprintf("xHCI: MMIO request exceeds BAR0 aperture\n\r");
        return NULL;
    }
    return vmem_map_mmio(phys, bytes);
}

bool usb_platform_dma_ready(void) {
    return boot_info.hhdm_offset != 0;
}

uint64_t usb_platform_alloc_dma(size_t pages, bool supports_64bit, void **out) {
    if (!out)
        return 0;
    *out = NULL;
    if (!boot_info.hhdm_offset || !pages || pages > SIZE_MAX / USB_DMA_PAGE_SIZE)
        return 0;
    uint64_t bytes = (uint64_t)pages * USB_DMA_PAGE_SIZE;
    uint64_t phys = pmm_early_alloc_pages(pages);
    uint64_t offset = boot_info.hhdm_offset;
    if (!phys || (phys & (USB_DMA_PAGE_SIZE - 1u)) || phys > UINT64_MAX - bytes ||
        phys > UINT64_MAX - offset || phys + offset > UINT64_MAX - (bytes - 1ull) ||
        (!supports_64bit && (phys >= 0x100000000ull || bytes > 0x100000000ull - phys))) {
        kprintf("xHCI: invalid/unsupported DMA allocation (bump pages retained)\n\r");
        return 0;
    }
    *out = (void *)(uintptr_t)(phys + offset);
    memset(*out, 0, (size_t)bytes);
    return phys;
}
