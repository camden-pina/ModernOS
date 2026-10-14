/* ACPI resources and Intel Meteor Point GPIO programming for the verified
 * ALC287/17aa3907 machine. GPIO numbers and MMIO windows come from _CRS.
 * Register definitions follow Intel's primary pinctrl driver; no host I/O. */
#ifndef MODERNOS_AW88399_POWER_H
#define MODERNOS_AW88399_POWER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct aw_power_resources {
    unsigned reset_pin, pull;
    uint32_t bus_hz;
};

static inline uint16_t aw_resource16(const uint8_t *p) {
    return p[0] | (uint16_t)p[1] << 8;
}

static inline uint32_t aw_resource32(const uint8_t *p) {
    return aw_resource16(p) | (uint32_t)aw_resource16(p + 2) << 16;
}

static inline uint64_t aw_resource64(const uint8_t *p) {
    return aw_resource32(p) | (uint64_t)aw_resource32(p + 4) << 32;
}

static inline bool aw_resource_name(const uint8_t *p, size_t n, const char *name) {
    size_t len = strlen(name) + 1;
    return n >= len && !memcmp(p, name, len);
}

static inline bool aw_power_parse(const uint8_t *bytes, size_t size,
                                  struct aw_power_resources *out) {
    unsigned addresses = 0, outputs = 0;
    struct aw_power_resources found = {0};
    if (!bytes || !out || size > 4096)
        return false;
    for (size_t at = 0; at < size;) {
        uint8_t tag = bytes[at];
        size_t n;
        if (tag & 0x80) {
            if (size - at < 3)
                return false;
            n = 3 + aw_resource16(bytes + at + 1);
        } else
            n = 1 + (tag & 7);
        if (n > size - at)
            return false;
        const uint8_t *p = bytes + at;
        if (tag == 0x79)
            return at + n == size && addresses == 3 && outputs == 1 ? (*out = found, true) : false;
        if (tag == 0x8e) {
            if (n < 19 || p[3] != 2 || p[5] != 1 || aw_resource16(p + 10) != 6 ||
                aw_resource16(p + 7) || !aw_resource_name(p + 18, n - 18, "\\_SB.PC02.I2C2"))
                return false;
            unsigned address = aw_resource16(p + 16);
            unsigned bit = address == 0x34 ? 1 : address == 0x35 ? 2 : 0;
            uint32_t hz = aw_resource32(p + 12);
            if (!bit || addresses & bit || !hz || hz > 400000 ||
                (found.bus_hz && found.bus_hz != hz))
                return false;
            addresses |= bit;
            found.bus_hz = hz;
        } else if (tag == 0x8c) {
            if (n < 23 || p[3] != 1)
                return false;
            unsigned pins = aw_resource16(p + 14), source = aw_resource16(p + 17);
            unsigned vendor = aw_resource16(p + 19), vendor_size = aw_resource16(p + 21);
            if (pins < 23 || source < pins || source > n || (source - pins) % 2 ||
                vendor < source || vendor > n || vendor_size > n - vendor)
                return false;
            if (p[4] == 1) {
                if (++outputs != 1 || !(aw_resource16(p + 5) & 1) ||
                    (aw_resource16(p + 7) & 3) != 1 || source - pins != 2 ||
                    !aw_resource_name(p + source, vendor - source, "\\_SB.PGPI") ||
                    (p[9] != 0 && p[9] != 1 && p[9] != 2 && p[9] != 3))
                    return false;
                found.reset_pin = aw_resource16(p + pins);
                found.pull = p[9];
            } else if (p[4] != 0)
                return false; /* Interrupt resource is not a reset output. */
        }
        at += n;
    }
    return false;
}

/* INTC1084's GPP_A is GPIO 128..142 in memory resource 1. It is the
 * community's first pad group, so its ownership and lock register index is 0. */
static inline bool aw_power_window(const uint8_t *bytes, size_t size, uint64_t *base) {
    unsigned memories = 0;
    uint64_t selected = 0;
    if (!bytes || !base || size > 4096)
        return false;
    for (size_t at = 0; at < size;) {
        uint8_t tag = bytes[at];
        size_t n;
        if (tag & 0x80) {
            if (size - at < 3)
                return false;
            n = 3 + aw_resource16(bytes + at + 1);
        } else
            n = 1 + (tag & 7);
        if (n > size - at)
            return false;
        const uint8_t *p = bytes + at;
        if (tag == 0x79)
            return at + n == size && memories == 5 && selected ? (*base = selected, true) : false;
        if (tag == 0x8a) {
            if (n < 46 || p[3] != 0 || (p[4] & 0x0d) != 0x0d || aw_resource64(p + 6) ||
                aw_resource64(p + 30))
                return false;
            uint64_t start = aw_resource64(p + 14), end = aw_resource64(p + 22),
                     length = aw_resource64(p + 38);
            if (!start || (start & 65535) || start > 0x000fffffffff0000ULL || length != 65536 ||
                end != start + length - 1)
                return false;
            if (memories++ == 1)
                selected = start;
        }
        at += n;
    }
    return false;
}

struct aw_gpio_io {
    void *context;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    void (*delay)(unsigned);
};

static inline bool aw_gpio_reset(const struct aw_gpio_io *io, unsigned gpio, unsigned pull) {
    if (!io || !io->read || !io->write || !io->delay || gpio < 128 || gpio > 142 || pull > 3)
        return false;
    unsigned pin = gpio - 128;
    uint32_t rev = io->read(io->context, 0);
    if (rev == UINT32_MAX || (rev >> 16) < 0x92 || (rev >> 16) >= 0x110)
        return false;
    if (io->read(io->context, 0xb0 + (pin / 8) * 4) & (15U << ((pin % 8) * 4)))
        return false;
    uint32_t bit = 1U << pin, locked = io->read(io->context, 0x110),
             tx_locked = io->read(io->context, 0x114);
    if (tx_locked & bit)
        return false;
    unsigned padbar = io->read(io->context, 0x0c);
    if (padbar < 0x400 || (padbar & 15) || padbar > 65536 - 15 * 16)
        return false;
    unsigned pad = padbar + pin * 16;
    uint32_t old = io->read(io->context, pad);
    uint32_t mode_mask = (15U << 10) | (3U << 8);
    uint32_t routing = (15U << 17);
    uint32_t configured = (old & ~(mode_mask | routing)) | (1U << 9);
    uint32_t term_mask = 15U << 10, term = io->read(io->context, pad + 4);
    uint32_t wanted_term = pull == 1 ? (12U << 10) : pull == 2 ? (4U << 10) : 0;
    bool change_term = pull != 0;
    if (locked & bit) {
        if ((old & mode_mask) != (1U << 9) || (old & routing) ||
            (change_term && (term & term_mask) != wanted_term))
            return false;
        configured = old;
    } else {
        if (change_term)
            io->write(io->context, pad + 4, (term & ~term_mask) | wanted_term);
        /* Set the inactive high level together with GPIO output enable. */
        io->write(io->context, pad, configured | 1U);
        if ((io->read(io->context, pad) & (mode_mask | routing)) != (1U << 9) ||
            (change_term && (io->read(io->context, pad + 4) & term_mask) != wanted_term))
            return false;
    }
    /* Both amps share this reset: reset once before loading either DSP.
     * The primary AW88399 driver uses high/low/high with 1-ms holds. */
    for (unsigned phase = 0; phase < 3; phase++) {
        uint32_t level = phase == 1 ? 0 : 1;
        uint32_t value = io->read(io->context, pad);
        io->write(io->context, pad, (value & ~1U) | level);
        if ((io->read(io->context, pad) & 1) != level)
            return false;
        io->delay(1000);
    }
    return true;
}

bool aw88399_power_init(void);
#endif
