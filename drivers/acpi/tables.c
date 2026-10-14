// tables.c
#include <acpi/tables.h>
#include <printk.h>
#include <panic.h>
#include <string.h>

// Note: acpi_checksum is defined in acpi.c and declared in acpi.h.
extern unsigned int acpi_checksum(void *ptr);

acpi_status_t acpi_table_find(void **table, const char *signature, size_t idx) {
    kassert(table != NULL);
    kassert(signature != NULL);

    _acpi_sdt_hdr_t *hdr;
    size_t curr_idx = 0;
    size_t table_count = acpi_table_count();

    kprintf("Total Tables: %llu\n", table_count);
    kprintf("Searching for ACPI table with signature %4.4s at index %llu\n", signature, idx);

    for (size_t i = 0; i < table_count; i++) {
        hdr = acpi_table_at(i);
        if (!hdr)
            continue;
        kprintf("Table signature: %4.4s\n", hdr->signature);

        if (memcmp(hdr->signature, signature, 4) == 0) {
            if (curr_idx == idx) {
                *table = (void *)hdr;
                kprintf("ACPI table %4.4s found at index %llu\n", signature, idx);
                return ACPI_SUCCESS;
            }
            curr_idx++;
        }
    }

    kprintf("ACPI table %4.4s not found at index %llu\n", signature, idx);
    return ACPI_NO_TABLE;
}

void *acpi_scan(const char *name, size_t idx) {
    kassert(name != NULL);
    // Special-case: DSDT is obtained from the FADT.
    if (memcmp(name, "DSDT", 4) == 0) {
        if (!fadt) {
            kprintf("acpi_scan: FADT not found, cannot locate DSDT.\n");
            return NULL;
        }
        void *dsdt_ptr = acpi_dsdt();
        if (dsdt_ptr && acpi_checksum(dsdt_ptr)) {
            kprintf("acpi_scan: DSDT found.\n");
            return dsdt_ptr;
        } else {
            kprintf("acpi_scan: DSDT checksum failed or not found.\n");
            return NULL;
        }
    }

    void *ptr = NULL;
    acpi_status_t status = acpi_table_find(&ptr, name, idx);
    if (status == ACPI_SUCCESS) {
        kprintf("acpi_scan: ACPI table %4.4s found at index %llu\n", name, idx);
        return ptr;
    } else {
        kprintf("acpi_scan: Failed to find ACPI table %4.4s at index %llu\n", name, idx);
        return NULL;
    }
}
