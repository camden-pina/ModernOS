#include <drivers/block.h>
#include <uapi/abi.h>
#include <kernel.h>
#include <string.h>
#include <mm/pmm.h>

struct block_device block_devices[BLOCK_MAX];
unsigned block_count;

uint16_t get_le16(const void *p) {
    const uint8_t *b = p;
    return b[0] | ((uint16_t)b[1] << 8);
}

uint32_t get_le32(const void *p) {
    const uint8_t *b = p;
    return get_le16(b) | ((uint32_t)get_le16(b + 2) << 16);
}

uint64_t get_le64(const void *p) {
    const uint8_t *b = p;
    return get_le32(b) | ((uint64_t)get_le32(b + 4) << 32);
}

void put_le16(void *p, uint16_t v) {
    uint8_t *b = p;
    b[0] = v;
    b[1] = v >> 8;
}

void put_le32(void *p, uint32_t v) {
    uint8_t *b = p;
    put_le16(b, v);
    put_le16(b + 2, v >> 16);
}

uint32_t disk_crc32(uint32_t crc, const void *data, size_t size) {
    const uint8_t *p = data;
    while (size--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((0 - (crc & 1)) & 0xedb88320u);
    }
    return crc;
}

struct block_device *block_register(const struct block_device *device) {
    if (block_count == BLOCK_MAX || device->sector_size < 512 || device->sector_size > 4096 ||
        (device->sector_size & (device->sector_size - 1)) || !device->sectors ||
        device->sectors > UINT64_MAX / device->sector_size)
        return NULL;
    block_devices[block_count] = *device;
    return &block_devices[block_count++];
}

static int transfer(struct block_device *d, uint64_t lba, unsigned count, void *data, bool write) {
    if (!d || !data || !count || lba >= d->sectors || count > d->sectors - lba)
        return -E_INVAL;
    if (write && d->readonly)
        return -E_ROFS;
    if (d->parent)
        return transfer(d->parent, d->start + lba, count, data, write);
    if (!d->transfer)
        return -E_IO;
    return d->transfer(d, lba, count, data, write);
}

int block_read(struct block_device *d, uint64_t lba, unsigned count, void *data) {
    return transfer(d, lba, count, data, false);
}

int block_write(struct block_device *d, uint64_t lba, unsigned count, const void *data) {
    return transfer(d, lba, count, (void *)data, true);
}

int block_flush(struct block_device *d) {
    while (d && d->parent)
        d = d->parent;
    return d && d->flush ? d->flush(d) : 0;
}

int block_read_bytes(struct block_device *d, uint64_t offset, void *data, size_t size) {
    if (!d || d->sectors > UINT64_MAX / d->sector_size || offset > d->sectors * d->sector_size ||
        size > d->sectors * d->sector_size - offset)
        return -E_IO;
    uint8_t sector[4096], *out = data;
    while (size) {
        uint64_t lba = offset / d->sector_size;
        unsigned skip = offset % d->sector_size;
        if (!skip && size >= d->sector_size) {
            unsigned count = min(size / d->sector_size, 128 * 1024 / d->sector_size);
            int e = block_read(d, lba, count, out);
            if (e)
                return e;
            size_t n = (size_t)count * d->sector_size;
            out += n;
            offset += n;
            size -= n;
            continue;
        }
        size_t n = min(size, d->sector_size - skip);
        int e = block_read(d, lba, 1, sector);
        if (e)
            return e;
        memcpy(out, sector + skip, n);
        out += n;
        offset += n;
        size -= n;
    }
    return 0;
}

static void partition(struct block_device *d, uint64_t first, uint64_t count, unsigned index) {
    if (!first || first >= d->sectors || !count || count > d->sectors - first)
        return;
    /* Reject overlapping partitions, including malformed GPT/MBR records. */
    for (unsigned i = 0; i < block_count; i++)
        if (block_devices[i].parent == d &&
            first < block_devices[i].start + block_devices[i].sectors &&
            block_devices[i].start < first + count)
            return;
    struct block_device p = {.sector_size = d->sector_size,
                             .sectors = count,
                             .start = first,
                             .parent = d,
                             .readonly = d->readonly};
    snprintf(p.name, sizeof(p.name), "%sp%u", d->name, index);
    block_register(&p);
}

static bool gpt(struct block_device *d) {
    uint8_t header[4096];
    if (block_read(d, 1, 1, header) || memcmp(header, "EFI PART", 8))
        return false;
    unsigned hsize = get_le32(header + 12), num = get_le32(header + 80),
             esize = get_le32(header + 84);
    uint64_t table = get_le64(header + 72), first = get_le64(header + 40),
             last = get_le64(header + 48);
    if (hsize < 92 || hsize > d->sector_size || num > 4096 || esize < 128 || esize > 4096 ||
        (esize & 7) || first > last || last >= d->sectors || table >= d->sectors ||
        get_le64(header + 24) != 1 || get_le32(header + 8) != 0x10000)
        return true;
    uint32_t saved = get_le32(header + 16);
    put_le32(header + 16, 0);
    if (~disk_crc32(~0u, header, hsize) != saved) {
        pr_warn("%s: GPT header checksum failed\n", d->name);
        return true;
    }
    uint64_t bytes = (uint64_t)num * esize;
    if (bytes > (d->sectors - table) * d->sector_size)
        return true;
    uint32_t crc = ~0u;
    uint8_t entry[4096];
    for (uint64_t off = 0; off < bytes;) {
        size_t n = min((uint64_t)sizeof(entry), bytes - off);
        if (block_read_bytes(d, table * d->sector_size + off, entry, n))
            return true;
        crc = disk_crc32(crc, entry, n);
        off += n;
    }
    if (~crc != get_le32(header + 88)) {
        pr_warn("%s: GPT entry checksum failed\n", d->name);
        return true;
    }
    for (unsigned i = 0; i < num; i++) {
        if (block_read_bytes(d, table * d->sector_size + (uint64_t)i * esize, entry, esize))
            break;
        bool used = false;
        for (unsigned j = 0; j < 16; j++)
            if (entry[j])
                used = true;
        if (!used)
            continue;
        uint64_t begin = get_le64(entry + 32), end = get_le64(entry + 40);
        if (begin >= first && end <= last && end >= begin)
            partition(d, begin, end - begin + 1, i + 1);
    }
    return true;
}

static bool extended(unsigned t) {
    return t == 5 || t == 15 || t == 0x85;
}

static void mbr(struct block_device *d) {
    uint8_t b[4096];
    if (block_read(d, 0, 1, b) || get_le16(b + 510) != 0xaa55)
        return;
    for (unsigned i = 0; i < 4; i++) {
        uint8_t *p = b + 446 + i * 16;
        unsigned type = p[4];
        uint64_t start = get_le32(p + 8), count = get_le32(p + 12);
        if (!type || type == 0xee)
            continue;
        if (!extended(type)) {
            partition(d, start, count, i + 1);
            continue;
        }
        uint64_t ebr = start, limit = start + count;
        unsigned number = 5;
        uint64_t seen[128];
        unsigned seen_count = 0;
        while (ebr >= start && ebr < limit && ebr < d->sectors && seen_count < 128) {
            bool duplicate = false;
            for (unsigned j = 0; j < seen_count; j++)
                if (seen[j] == ebr)
                    duplicate = true;
            if (duplicate)
                break;
            seen[seen_count++] = ebr;
            uint8_t e[4096];
            if (block_read(d, ebr, 1, e) || get_le16(e + 510) != 0xaa55)
                break;
            uint64_t begin = ebr + get_le32(e + 454), length = get_le32(e + 458);
            if (e[450] && begin >= start && begin < limit && length <= limit - begin)
                partition(d, begin, length, number++);
            if (!extended(e[466]))
                break;
            ebr = start + get_le32(e + 470);
        }
    }
}

void block_partitions(void) {
    unsigned disks = block_count;
    for (unsigned i = 0; i < disks; i++)
        if (!block_devices[i].parent && !gpt(&block_devices[i]))
            mbr(&block_devices[i]);
}

size_t block_info(char *out, size_t cap) {
    size_t used = 0;
    for (unsigned i = 0; i < block_count && used < cap; i++) {
        struct block_device *d = &block_devices[i];
        int n = snprintf(out + used, cap - used, "%s sectors=%llu sector_size=%u %s%s\n", d->name,
                         (unsigned long long)d->sectors, d->sector_size,
                         d->parent ? "partition " : "drive ", d->readonly ? "ro" : "rw");
        used += min((size_t)max(n, 0), cap - used);
    }
    return min(used, cap ? cap - 1 : 0);
}
