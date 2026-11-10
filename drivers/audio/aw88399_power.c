#include <drivers/aw88399_power.h>
#include <acpica.h>
#include <mm/vmem_io.h>
#include <printk.h>
#include <tsc.h>

/* ACPICA returns the object and its data in one caller-owned allocation. */
static ACPI_OBJECT *eval_object(ACPI_HANDLE device, char *name, ACPI_OBJECT_TYPE type) {
    ACPI_BUFFER result = {ACPI_ALLOCATE_BUFFER, NULL};
    ACPI_STATUS status = AcpiEvaluateObject(device, name, NULL, &result);
    ACPI_OBJECT *value = result.Pointer;
    if (ACPI_FAILURE(status) || !value || result.Length < sizeof(*value) ||
        value->Type != type) {
        if (value)
            AcpiOsFree(value);
        return NULL;
    }
    return value;
}

static bool eval_string(ACPI_HANDLE device, char *name, const char *expected) {
    ACPI_OBJECT *value = eval_object(device, name, ACPI_TYPE_STRING);
    if (!value)
        return false;
    bool matches = value->String.Pointer && (size_t)value->String.Length == (size_t)strlen(expected) &&
                   !memcmp(value->String.Pointer, expected, value->String.Length);
    AcpiOsFree(value);
    return matches;
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
    ACPI_HANDLE speaker, gpio;
    ACPI_OBJECT *speaker_crs = NULL, *gpio_crs = NULL;
    struct aw_power_resources resources;
    uint64_t physical;
    bool powered = false;
    if (ACPI_FAILURE(AcpiGetHandle(NULL, "\\_SB.PC02.I2C2.SPKR", &speaker)) ||
        ACPI_FAILURE(AcpiGetHandle(NULL, "\\_SB.PGPI", &gpio)) ||
        !eval_string(speaker, "_HID", "AWDZ8399") ||
        !eval_string(speaker, "_SUB", "17AA3907") ||
        !eval_string(gpio, "_HID", "INTC1084") ||
        !(speaker_crs = eval_object(speaker, "_CRS", ACPI_TYPE_BUFFER)) ||
        !aw_power_parse(speaker_crs->Buffer.Pointer, speaker_crs->Buffer.Length, &resources) ||
        !(gpio_crs = eval_object(gpio, "_CRS", ACPI_TYPE_BUFFER)) ||
        !aw_power_window(gpio_crs->Buffer.Pointer, gpio_crs->Buffer.Length, &physical))
        goto done;
    /* _CRS contains the actual reset, not the separate interrupt GPIO. */
    if (resources.reset_pin != 140 || resources.pull != 1)
        goto done;
    void *registers = vmem_map_mmio(physical, 65536);
    if (!registers)
        goto done;
    pr_info("Audio: AW88399 ACPI I2C2 0x34/0x35 bus=%u reset=PGPI GPIO%u MMIO=%llx\n",
            resources.bus_hz, resources.reset_pin, (unsigned long long)physical);
    struct aw_gpio_io io = {registers, gpio_read, gpio_write, gpio_delay};
    if (!aw_gpio_reset(&io, resources.reset_pin, resources.pull))
        goto done;
    pr_info("Audio: AW88399 shared amplifier reset high/low/high completed\n");
    powered = true;
done:
    if (speaker_crs)
        AcpiOsFree(speaker_crs);
    if (gpio_crs)
        AcpiOsFree(gpio_crs);
    if (!powered)
        pr_warn(
            "Audio: AW88399 ACPI reset resource/ownership/lock check failed; amplifiers remain muted\n");
    return powered;
}
