#ifndef COAST_ACPI_POWER_H
#define COAST_ACPI_POWER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void acpi_power_init(void);
bool acpi_power_available(unsigned action);
uint32_t acpi_power_prepare(unsigned action);
uint32_t acpi_power_disable_gpes(void);
uint32_t acpi_power_restore_gpes(void);
uint32_t acpi_power_enter(unsigned action);
size_t acpi_power_info(char *, size_t);
#endif
