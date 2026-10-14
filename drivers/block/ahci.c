#include <drivers/block.h>
#include <mm/vmem_io.h>
#include <mm/user.h>
#include <mm/pmm.h>
#include <sched.h>
#include <sync.h>
#include <string.h>
#include <tsc.h>

#define AHCI_PORT_MAX 32
#define AHCI_DMA_PAGES 32

struct command_header {
    uint16_t flags, prdt_count;
    uint32_t transferred;
    uint64_t table;
    uint32_t reserved[4];
} __attribute__((packed));
struct port {
    volatile uint8_t *regs;
    struct command_header *headers;
    uint64_t table_phys, dma_phys;
    uint8_t *table, *dma;
    mutex_t lock;
    bool failed;
};

static struct port ports[AHCI_PORT_MAX];
static unsigned port_count, disk_count;

static uint32_t read_port(struct port *p, unsigned n) {
    return *(volatile uint32_t *)(p->regs + n);
}

static void write_port(struct port *p, unsigned n, uint32_t v) {
    *(volatile uint32_t *)(p->regs + n) = v;
}

static void pause_io(void) {
    if (sched_ready())
        sched_sleep(1000);
    else
        __asm__ volatile("pause" ::: "memory");
}

static bool wait_clear(struct port *p, unsigned reg, unsigned bits) {
    uint64_t start = tsc_uptime_us();
    while (read_port(p, reg) & bits) {
        if (tsc_uptime_us() - start > 5000000)
            return false;
        if (tsc_uptime_us() - start > 1000)
            pause_io();
        else
            __asm__ volatile("pause" ::: "memory");
    }
    return true;
}

static int issue(struct port *p, unsigned opcode, uint64_t lba, unsigned sectors, size_t bytes,
                 bool write) {
    if (p->failed)
        return -E_IO;
    if (!wait_clear(p, 0x20, 0x88)) {
        p->failed = true;
        return -E_TIMEDOUT;
    }

    memset(p->table, 0, 4096);
    memset(&p->headers[0], 0, sizeof(*p->headers));
    uint8_t *fis = p->table;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = opcode;
    fis[7] = 0x40;
    fis[4] = lba;
    fis[5] = lba >> 8;
    fis[6] = lba >> 16;
    fis[8] = lba >> 24;
    fis[9] = lba >> 32;
    fis[10] = lba >> 40;
    fis[12] = sectors;
    fis[13] = sectors >> 8;

    p->headers[0].flags = 5 | (write ? 0x40 : 0);
    p->headers[0].prdt_count = bytes ? 1 : 0;
    p->headers[0].table = p->table_phys;
    if (bytes) {
        uint32_t *prdt = (void *)(p->table + 128);
        prdt[0] = p->dma_phys;
        prdt[1] = p->dma_phys >> 32;
        prdt[3] = bytes - 1;
    }

    write_port(p, 0x10, 0xffffffff);
    write_port(p, 0x30, 0xffffffff);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    write_port(p, 0x38, 1);
    if (!wait_clear(p, 0x38, 1)) {
        /* Quarantine DMA memory if hardware might still own this command. */
        p->failed = true;
        return -E_TIMEDOUT;
    }

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if ((read_port(p, 0x10) & (1u << 30)) || (read_port(p, 0x20) & 1))
        return -E_IO;
    return 0;
}

static int ahci_transfer(struct block_device *d, uint64_t lba, unsigned count, void *data,
                         bool write) {
    struct port *p = d->private;
    int e = 0;

    mutex_lock(&p->lock);
    while (count) {
        unsigned n = min(count, AHCI_DMA_PAGES * 4096 / d->sector_size);
        size_t bytes = (size_t)n * d->sector_size;
        if (write)
            memcpy(p->dma, data, bytes);

        e = issue(p, write ? 0x35 : 0x25, lba, n, bytes, write);
        if (e)
            break;

        if (!write)
            memcpy(data, p->dma, bytes);
        lba += n;
        count -= n;
        data = (uint8_t *)data + bytes;
    }

    mutex_unlock(&p->lock);
    return e;
}

static int ahci_flush(struct block_device *d) {
    struct port *p = d->private;
    mutex_lock(&p->lock);
    int e = issue(p, 0xea, 0, 0, 0, false);
    mutex_unlock(&p->lock);
    return e;
}

void ahci_probe(pci_device_header_t *h) {
    uint64_t bar = h->bar[5] & ~15u;
    if (!bar || (h->bar[5] & 7))
        return;
    volatile uint16_t *command = (void *)((uintptr_t)h + 4);
    *command |= 6 | 0x400;
    volatile uint8_t *regs = vmem_map_mmio(bar, 8192);
    if (!regs)
        return;
    uint32_t cap = *(volatile uint32_t *)(regs);
    /* Request OS ownership if the optional BIOS handoff capability exists. */
    if (*(volatile uint32_t *)(regs + 0x24) & 1) {
        volatile uint32_t *handoff = (void *)(regs + 0x28);
        *handoff |= 2;
        uint64_t start = tsc_uptime_us();
        while (*handoff & 0x11) {
            if (tsc_uptime_us() - start > 2000000)
                return;
            pause_io();
        }
    }
    volatile uint32_t *ghc = (void *)(regs + 4);
    *ghc = (*ghc | (1u << 31)) & ~2u; /* AHCI enabled; polling */
    uint32_t implemented = *(volatile uint32_t *)(regs + 0x0c);
    for (unsigned i = 0; i < 32 && port_count < AHCI_PORT_MAX; i++) {
        if (!(implemented & (1u << i)))
            continue;
        volatile uint8_t *preg = regs + 0x100 + i * 128;
        if ((*(volatile uint32_t *)(preg + 0x28) & 0xf0f) != 0x103 ||
            *(volatile uint32_t *)(preg + 0x24) != 0x101)
            continue; /* SATA only */
        struct port *p = &ports[port_count++];
        p->regs = preg;
        write_port(p, 0x18, read_port(p, 0x18) & ~1u);
        if (!wait_clear(p, 0x18, 1u << 15))
            continue;
        write_port(p, 0x18, read_port(p, 0x18) & ~(1u << 4));
        if (!wait_clear(p, 0x18, 1u << 14))
            continue;
        uint64_t cl = pmm_alloc_pages(1), rf = pmm_alloc_pages(1);
        p->table_phys = pmm_alloc_pages(1);
        p->dma_phys = pmm_alloc_pages(AHCI_DMA_PAGES);
        if (!cl || !rf || !p->table_phys || !p->dma_phys)
            continue;
        if (!(cap & (1u << 31)) &&
            max(max(cl, rf), max(p->table_phys, p->dma_phys + AHCI_DMA_PAGES * 4096)) >=
                0x100000000ULL)
            continue;
        p->headers = phys_ptr(cl);
        p->table = phys_ptr(p->table_phys);
        p->dma = phys_ptr(p->dma_phys);
        memset(p->headers, 0, 4096);
        memset(phys_ptr(rf), 0, 4096);
        write_port(p, 0, cl);
        write_port(p, 4, cl >> 32);
        write_port(p, 8, rf);
        write_port(p, 12, rf >> 32);
        write_port(p, 0x14, 0);
        write_port(p, 0x10, 0xffffffff);
        write_port(p, 0x18, (read_port(p, 0x18) & ~(15u << 28)) | 0x11);
        if (issue(p, 0xec, 0, 0, 512, false))
            continue;
        uint8_t *b = p->dma;
        if (!(get_le16(b + 166) & (1u << 10)))
            continue; /* LBA48 required */
        uint64_t sectors = get_le64(b + 200);
        unsigned sector_size = 512, info = get_le16(b + 212);
        if ((info & 0xc000) == 0x4000 && (info & 0x1000))
            sector_size = 2 * get_le32(b + 234);
        struct block_device d = {.sector_size = sector_size,
                                 .sectors = sectors,
                                 .private = p,
                                 .transfer = ahci_transfer,
                                 .flush = ahci_flush};
        snprintf(d.name, sizeof(d.name), "sata%u", disk_count++);
        if (block_register(&d))
            pr_info("%s: %llu MiB, %u byte sectors\n", d.name,
                    (unsigned long long)(sectors * sector_size >> 20), sector_size);
    }
}
