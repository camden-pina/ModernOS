/* Bounded, allocation-free reader for Awinic ACF v1 and its nested bins.
 * Format reference: Linux sound/soc/codecs/aw88395/{aw88395_data_type.h,
 * aw88395_lib.c}. Firmware bytes are supplied locally, never manufactured. */
#ifndef AW88399_FIRMWARE_H
#define AW88399_FIRMWARE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct aw_section {
    const uint8_t *bytes;
    size_t size;
};

struct aw_profile {
    struct aw_section registers, configuration, program;
};

static inline uint16_t aw_le16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static inline uint32_t aw_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint8_t aw_crc8(const uint8_t *p, size_t size) {
    uint8_t crc = 0;
    for (size_t i = 0; i < size; i++) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0x8c : 0);
    }
    return crc;
}

static inline bool aw_bin_profile(const uint8_t *bytes, size_t size, struct aw_profile *profile,
                                  unsigned depth) {
    if (size < 60 || depth > 2 || aw_le32(bytes + 4) != 0x01000000 || aw_le32(bytes + 12) != 1 ||
        aw_le32(bytes + 16) != size - 60)
        return false;

    uint32_t sum = 0;
    for (size_t i = 4; i < size; i++)
        sum += bytes[i];
    if (sum != aw_le32(bytes))
        return false;

    uint32_t kind = aw_le32(bytes + 8);
    const uint8_t *data = bytes + 60;
    size_t length = size - 60;

    if (kind == 0x2000) {
        if (length < 4)
            return false;
        unsigned count = aw_le32(data);
        if (!count || count > 8 || length < 4 + count * 8)
            return false;

        size_t previous = 4 + count * 8;
        for (unsigned i = 0; i < count; i++) {
            size_t offset = aw_le32(data + 4 + i * 8), child = aw_le32(data + 8 + i * 8);
            if (offset != previous || offset > length || child > length - offset ||
                !aw_bin_profile(data + offset, child, profile, depth + 1))
                return false;
            previous = offset + child;
        }
        return previous == length;
    }

    if (kind == 0) {
        if (profile->registers.bytes || length < 4 || aw_le32(bytes + 32) != 2 ||
            aw_le32(bytes + 36) != 2 || (length - 4) % 4 || aw_le32(data) != (length - 4) / 4)
            return false;

        profile->registers = (struct aw_section){data + 4, length - 4};
        for (size_t i = 0; i < length - 4; i += 4)
            if (aw_le16(data + 4 + i) > 0x70)
                return false;
    } else if (kind == 0x10) {
        if (profile->configuration.bytes || length < 12 || aw_le32(data) != 0x9b00 ||
            aw_le32(data + 8) != 4 || (length - 12) % 4 || aw_le32(data + 4) != (length - 12) / 4)
            return false;

        profile->configuration = (struct aw_section){data + 12, length - 12};
    } else if (kind == 0x21) {
        if (profile->program.bytes || length < 12 || aw_le32(data + 4) != 0x8980 ||
            aw_le32(data + 8) != length - 12 || (length - 12) % 4)
            return false;

        profile->program = (struct aw_section){data + 12, length - 12};
    } else if (kind != 0x12)
        return false; /* Debug metadata has no executable/register effect. */

    return true;
}

static inline bool aw_acf_profile(const uint8_t *bytes, size_t size, unsigned channel,
                                  unsigned address, struct aw_profile *out) {
    if (!bytes || !out || channel > 1 || address < 0x34 || address > 0x35 || size < 80 ||
        size > 512 * 1024 || aw_le32(bytes) != 0x0a15f908 || aw_le32(bytes + 64) != 1)
        return false;

    unsigned tableSize = aw_le32(bytes + 52), count = aw_le32(bytes + 56),
             offset = aw_le32(bytes + 60);
    if (!count || count > 32 || offset < 80 || offset > size || tableSize != count * 64 ||
        tableSize > size - offset)
        return false;

    size_t end = offset + tableSize;
    const uint8_t *selected = NULL;
    unsigned priority = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *entry = bytes + offset + i * 64;
        size_t length = aw_le32(entry + 32), dataOffset = aw_le32(entry + 36);
        if (dataOffset != end || length > size - end ||
            aw_le32(entry + 40) != aw_crc8(bytes + end, length))
            return false;
        end += length;

        unsigned type = aw_le32(entry), index = aw_le16(entry + 20), device = aw_le16(entry + 24),
                 profile = aw_le16(entry + 26);
        unsigned rank = type == 0 && device == address ? 2 : type == 2 && index == channel ? 1 : 0;
        if (rank && profile == 0 && aw_le32(entry + 28) == 7 && rank >= priority) {
            if (rank == priority)
                return false;
            priority = rank;
            selected = entry;
        }
    }

    if (end != size || !selected)
        return false;

    struct aw_profile result = {0};
    if (!aw_bin_profile(bytes + aw_le32(selected + 36), aw_le32(selected + 32), &result, 0) ||
        !result.registers.size || !result.configuration.size || !result.program.size ||
        result.program.size / 4 + 0x4c0 > 0xfff || result.configuration.size / 4 + 0xd80 > 0xfff)
        return false;

    *out = result;
    return true;
}

#endif
