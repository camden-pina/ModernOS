#ifndef COAST_ACPI_OSL_H
#define COAST_ACPI_OSL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void acpi_os_set_root_pointer(uint64_t physical_rsdp);
/* Register validated tables/FACS before ACPICA maps reserved firmware RAM. */
bool acpi_os_mark_firmware_memory(uint64_t physical, size_t length);
/* BSP-only, after object initialization and before runtime GPE enable/APs.
 * Optional unsupported memory regions are reported; GPE admission must succeed. */
bool acpi_os_prepare_runtime(void);
/* Reserve immediately after AcpiEnableSubsystem, before _PIC/_INI can bind or
 * enable new GPE methods. Runtime preparation depends on this reservation. */
bool acpi_os_prepare_gpes(void);
struct acpi_runtime_status {
    uint32_t memory_regions, memory_regions_prepared, memory_region_failures;
    uint32_t deferred_regions;
    uint32_t fixed_mapping_failures;
    uint64_t prepared_pages;
    /* prepared means the pass finished, not that every optional region mapped.
     * Fixed-register readiness does not establish _PTS/device/physical support. */
    bool prepared, reset_mapping_ready, sleep_mapping_ready;
};
void acpi_os_get_runtime_status(struct acpi_runtime_status *status);
struct acpi_os_diagnostics {
    uint64_t mapping_rejections, notification_rejections, event_rejections;
    uint64_t gpe_rejections, notification_high_water, event_high_water;
    uint32_t gpe_capacity, event_workers;
};
void acpi_os_get_diagnostics(struct acpi_os_diagnostics *diagnostics);
/* Internal boot-time bridge: reserve dispatch AND ordered finish admission for
 * every configured GpeEventInfo. Topology mutation/unload is unsupported. */
bool acpi_os_reserve_gpes(void *const *contexts, size_t count);
/* Call after sched_init(), before sched_start(). Callbacks never run in IRQs. */
bool acpi_os_start_workers(void);
/* Reversible power admission, distinct from permanent AcpiOsTerminate. */
bool acpi_os_power_idle(void);
bool acpi_os_power_freeze(void);
void acpi_os_power_thaw(void);
bool acpi_os_acquire_global_lock(volatile uint32_t *lock);
bool acpi_os_release_global_lock(volatile uint32_t *lock);

#endif
