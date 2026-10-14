#include <drivers/pci/pci.h>
#include <usb_input.h>
#include <video/scanout.h>
#include <drivers/block.h>
#include <acpi/tables.h>
#include <mm/vmem_io.h>
#include <printk.h>
#include <net/network.h>
#include <drivers/audio.h>
#include <drivers/designware_i2c.h>

typedef struct {
    uint64_t base_address;
    uint16_t pci_segment_group;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} __attribute__((packed)) mcfg_allocation_t;

typedef struct {
    _acpi_sdt_hdr_t hdr;
    uint64_t reserved;
    mcfg_allocation_t allocations[];
} __attribute__((packed)) acpi_mcfg_t;

/* Retained for other drivers. This routine is NOT called for polling USB.
 * All register writes use their specified widths, avoiding PCI Status W1C.
 */
_Bool pci_enable_msi(pci_device_header_t *header, uint8_t lapic_id, uint8_t vector) {
    if (!header || ((uintptr_t)header & 0xfffu) || vector < 32 ||
        !(*(volatile uint16_t *)((uintptr_t)header + 6) & PCI_STATUS_CAPABILITIES_LIST))
        return 0;
    _Bool seen[64] = {0};
    unsigned offset = *(volatile uint8_t *)((uintptr_t)header + 0x34);
    while (offset) {
        if (offset < 0x40 || offset > 0xfc || (offset & 3u) || seen[offset >> 2])
            return 0;
        seen[offset >> 2] = 1;
        volatile uint8_t *cap = (volatile uint8_t *)header + offset;
        if (cap[0] == PCI_CAP_ID_MSI) {
            uint16_t control = *(volatile uint16_t *)(cap + 2);
            _Bool wide = (control & (1u << 7)) != 0;
            if (offset > (wide ? 0xf0u : 0xf4u))
                return 0;
            if ((control & (1u << 8)) && offset > (wide ? 0xe8u : 0xecu))
                return 0; /* Check optional per-vector mask register bounds. */
            *(volatile uint16_t *)(cap + 2) = control & ~1u;
            *(volatile uint32_t *)(cap + 4) = 0xfee00000u | ((uint32_t)lapic_id << 12);
            if (wide)
                *(volatile uint32_t *)(cap + 8) = 0;
            *(volatile uint16_t *)(cap + (wide ? 12 : 8)) = vector;
            if (control & (1u << 8)) {
                volatile uint32_t *mask = (volatile uint32_t *)(cap + (wide ? 16 : 12));
                *mask &= ~1u;
            }
            /* MME=0 selects one vector regardless of prior firmware state. */
            *(volatile uint16_t *)(cap + 2) = (control & ~(7u << 4)) | 1u;
            return (*(volatile uint16_t *)(cap + 2) & 1u) != 0;
        }
        offset = cap[1];
    }
    return 0;
}

static volatile pci_device_header_t *map_function(uint64_t ecam, uint8_t bus, uint8_t device,
                                                  uint8_t function) {
    /* MCFG base is bus-zero-relative, including allocations starting > 0. */
    uint64_t offset = ((uint64_t)bus << 20) | ((uint64_t)device << 15) | ((uint64_t)function << 12);
    if (ecam > UINT64_MAX - offset)
        return NULL;
    return vmem_map_mmio(ecam + offset, 4096);
}

static void enumerate_function(uint64_t ecam, uint8_t bus, uint8_t device, uint8_t function) {
    volatile pci_device_header_t *header = map_function(ecam, bus, device, function);
    if (!header || header->vendor_id == 0xffff)
        return;
    kprintf(
        "PCI %02x:%02x.%x | Vendor: %04x, Device: %04x | Class: %02x, Subclass: %02x, ProgIF: %02x\n\r",
        bus, device, function, header->vendor_id, header->device_id, header->class_code,
        header->subclass, header->prog_if);
    if (header->class_code == 0x0c && header->subclass == 3 && header->prog_if == 0x30) {
        kprintf("  -> FOUND xHCI USB CONTROLLER!\n\r");
        if (kernel_usb_register_xhci((pci_device_header_t *)header))
            kprintf("PCI: xHCI registered for USB startup\n\r");
        else
            kprintf("PCI: additional xHCI controller skipped (one supported)\n\r");
    }
    if (header->class_code == 1 && header->subclass == 8 && header->prog_if == 2)
        nvme_probe((pci_device_header_t *)header);
    if (header->class_code == 1 && header->subclass == 6 && header->prog_if == 1)
        ahci_probe((pci_device_header_t *)header);
    if (header->class_code == 3)
        scanout_probe((pci_device_header_t *)header);
    if (header->class_code == 2)
        network_probe((pci_device_header_t *)header);
    if (header->class_code == 4 && (header->subclass == 1 || header->subclass == 3))
        audio_probe((pci_device_header_t *)header);
    if (header->vendor_id == 0x8086 && header->device_id == 0x7f4e)
        designware_i2c_probe((pci_device_header_t *)header);
}

static void enumerate_device(uint64_t ecam, uint8_t bus, uint8_t device) {
    volatile pci_device_header_t *header = map_function(ecam, bus, device, 0);
    if (!header || header->vendor_id == 0xffff)
        return;
    _Bool multifunction = (header->header_type & 0x80u) != 0;
    enumerate_function(ecam, bus, device, 0);
    if (multifunction) {
        for (uint8_t function = 1; function < 8; ++function)
            enumerate_function(ecam, bus, device, function);
    }
}

void *pci_config_space(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function) {
    if (slot >= 32 || function >= 8)
        return NULL;
    acpi_mcfg_t *mcfg = acpi_locate_table("MCFG");
    if (!mcfg || mcfg->hdr.len < sizeof(*mcfg) ||
        (mcfg->hdr.len - sizeof(*mcfg)) % sizeof(mcfg_allocation_t))
        return NULL;
    size_t count = (mcfg->hdr.len - sizeof(*mcfg)) / sizeof(mcfg_allocation_t);
    for (size_t i = 0; i < count; i++) {
        mcfg_allocation_t *a = &mcfg->allocations[i];
        if (a->pci_segment_group == segment && a->start_bus <= bus && bus <= a->end_bus &&
            a->base_address && !(a->base_address & 0xfffffULL))
            return map_function(a->base_address, bus, slot, function);
    }
    return NULL;
}

void pci_init(void) {
    kprintf("Initializing PCIe Subsystem via ECAM...\n\r");
    acpi_mcfg_t *mcfg = (acpi_mcfg_t *)acpi_locate_table("MCFG");
    if (!mcfg || mcfg->hdr.len < sizeof(*mcfg) ||
        (mcfg->hdr.len - sizeof(*mcfg)) % sizeof(mcfg_allocation_t)) {
        kprintf("PCI: missing or invalid MCFG; ECAM unavailable\n\r");
        return;
    }
    size_t count = (mcfg->hdr.len - sizeof(*mcfg)) / sizeof(mcfg_allocation_t);
    for (size_t i = 0; i < count; ++i) {
        mcfg_allocation_t *allocation = &mcfg->allocations[i];
        if (!allocation->base_address || (allocation->base_address & 0xfffffull) ||
            allocation->start_bus > allocation->end_bus) {
            kprintf("PCI: invalid MCFG allocation skipped\n\r");
            continue;
        }
        kprintf("PCIe Segment Group %u, Base: %llx, Bus: %u to %u\n\r",
                allocation->pci_segment_group, (unsigned long long)allocation->base_address,
                allocation->start_bus, allocation->end_bus);
        for (uint16_t bus = allocation->start_bus; bus <= allocation->end_bus; ++bus) {
            for (uint8_t device = 0; device < 32; ++device)
                enumerate_device(allocation->base_address, (uint8_t)bus, device);
        }
    }
    kprintf("PCIe Enumeration Complete.\n\r");
}
