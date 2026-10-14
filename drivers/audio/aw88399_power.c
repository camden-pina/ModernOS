#include <drivers/aw88399_power.h>
#include <lai/core.h>
#include <mm/vmem_io.h>
#include <printk.h>
#include <tsc.h>

static bool eval_string(lai_nsnode_t *device, const char *name, const char *expected,
                        lai_state_t *state) {
    LAI_CLEANUP_VAR lai_variable_t value = LAI_VAR_INITIALIZER;
    lai_nsnode_t *node = lai_resolve_path(device, name);
    return node && !lai_eval(&value, node, state) && lai_obj_get_type(&value) == LAI_TYPE_STRING &&
           !strcmp(lai_exec_string_access(&value), expected);
}

static bool eval_buffer(lai_nsnode_t *device, lai_variable_t *value, lai_state_t *state) {
    lai_nsnode_t *node = lai_resolve_path(device, "_CRS");
    return node && !lai_eval(value, node, state) && lai_obj_get_type(value) == LAI_TYPE_BUFFER;
}

static uint32_t gpio_read(void *context, unsigned offset) {
    return *(volatile uint32_t *)((uint8_t *)context + offset);
}

static void gpio_write(void *context, unsigned offset, uint32_t value) {
    *(volatile uint32_t *)((uint8_t *)context + offset) = value;
}

static void gpio_delay(unsigned us) {
    uint64_t start = tsc_uptime_us();
    while (tsc_uptime_us() - start < us)
        __asm__ volatile("pause");
}

bool aw88399_power_init(void) {
    LAI_CLEANUP_STATE lai_state_t state;
    lai_init_state(&state);
    LAI_CLEANUP_VAR lai_variable_t speaker_crs = LAI_VAR_INITIALIZER,
                                   gpio_crs = LAI_VAR_INITIALIZER;
    lai_nsnode_t *speaker = lai_resolve_path(NULL, "\\_SB.PC02.I2C2.SPKR");
    lai_nsnode_t *gpio = lai_resolve_path(NULL, "\\_SB.PGPI");
    struct aw_power_resources resources;
    uint64_t physical;
    if (!speaker || !gpio || !eval_string(speaker, "_HID", "AWDZ8399", &state) ||
        !eval_string(speaker, "_SUB", "17AA3907", &state) ||
        !eval_string(gpio, "_HID", "INTC1084", &state) ||
        !eval_buffer(speaker, &speaker_crs, &state) ||
        !aw_power_parse(lai_exec_buffer_access(&speaker_crs), lai_exec_buffer_size(&speaker_crs),
                        &resources) ||
        !eval_buffer(gpio, &gpio_crs, &state) ||
        !aw_power_window(lai_exec_buffer_access(&gpio_crs), lai_exec_buffer_size(&gpio_crs),
                         &physical))
        goto failed;
    /* _CRS contains the actual reset, not the separate interrupt GPIO. */
    if (resources.reset_pin != 140 || resources.pull != 1)
        goto failed;
    void *registers = vmem_map_mmio(physical, 65536);
    if (!registers)
        goto failed;
    pr_info("Audio: AW88399 ACPI I2C2 0x34/0x35 bus=%u reset=PGPI GPIO%u MMIO=%llx\n",
            resources.bus_hz, resources.reset_pin, (unsigned long long)physical);
    struct aw_gpio_io io = {registers, gpio_read, gpio_write, gpio_delay};
    if (!aw_gpio_reset(&io, resources.reset_pin, resources.pull))
        goto failed;
    pr_info("Audio: AW88399 shared amplifier reset high/low/high completed\n");
    return true;
failed:
    pr_warn(
        "Audio: AW88399 ACPI reset resource/ownership/lock check failed; amplifiers remain muted\n");
    return false;
}
