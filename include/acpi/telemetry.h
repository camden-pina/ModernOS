#ifndef COAST_ACPI_TELEMETRY_H
#define COAST_ACPI_TELEMETRY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Boot lifetime, after sched_init()/OSL worker creation, before sched_start().
 * Enumeration and all firmware evaluations run on the dedicated BSP0 task.
 * No EC/SMBus/GPIO backend, battery policy, or hot-added namespace support. */
bool acpi_telemetry_start(void);
/* Cached, bounded text only: never evaluates AML, allocates, or waits. */
size_t acpi_telemetry_info(char *out, size_t cap);
/* A nonzero, increasing generation closes evaluation admission. idle means
 * the matching freeze is still in force and the worker has actually drained.
 * A stuck firmware method remains busy; there is no pretend cancellation.
 * Stale thaw/freeze generations cannot reopen or replace a newer freeze. */
void acpi_telemetry_freeze(uint64_t epoch);
bool acpi_telemetry_idle(uint64_t epoch);
void acpi_telemetry_thaw(uint64_t epoch);
#endif
