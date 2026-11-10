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
#include <acpica.h>
#include <acpi/osl.h>
#include <acpi/power.h>
#include <power.h>
#include <mm/pmm.h>
#include <mm/user.h>
#include <mm/vmem_io.h>

_Static_assert(offsetof(_acpi_fadt_t, x_firmware_ctrl) == offsetof(ACPI_TABLE_FADT, XFacs),
               "kernel FADT must match ACPICA FACS offsets");
_Static_assert(offsetof(_acpi_fadt_t, x_dsdt) == offsetof(ACPI_TABLE_FADT, XDsdt),
               "kernel FADT must match ACPICA DSDT offsets");

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
    if (!acpi_os_mark_firmware_memory(phys, hdr->len))
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

    /* Table discovery is needed by HPET/APIC/PCI before the event subsystem
     * can be enabled. AML initialization follows those controllers. */
    uint64_t physical_rsdp;
    if (!vmem_translate((uintptr_t)rsdp, &physical_rsdp))
        PANIC("ACPI Init: RSDP is not mapped");
    /* The ACPICA table manager temporarily maps both legacy and extended
     * FACS addresses while parsing the FADT, before choosing either one. */
    if (fadt->firmware_ctl && !acpi_os_mark_firmware_memory(fadt->firmware_ctl, 64))
        panic("ACPICA: invalid legacy FACS physical range");
    if (fadt->hdr.len >= offsetof(_acpi_fadt_t, x_firmware_ctrl) +
                         sizeof(fadt->x_firmware_ctrl) && fadt->x_firmware_ctrl &&
        !acpi_os_mark_firmware_memory(fadt->x_firmware_ctrl, 64))
        panic("ACPICA: invalid extended FACS physical range");
    acpi_os_set_root_pointer(physical_rsdp);
    /* Register every validated SDT, including entries after the FADT. */
    for (size_t i = 0; i < acpi_table_count(); i++)
        (void)acpi_table_at(i);
    ACPI_STATUS status = AcpiInitializeSubsystem();
    if (ACPI_FAILURE(status))
        panic("ACPICA subsystem: %s", AcpiFormatException(status));
    status = AcpiInitializeTables(NULL, 32, FALSE);
    if (ACPI_FAILURE(status))
        panic("ACPICA tables: %s", AcpiFormatException(status));

    kprintf("ACPICA %08x: table manager initialized.\n\r", ACPI_CA_VERSION);
    return 1;
}


/* Firmware power requests are reported until a system-wide shutdown policy
 * exists. The fixed event is acknowledged by ACPICA before LAPIC EOI. */
static UINT32 power_button(void *context) {
    (void)context;
    power_button_event();
    pr_info("ACPICA: fixed power button event received\n");
    return ACPI_INTERRUPT_HANDLED;
}

static ACPI_STATUS count_device(ACPI_HANDLE object, UINT32 level, void *context,
                                void **result) {
    (void)object;
    (void)level;
    (void)result;
    ++*(unsigned *)context;
    return AE_OK;
}

bool acpi_start(void) {
    if (!fadt)
        return false;
    /* GPE block creation discovers namespace _Lxx/_Exx methods, so load
     * AML before initializing event blocks. */
    ACPI_STATUS status = AcpiLoadTables();
    if (ACPI_FAILURE(status))
        panic("ACPICA namespace: %s", AcpiFormatException(status));
    status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(status))
        panic("ACPICA hardware/events: %s", AcpiFormatException(status));
    if (!acpi_os_prepare_gpes())
        panic("ACPICA: cannot reserve boot GPE topology admission");

    /* Firmware can own the FACS global lock when boot AML acquires it.
     * Its release completes through the SCI, so allow interrupts now that
     * the SCI/global-lock handlers are installed. The LAPIC timer stays
     * masked until sched_start(); deferred AML remains queued for workers.
     * Keep SCI available for later boot consumers such as audio _CRS too.
     * kernel_main restores CLI before setting up the scheduler and APs. */
    __asm__ volatile("sti" ::: "memory");
    uint64_t boot_irq_flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(boot_irq_flags));
    if (!(boot_irq_flags & 0x200))
        panic("ACPICA: SCI interrupts disabled before boot AML");
    pr_info("ACPICA: boot AML SCI interrupts enabled (IF=1)\n");

    /* APIC routing is active. Inform AML which interrupt model the OS uses;
     * absence of _PIC is valid, but failure of a present method is reported. */
    ACPI_OBJECT pic = {.Integer = {.Type = ACPI_TYPE_INTEGER, .Value = 1}};
    ACPI_OBJECT_LIST arguments = {1, &pic};
    status = AcpiEvaluateObject(NULL, "\\_PIC", &arguments, NULL);
    if (ACPI_FAILURE(status) && status != AE_NOT_FOUND)
        pr_warn("ACPICA: _PIC(1) failed: %s\n", AcpiFormatException(status));

    status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(status))
        panic("ACPICA objects: %s", AcpiFormatException(status));
    if (!acpi_os_prepare_runtime())
        panic("ACPICA: runtime preparation failed");
    status = AcpiUpdateAllGpes();
    if (ACPI_FAILURE(status))
        panic("ACPICA runtime GPEs: %s", AcpiFormatException(status));
    unsigned devices = 0;
    status = AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT, ACPI_UINT32_MAX,
                               count_device, NULL, &devices, NULL);
    if (ACPI_FAILURE(status))
        panic("ACPICA namespace walk: %s", AcpiFormatException(status));
    pr_info("ACPICA: namespace initialized, %u devices; _REG/_STA/_INI pass finished (individual firmware errors reported separately)\n", devices);

    UINT8 a, b;
    status = AcpiGetSleepTypeData(ACPI_STATE_S5, &a, &b);
    if (ACPI_SUCCESS(status))
        pr_info("ACPICA: _S5 evaluated, sleep types %u/%u\n", a, b);
    else
        pr_info("ACPICA: S5 unavailable: %s\n", AcpiFormatException(status));
    status = AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON, power_button, NULL);
    if (ACPI_FAILURE(status))
        pr_warn("ACPICA: fixed power button unavailable: %s\n", AcpiFormatException(status));
    pr_info("ACPICA: hardware, SCI and AML initialization complete\n");
    acpi_power_init();
    acpi_selftest_init();
    return true;
}
