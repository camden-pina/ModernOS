// acpi.c
// ##############################
//         ACPI Initialization and Host Interface
// ##############################

#include <acpi/acpi.h>
#include <stdint.h>
#include <string.h>
#include <acpi/tables.h>
#include <printk.h>
#include <panic.h>
#include <lai/core.h>
#include <mm/pmm.h>
#include <mm/user.h>
#include <mm/vmem_io.h>
#include <drivers/pci/pci.h>
#include <tsc.h>

/* Firmware table addresses are physical, while all kernel pointers are
   in the shared higher half. Validate and map each table before accessing it. */
void *acpi_phys_table(uint64_t phys) {
    if (!phys || phys >= 0x0000800000000000ULL)
        return NULL;
    uint64_t hhdm = (uintptr_t)phys_ptr(0), root = kernel_cr3();
    uint64_t start = phys & ~4095ULL, head_end = (phys + sizeof(_acpi_sdt_hdr_t) + 4095) & ~4095ULL;
    for (uint64_t p = start; p < head_end; p += 4096)
        if (!vmem_map_page_in_root(root, p + hhdm, p))
            return NULL;
    _acpi_sdt_hdr_t *hdr = phys_ptr(phys);
    if (hdr->len < sizeof(*hdr) || hdr->len > 1024 * 1024)
        return NULL;
    uint64_t end = (phys + hdr->len + 4095) & ~4095ULL;
    for (uint64_t p = head_end; p < end; p += 4096)
        if (!vmem_map_page_in_root(root, p + hhdm, p))
            return NULL;
    return hdr;
}

// Global ACPI table pointers
static _acpi_sdt_hdr_t *root_table;
static size_t root_entry_size;
struct _acpi_fadt_t *fadt = NULL;

// ACPI variables (for power management, etc.)
static unsigned int acpi_smi_cmd;
static unsigned char acpi_enable;
static unsigned char acpi_disable;
static unsigned int acpi_pm1a_cnt;
static unsigned int acpi_pm1b_cnt;
static unsigned short acpi_slp_typ_a;
static unsigned short acpi_slp_typ_b;
static unsigned short acpi_slp_en;
static unsigned short acpi_sci_en;
static unsigned char acpi_pm1_cnt_len;

// Calculate checksum for an ACPI table.
// Returns nonzero (true) if the table's bytes sum to zero.
unsigned int acpi_checksum(void *ptr) {
    _acpi_sdt_hdr_t *hdr = (_acpi_sdt_hdr_t *)ptr;
    unsigned char *bytes = (unsigned char *)ptr;
    unsigned char sum = 0;
    for (unsigned int i = 0; i < hdr->len; i++) {
        sum += bytes[i];
    }
    return (sum == 0);
}

size_t acpi_table_count(void) {
    return root_table ? (root_table->len - sizeof(*root_table)) / root_entry_size : 0;
}

void *acpi_table_at(size_t index) {
    if (index >= acpi_table_count())
        return NULL;
    uint64_t address = 0;
    /* Entries begin at offset 36, so an XSDT address may be unaligned. */
    memcpy(&address, (uint8_t *)root_table + sizeof(*root_table) + index * root_entry_size,
           root_entry_size);
    _acpi_sdt_hdr_t *hdr = acpi_phys_table(address);
    return hdr && acpi_checksum(hdr) ? hdr : NULL;
}

void *acpi_dsdt(void) {
    if (!fadt || fadt->hdr.len < offsetof(_acpi_fadt_t, dsdt) + sizeof(fadt->dsdt))
        return NULL;
    uint64_t address = fadt->dsdt;
    if (fadt->hdr.len >= offsetof(_acpi_fadt_t, x_dsdt) + sizeof(fadt->x_dsdt) && fadt->x_dsdt)
        address = fadt->x_dsdt;
    _acpi_sdt_hdr_t *hdr = acpi_phys_table(address);
    return hdr && !memcmp(hdr->signature, "DSDT", 4) && acpi_checksum(hdr) ? hdr : NULL;
}

// Locate an ACPI table by signature within the selected RSDT or XSDT.
void *acpi_locate_table(const char *table) {
    for (size_t e = 0; e < acpi_table_count(); e++) {
        _acpi_sdt_hdr_t *hdr = acpi_table_at(e);
        if (hdr && memcmp(hdr->signature, table, 4) == 0)
            return (void *)hdr;
    }
    return NULL;
}

// Initialize ACPI using the provided RSDP pointer.
_Bool acpi_init(unsigned long long *rsdp_ptr) {
    acpi_smi_cmd = 0;
    acpi_enable = 0;
    acpi_disable = 0;
    acpi_pm1a_cnt = 0;
    acpi_pm1b_cnt = 0;
    acpi_slp_typ_a = 0;
    acpi_slp_typ_b = 0;
    acpi_slp_en = 0;
    acpi_sci_en = 0;
    acpi_pm1_cnt_len = 0;

    root_table = NULL;
    fadt = NULL;

    kprintf("Initializing ACPI...\n\r");

    _acpi_rsdp_t *rsdp = (_acpi_rsdp_t *)rsdp_ptr;
    if (!rsdp || memcmp(rsdp->signature, "RSD PTR ", 8))
        PANIC("ACPI Init: Invalid RSDP");
    unsigned char sum = 0;
    for (unsigned i = 0; i < 20; i++)
        sum += ((unsigned char *)rsdp)[i];
    if (sum)
        PANIC("ACPI Init: Invalid RSDP checksum");
    if (rsdp->rev >= 2) {
        if (rsdp->length < sizeof(*rsdp) || rsdp->length > 4096)
            PANIC("ACPI Init: Invalid RSDP length");
        for (unsigned i = 20; i < rsdp->length; i++)
            sum += ((unsigned char *)rsdp)[i];
        if (sum)
            PANIC("ACPI Init: Invalid extended RSDP checksum");
    }
    bool extended = rsdp->rev >= 2 && rsdp->xsdt_ptr;
    root_entry_size = extended ? 8 : 4;
    root_table = acpi_phys_table(extended ? rsdp->xsdt_ptr : rsdp->rsdt_ptr);
    if (!root_table || memcmp(root_table->signature, extended ? "XSDT" : "RSDT", 4) ||
        (root_table->len - sizeof(*root_table)) % root_entry_size || !acpi_checksum(root_table))
        PANIC("ACPI Init: Invalid root table");
    kprintf("ACPI: using %s\n\r", extended ? "XSDT" : "RSDT");

    kprintf("Locating FADT...\n\r");
    fadt = (_acpi_fadt_t *)acpi_locate_table("FACP");
    if (!fadt)
        PANIC("ACPI Init: FADT not found");

    kprintf("Verifying FADT checksum...\n\r");
    if (!acpi_checksum(fadt))
        PANIC("ACPI Init: Invalid FADT Checksum");

    // Retrieve DSDT pointer from FADT (prefer extended address if available)
    void *dsdt_ptr = acpi_dsdt();
    kprintf("Verifying DSDT checksum...\n\r");
    if (!dsdt_ptr || !acpi_checksum(dsdt_ptr))
        PANIC("ACPI Init: Invalid DSDT Checksum");

    // Initialize the LAI library for AML interpretation.
    lai_set_acpi_revision(rsdp->rev ? 2 : 1);
    lai_create_namespace();

    kprintf("ACPI initialization complete.\n\r");
    return 1;
}

// =====================================================
// LAI Host Functions
// (These functions allow the LAI library to interface with your OS.)
// They are currently stubs and should be filled with real implementations.
// =====================================================

void *laihost_malloc(size_t sz) {
    return kmalloc(sz);
}

void *laihost_realloc(void *ptr, size_t newsize, size_t oldsize) {
    return krealloc(ptr, newsize);
}

void laihost_free(void *ptr, size_t sz) {
    kfree(ptr);
}

void laihost_log(int level, const char *message) {
    kprintf("[%d] %s", level, message);
}

void laihost_panic(const char *message) {
    panic("%s", message);
}

void *laihost_scan(char *signature, size_t index) {
    kprintf("laihost_scan() [%s], [%llu]\n", signature, index);
    // Special-case: DSDT must be obtained from the FADT.
    if (memcmp(signature, "DSDT", 4) == 0) {
        if (!fadt) {
            kprintf("laihost_scan: FADT not found, cannot locate DSDT.\n");
            return NULL;
        }
        void *dsdt_ptr = acpi_dsdt();
        if (dsdt_ptr && acpi_checksum(dsdt_ptr)) {
            kprintf("laihost_scan: DSDT found.\n");
            return dsdt_ptr;
        } else {
            kprintf("laihost_scan: DSDT checksum failed or not found.\n");
            return NULL;
        }
    }
    void *ptr = NULL;
    acpi_status_t status = acpi_table_find(&ptr, signature, index);
    if (status == ACPI_SUCCESS) {
        kprintf("laihost_scan: ACPI table %4.4s found at index %llu\n", signature, index);
        return ptr;
    } else {
        kprintf("laihost_scan: Failed to find ACPI table %4.4s at index %llu\n", signature, index);
        return NULL;
    }
}

void *laihost_map(size_t address, size_t size) {
    /* Firmware SystemMemory fields include the real ACPI NVS variables used
     * by _CRS. Map them and device regions as supervisor-only MMIO. */
    if (!size || size > 16 * 1024 * 1024)
        return NULL;
    return vmem_map_mmio(address, size);
}

void laihost_unmap(void *addr, size_t size) {
    /* MMIO mappings use the shared higher-half direct map; they are borrowed
     * rather than allocated virtual ranges. Other drivers may retain them. */
    (void)addr;
    (void)size;
}

void laihost_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1" ::"a"(value), "Nd"(port) : "memory");
}

void laihost_outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0,%1" ::"a"(value), "Nd"(port) : "memory");
}

void laihost_outd(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0,%1" ::"a"(value), "Nd"(port) : "memory");
}

uint8_t laihost_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

uint16_t laihost_inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1,%0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

uint32_t laihost_ind(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1,%0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

void laihost_pci_writeb(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function,
                        uint16_t offset, uint8_t value) {
    uint8_t *config = pci_config_space(segment, bus, slot, function);
    if (config && offset < 4096)
        *(volatile uint8_t *)(config + offset) = value;
}

uint8_t laihost_pci_readb(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function,
                          uint16_t offset) {
    uint8_t *config = pci_config_space(segment, bus, slot, function);
    return config && offset < 4096 ? *(volatile uint8_t *)(config + offset) : UINT8_MAX;
}

void laihost_pci_writew(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function,
                        uint16_t offset, uint16_t value) {
    uint8_t *config = pci_config_space(segment, bus, slot, function);
    if (config && offset <= 4094 && !(offset & 1))
        *(volatile uint16_t *)(config + offset) = value;
}

uint16_t laihost_pci_readw(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function,
                           uint16_t offset) {
    uint8_t *config = pci_config_space(segment, bus, slot, function);
    return config && offset <= 4094 && !(offset & 1) ? *(volatile uint16_t *)(config + offset)
                                                     : UINT16_MAX;
}

void laihost_pci_writed(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function,
                        uint16_t offset, uint32_t value) {
    uint8_t *config = pci_config_space(segment, bus, slot, function);
    if (config && offset <= 4092 && !(offset & 3))
        *(volatile uint32_t *)(config + offset) = value;
}

uint32_t laihost_pci_readd(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function,
                           uint16_t offset) {
    uint8_t *config = pci_config_space(segment, bus, slot, function);
    return config && offset <= 4092 && !(offset & 3) ? *(volatile uint32_t *)(config + offset)
                                                     : UINT32_MAX;
}

void laihost_sleep(uint64_t duration) {
    uint64_t us = duration > UINT64_MAX / 1000 ? UINT64_MAX : duration * 1000,
             start = tsc_uptime_us();
    while (tsc_uptime_us() - start < us)
        __asm__ volatile("pause");
}

uint64_t laihost_timer(void) {
    return tsc_uptime_us() * 10; /* ACPI Timer() units are 100 ns. */
}

int laihost_sync_wait(struct lai_sync_state *state, unsigned int val, int64_t deadline) {
    return 0;
}

void laihost_sync_wake(struct lai_sync_state *state) {
    // TODO: Implement synchronization wake.
}

void laihost_handle_amldebug(lai_variable_t *variable) {
    // TODO: Implement AML debugging functionality.
}

void laihost_handle_global_notify(lai_nsnode_t *node, int event) {
    // TODO: Implement global notify handling.
}
