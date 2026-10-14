#ifndef _ACPI_H
#define _ACPI_H 1

#include <kernel.h>
#include <string.h>
#include <io.h>
#include <stdint.h>
#include <stddef.h>

typedef unsigned char acpi_status_t;

#define ACPI_SUCCESS 0
#define ACPI_INTEGRITY 1
#define ACPI_MEMORY 2
#define ACPI_NO_TABLE 3

// Global ACPI table pointers shared across modules.
extern struct _acpi_fadt_t *fadt;

// Public functions
void *acpi_locate_table(const char *table);
size_t acpi_table_count(void);
void *acpi_table_at(size_t index);
void *acpi_dsdt(void);
_Bool acpi_init(unsigned long long *rsdp_ptr);

// Utility checksum function (used internally)
unsigned int acpi_checksum(void *ptr);

#endif
