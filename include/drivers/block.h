#ifndef BLOCK_H
#define BLOCK_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/pci/pci.h>

#define BLOCK_MAX 128

struct block_device {
    char name[32];
    uint32_t sector_size;
    uint64_t sectors, start;
    struct block_device *parent;
    void *private;
    int (*transfer)(struct block_device *, uint64_t, unsigned, void *, bool);
    int (*flush)(struct block_device *);
    bool readonly;
};

extern struct block_device block_devices[BLOCK_MAX];
extern unsigned block_count;
struct block_device *block_register(const struct block_device *device);
int block_read(struct block_device *d, uint64_t lba, unsigned count, void *data);
int block_write(struct block_device *d, uint64_t lba, unsigned count, const void *data);
int block_flush(struct block_device *d);
int block_read_bytes(struct block_device *d, uint64_t offset, void *data, size_t size);
void block_partitions(void);
size_t block_info(char *out, size_t cap);
void nvme_probe(pci_device_header_t *header);
void ahci_probe(pci_device_header_t *header);
uint16_t get_le16(const void *p);
uint32_t get_le32(const void *p);
uint64_t get_le64(const void *p);
void put_le16(void *p, uint16_t v);
void put_le32(void *p, uint32_t v);
uint32_t disk_crc32(uint32_t crc, const void *data, size_t size);
#endif
