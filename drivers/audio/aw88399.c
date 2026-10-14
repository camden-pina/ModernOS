/* Lenovo Legion Pro 7i 16IAX10H (ALC287 subsystem 17aa3907):
 * AW88399 at I2C2 0x34=right and 0x35=left. Only this verified device identity
 * selects the quirk. DSP program/EQ/protection come from the user's firmware. */
#include <drivers/aw88399_core.h>
#include <drivers/aw88399_power.h>
#include <drivers/designware_i2c.h>
#include <fs/vfs.h>
#include <mm/pmm.h>
#include <mm/user.h>
#include <tsc.h>
#include <sync.h>
#include <printk.h>

static struct aw_chip speakers[2];
static mutex_t speakers_mutex;
static bool matched, ready;
static uint64_t retry_after;

static void speaker_delay(unsigned microseconds) {
    uint64_t deadline = tsc_uptime_us() + microseconds;
    while (tsc_uptime_us() < deadline)
        __asm__ volatile("pause");
}

static bool speaker_read(void *context, uint8_t reg, uint16_t *value) {
    uint8_t bytes[2];
    if (!designware_i2c_transfer((uintptr_t)context, &reg, 1, bytes, 2))
        return false;
    *value = (uint16_t)bytes[0] << 8 | bytes[1];
    return true;
}

static bool speaker_write(void *context, uint8_t reg, const uint8_t *bytes, size_t size) {
    if (size > 128)
        return false;
    uint8_t packet[129];
    packet[0] = reg;
    for (size_t i = 0; i < size; i++)
        packet[i + 1] = bytes[i];
    return designware_i2c_transfer((uintptr_t)context, packet, size + 1, NULL, 0);
}

void aw88399_match(uint32_t vendor, uint32_t subsystem) {
    matched = vendor == 0x10ec0287 && subsystem == 0x17aa3907;
}

void aw88399_init(void) {
    if (!matched)
        return;
    if (!aw88399_power_init())
        return;
    struct file *file = NULL;
    if (vfs_open("/lib/firmware/aw88399_acf.bin", O_RDONLY, 0, &file) < 0) {
        pr_warn("Audio: built-in AW88399 speakers need /lib/firmware/aw88399_acf.bin\n");
        return;
    }
    size_t size = file->node.size, pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t physical = 0;
    if (size < 80 || size > 512 * 1024 || !(physical = pmm_alloc_pages(pages)))
        goto done;
    uint8_t *bytes = phys_ptr(physical);
    if (vfs_read(file, bytes, size) != (long)size)
        goto done;
    for (unsigned i = 0; i < 2; i++) {
        unsigned address = 0x34 + i, channel = 1 - i;
        struct aw_profile profile;
        speakers[i] = (struct aw_chip){.context = (void *)(uintptr_t)address,
                                       .read = speaker_read,
                                       .write = speaker_write,
                                       .delay = speaker_delay,
                                       .skip_boost_status = true};
        if (!aw_acf_profile(bytes, size, channel, address, &profile) ||
            !aw_chip_load(&speakers[i], &profile))
            goto failed;
    }
    ready = true;
    pr_info("Audio: AW88399 left/right speaker DSP firmware and protection loaded\n");
    goto done;
failed:
    for (unsigned i = 0; i < 2; i++)
        if (speakers[i].write)
            aw_chip_stop(&speakers[i]);
    pr_warn("Audio: AW88399 firmware/I2C initialization failed; amplifiers remain muted\n");
done:
    if (physical)
        pmm_free_pages(physical, pages);
    vfs_close(file);
}

bool aw88399_start(void) {
    if (!matched)
        return true;
    mutex_lock(&speakers_mutex);
    bool success = ready;
    if (!ready || tsc_uptime_us() < retry_after) {
        mutex_unlock(&speakers_mutex);
        return false;
    }
    if (success)
        for (unsigned i = 0; i < 2; i++)
            if (!aw_chip_start(&speakers[i])) {
                success = false;
                break;
            }
    if (!success) {
        for (unsigned i = 0; i < 2; i++)
            if (speakers[i].write)
                aw_chip_stop(&speakers[i]);
        pr_warn("Audio: AW88399 playback clock/DSP protection check failed\n");
        retry_after = tsc_uptime_us() + 1000000;
    }
    mutex_unlock(&speakers_mutex);
    return success;
}

void aw88399_stop(void) {
    if (!matched || !ready)
        return;
    mutex_lock(&speakers_mutex);
    for (unsigned i = 0; i < 2; i++)
        aw_chip_stop(&speakers[i]);
    mutex_unlock(&speakers_mutex);
}
