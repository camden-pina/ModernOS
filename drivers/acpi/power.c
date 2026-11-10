/* Narrow BSP-only terminal backend. No namespace teardown or reset fallback. */
#include <acpica.h>
#include <acpi/power.h>
#include <acpi/osl.h>
#include <acpi/acpi.h>
#include <sched.h>
#include <sync.h>
#include <uapi/abi.h>
#include <panic.h>

static bool initialized, s5_advertised;
static struct acpi_runtime_status runtime;

void acpi_power_init(void) {
    if (sched_ready())
        return;
    acpi_os_get_runtime_status(&runtime);
    UINT8 a, b;
    s5_advertised = ACPI_SUCCESS(AcpiGetSleepTypeData(ACPI_STATE_S5, &a, &b));
    initialized = true;
}

bool acpi_power_available(unsigned action) {
    if (!initialized || !runtime.prepared)
        return false;
    return action == OS_POWER_OFF ? s5_advertised && runtime.sleep_mapping_ready :
           action == OS_POWER_RESET && runtime.reset_mapping_ready;
}

static void require_bsp(bool irq_enabled) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    if (!sched_ready() || this_cpu()->id || !!(flags & 0x200) != irq_enabled)
        panic("power: ACPI backend CPU/interrupt invariant");
}

uint32_t acpi_power_prepare(unsigned action) {
    require_bsp(true);
    if (!acpi_power_available(action))
        return AE_SUPPORT;
    /* _PTS/_SST may need Sleep, SCI, global-lock and event-worker progress. */
    return action == OS_POWER_OFF ? AcpiEnterSleepStatePrep(ACPI_STATE_S5) : AE_OK;
}

uint32_t acpi_power_disable_gpes(void) {
    require_bsp(true);
    return AcpiDisableAllGpes();
}

uint32_t acpi_power_restore_gpes(void) {
    require_bsp(true);
    /* No _WAK/S0 fiction: aborted arbitrary _PTS side effects aren't proven
       reversible. This restores only runtime GPE service/software admission. */
    return AcpiEnableAllRuntimeGpes();
}

uint32_t acpi_power_enter(unsigned action) {
    require_bsp(false);
    if (!acpi_power_available(action))
        return AE_SUPPORT;
    return action == OS_POWER_OFF ? AcpiEnterSleepState(ACPI_STATE_S5) : AcpiReset();
}

size_t acpi_power_info(char *out, size_t cap) {
    int n = snprintf(out, cap,
        "ACPICA runtime=%s version=%08x\nfirmware_tables=%u\n"
        "S5_advertised=%u reset_register_ready=%u sleep_registers_ready=%u\n"
        "runtime_regions=%u prepared=%u failed=%u deferred=%u fixed_failures=%u\n"
        "Fixed-register readiness does not prove _PTS or physical hardware support.\n"
        "Unsupported: suspend/resume, EC/GPIO/GenericSerialBus handlers, thermal/CPU policy, PCI _PRT.\n",
        initialized ? "initialized" : "unavailable", ACPI_CA_VERSION,
        initialized ? (unsigned)acpi_table_count() : 0, s5_advertised,
        runtime.reset_mapping_ready, runtime.sleep_mapping_ready,
        runtime.memory_regions, runtime.memory_regions_prepared,
        runtime.memory_region_failures, runtime.deferred_regions,
        runtime.fixed_mapping_failures);
    return n < 0 || !cap ? 0 : min((size_t)n, cap - 1);
}
