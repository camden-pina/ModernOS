/* Coast freestanding x86-64 port. Derived from ACPICA platform interfaces.
 * The ACPICA runtime is compiled with its own prefixed C-library functions;
 * it never links against host libc or replaces Coast's string functions. */
#ifndef __ACCOAST_H__
#define __ACCOAST_H__
#include <stdint.h>
#include <stdbool.h>
#define ACPI_MACHINE_WIDTH 64
#define ACPI_USE_LOCAL_CACHE
#define USE_NATIVE_ALLOCATE_ZEROED
#define ACPI_MUTEX_TYPE ACPI_BINARY_SEMAPHORE
#define ACPI_USE_NATIVE_MATH64
#define ACPI_FLUSH_CPU_CACHE() __asm__ volatile("wbinvd" ::: "memory")
bool acpi_os_acquire_global_lock(volatile uint32_t *lock);
bool acpi_os_release_global_lock(volatile uint32_t *lock);
#define ACPI_ACQUIRE_GLOBAL_LOCK(facs, acquired) \
    ((acquired) = acpi_os_acquire_global_lock((volatile uint32_t *) \
        ((uint8_t *)(facs) + 16)))
#define ACPI_RELEASE_GLOBAL_LOCK(facs, pending) \
    ((pending) = acpi_os_release_global_lock((volatile uint32_t *) \
        ((uint8_t *)(facs) + 16)))
#ifdef COAST_ACPICA_INTERNAL
#define memcmp AcpiModernMemcmp
#define memcpy AcpiModernMemcpy
#define memmove AcpiModernMemmove
#define memset AcpiModernMemset
#define strlen AcpiModernStrlen
#define strcpy AcpiModernStrcpy
#define strncpy AcpiModernStrncpy
#define strcmp AcpiModernStrcmp
#define strncmp AcpiModernStrncmp
#define strchr AcpiModernStrchr
#define strcat AcpiModernStrcat
#define strncat AcpiModernStrncat
#define strstr AcpiModernStrstr
#define strtoul AcpiModernStrtoul
#define strpbrk AcpiModernStrpbrk
#define strtok AcpiModernStrtok
#define toupper AcpiModernToupper
#define tolower AcpiModernTolower
#define vsnprintf AcpiModernVsnprintf
#define snprintf AcpiModernSnprintf
#define sprintf AcpiModernSprintf
#endif
#endif
