/* AW88399 DSP firmware, factory trim and protected playback sequencing.
 * Register/format reference: Awinic's upstream Linux aw88399-lib.c and
 * sound/aw88399.h. Bus callbacks also allow a register-model test. */
#ifndef AW88399_CORE_H
#define AW88399_CORE_H
#include <drivers/aw88399_firmware.h>

struct aw_chip {
    void *context;
    bool (*read)(void *, uint8_t, uint16_t *);
    bool (*write)(void *, uint8_t, const uint8_t *, size_t);
    void (*delay)(unsigned);
    uint16_t volume, calibration, crc_control, dither;
    size_t program_size, configuration_size;
    bool fuse_and, skip_boost_status, loaded, playing;
};

static inline bool aw_write(struct aw_chip *chip, unsigned reg, uint16_t value) {
    uint8_t bytes[2] = {value >> 8, value};
    return chip->write(chip->context, reg, bytes, 2);
}

static inline bool aw_update(struct aw_chip *chip, unsigned reg, uint16_t mask, uint16_t value) {
    uint16_t previous;
    return chip->read(chip->context, reg, &previous) &&
           aw_write(chip, reg, (previous & ~mask) | (value & mask));
}

static inline bool aw_dsp_read(struct aw_chip *chip, unsigned address, uint16_t *value) {
    return aw_write(chip, 0x40, address) && chip->read(chip->context, 0x41, value);
}

static inline void aw_chip_stop(struct aw_chip *chip) {
    aw_update(chip, 0x05, 0x3ff, 0x3ff);
    aw_update(chip, 0x04, 0x100, 0x100);
    chip->delay(4000);
    aw_update(chip, 0x08, 0x200, 0);
    aw_update(chip, 0x04, 7, 7);
    chip->playing = false;
}

static inline bool aw_dsp_upload(struct aw_chip *chip, unsigned address,
                                 struct aw_section section) {
    if (!aw_write(chip, 0x40, address))
        return false;

    uint8_t bytes[128];
    for (size_t offset = 0; offset < section.size;) {
        size_t count = section.size - offset;
        if (count > sizeof(bytes))
            count = sizeof(bytes);
        if (count & 1)
            return false;

        for (size_t i = 0; i < count; i += 2) {
            bytes[i] = section.bytes[offset + i + 1];
            bytes[i + 1] = section.bytes[offset + i];
        }

        if (!chip->write(chip->context, 0x41, bytes, count))
            return false;
        offset += count;
    }

    return true;
}

static inline bool aw_chip_load(struct aw_chip *chip, const struct aw_profile *profile) {
    uint16_t id;
    chip->loaded = false;
    if (!chip->read(chip->context, 0, &id) || id != 0x2183)
        return false;

    aw_chip_stop(chip);
    bool protected_dsp = false;
    for (size_t offset = 0; offset < profile->registers.size; offset += 4) {
        unsigned reg = aw_le16(profile->registers.bytes + offset);
        uint16_t value = aw_le16(profile->registers.bytes + offset + 2);
        if (reg == 0x4a) {
            chip->calibration = value;
            continue;
        }
        if (reg == 4) {
            protected_dsp = !(value & 4);
            value |= 0x107;
        }
        if (reg == 5) {
            chip->volume = value & 0x3ff;
            value |= 0x3ff;
        }
        if (reg == 8)
            value &= ~0x200;
        if (reg == 0x18) {
            chip->fuse_and = !!(value & 4);
            chip->dither = value & 0x8000;
            value &= ~0x8000;
        }
        if (reg == 0x4b)
            chip->crc_control = value;
        if (!aw_write(chip, reg, value))
            goto failed;
    }

    if (!protected_dsp || !chip->calibration || !aw_update(chip, 4, 1, 0) ||
        !aw_update(chip, 0x18, 8, 0))
        goto failed;
    chip->delay(1000);

    uint16_t value;
    if (!aw_dsp_read(chip, 0x1f40, &value) || value != 0x4638 || !aw_write(chip, 0x40, 0x9b00) ||
        !aw_write(chip, 0x41, 0x5555) || !aw_dsp_read(chip, 0x9b00, &value) || value != 0x5555)
        goto failed;

    if (!aw_dsp_upload(chip, 0x8980, profile->program) ||
        !aw_dsp_upload(chip, 0x9b00, profile->configuration) || !aw_update(chip, 0x18, 8, 8))
        goto failed;

    chip->program_size = profile->program.size;
    chip->configuration_size = profile->configuration.size;

    aw_chip_stop(chip);
    chip->loaded = true;
    return true;

failed:

    aw_chip_stop(chip);
    return false;
}

static inline bool aw_status(struct aw_chip *chip, uint16_t required, uint16_t forbidden) {
    for (unsigned retry = 0; retry < 10; retry++) {
        uint16_t status;
        if (!chip->read(chip->context, 1, &status))
            return false;
        if ((status & required) == required && !(status & forbidden))
            return true;
        chip->delay(2000);
    }
    return false;
}

static inline bool aw_check_crc(struct aw_chip *chip, size_t size, unsigned base, unsigned enable) {
    if (size < 4 || size % 4 || size / 4 - 1 + base > 0xfff)
        return false;
    if (!aw_update(chip, 0x4b, 0x3fff, (size / 4 - 1 + base) | enable))
        return false;
    chip->delay(2000);
    uint16_t status;
    return chip->read(chip->context, 0x20, &status) && ((status >> 12) & 7) == 4 &&
           aw_update(chip, 0x4b, enable, 0);
}

static inline bool aw_factory_calibration(struct aw_chip *chip) {
    uint16_t ih, il, vh, vl, sense;
    if (!chip->read(chip->context, 0x74, &ih) || !chip->read(chip->context, 0x78, &il) ||
        !chip->read(chip->context, 0x3b, &sense))
        return false;

    unsigned fused = chip->fuse_and ? (ih & il) : (ih | il);
    int current = (int)(fused & 0x3ff);
    if (current & 0x200)
        current -= 1024;

    bool external = sense & 0x20;
    if (!chip->read(chip->context, external ? 0x75 : 0x76, &vh) ||
        !chip->read(chip->context, external ? 0x79 : 0x7a, &vl))
        return false;

    fused = chip->fuse_and ? (vh & vl) : (vh | vl);
    int voltage = external ? (int)(fused & 0x3ff) : (int)((fused >> 9) & 0x3f);
    if (voltage & (external ? 0x200 : 0x20))
        voltage -= external ? 1024 : 64;

    int current_factor = 1000 + current, voltage_factor = 1000 + voltage * (external ? 1 : 2);
    if (current_factor <= 0 || voltage_factor <= 0)
        return false;

    uint64_t calibration = (4096ULL * (external ? 18875 : 12600) / 3125) * current_factor /
                           voltage_factor * chip->calibration;
    calibration >>= 12;
    if (!calibration || calibration > 65535)
        return false;

    return aw_write(chip, 0x4a, calibration);
}

static inline bool aw_chip_start(struct aw_chip *chip) {
    if (chip->playing)
        return true;
    if (!chip->loaded)
        return false;
    if (!aw_update(chip, 0x18, 0x8000, 0) || !aw_update(chip, 4, 1, 0))
        goto failed;
    chip->delay(2000);
    if (!aw_status(chip, 0x11, 0)) {
        uint16_t pll;
        if (!chip->read(chip->context, 0x51, &pll) || !(pll & 0x4000) ||
            !aw_update(chip, 0x51, 0x4000, 0))
            goto failed;
        bool locked = aw_status(chip, 0x11, 0);
        aw_update(chip, 0x51, 0x4000, 0x4000);
        chip->delay(2000);
        if (!locked || !aw_status(chip, 0x11, 0))
            goto failed;
    }
    if (!aw_update(chip, 4, 2, 0))
        goto failed;
    chip->delay(1000);
    uint16_t pwm;
    if (!chip->read(chip->context, 0x16, &pwm))
        goto failed;
    uint16_t required = 0x11 | ((pwm & 0x2000) ? 0 : 0x100) | (chip->skip_boost_status ? 0 : 0x200);
    if (!aw_status(chip, required, 0x482a) || !aw_update(chip, 0x17, 1, 1))
        goto failed;
    bool crc = aw_check_crc(chip, chip->program_size, 0x4c0, 0x2000) &&
               aw_check_crc(chip, chip->configuration_size, 0xd80, 0x1000);
    bool restored = aw_write(chip, 0x4b, chip->crc_control) && aw_update(chip, 0x17, 1, 0);
    if (!crc || !restored || !aw_factory_calibration(chip) || !aw_update(chip, 4, 4, 0))
        goto failed;
    chip->delay(1000);
    bool watchdog = false;
    for (unsigned retry = 0; retry < 5; retry++) {
        uint16_t status;
        if (!chip->read(chip->context, 0x42, &status))
            goto failed;
        if (status & 0xff) {
            watchdog = true;
            break;
        }
        chip->delay(2000);
    }
    if (!watchdog || !aw_update(chip, 8, 0x200, 0x200) ||
        !aw_update(chip, 0x18, 0x8000, chip->dither) || !aw_update(chip, 4, 0x100, 0))
        goto failed;
    for (unsigned attenuation = 1023; attenuation > chip->volume;) {
        attenuation = attenuation - chip->volume > 64 ? attenuation - 64 : chip->volume;
        if (!aw_update(chip, 5, 0x3ff, attenuation))
            goto failed;
        chip->delay(100);
    }
    uint16_t ignored;
    chip->read(chip->context, 2, &ignored);
    chip->read(chip->context, 2, &ignored);
    chip->playing = true;
    return true;
failed:
    aw_chip_stop(chip);
    return false;
}

#endif
