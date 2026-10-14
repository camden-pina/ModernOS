#ifndef MODERNOS_BOOT_SMP_H
#define MODERNOS_BOOT_SMP_H
#include <stdint.h>
#include <stdbool.h>

/* Boot adapters describe processors, but the scheduler owns their stacks and
 * initialization. loader_data is opaque outside the adapter that supplied it. */
struct boot_cpu {
    uint32_t processor_id, lapic_id;
    void *loader_data;
};

typedef void (*boot_ap_entry)(struct boot_cpu *, uintptr_t argument);
struct boot_smp {
    uint32_t bsp_lapic_id;
    uint64_t cpu_count;
    struct boot_cpu *cpus;
    bool (*start)(struct boot_smp *, struct boot_cpu *, boot_ap_entry, uintptr_t argument,
                  uintptr_t stack_top);
};

/* Called after ACPI and the BSP local APIC have been initialized. */
struct boot_smp *boot_smp_native_init(void);
/* Adapters can enter a generic AP callback on its scheduler-owned stack. */
void boot_ap_enter(boot_ap_entry, struct boot_cpu *, uintptr_t argument, uintptr_t stack_top)
    __attribute__((__noreturn__));
#endif
