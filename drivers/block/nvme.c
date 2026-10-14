#include <drivers/block.h>
#include <mm/vmem_io.h>
#include <mm/user.h>
#include <mm/pmm.h>
#include <sync.h>
#include <sched.h>
#include <tsc.h>
#include <string.h>

#define NVME_MAX 8
#define QUEUE_DEPTH 32
#define DMA_PAGES 32

struct command {
    uint8_t op, flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved, mptr, prp1, prp2;
    uint32_t dword[6];
} __attribute__((packed));
struct completion {
    uint32_t result, reserved;
    uint16_t head, sqid, cid, status;
} __attribute__((packed));
struct queue {
    struct command *sq;
    volatile struct completion *cq;
    uint64_t sq_phys, cq_phys;
    uint16_t tail, head, cid;
    uint8_t phase;
    unsigned id;
};

struct controller {
    volatile uint8_t *regs;
    struct queue admin, io;
    unsigned stride, index, max_transfer;
    uint64_t dma_phys, prps_phys;
    void *dma;
    mutex_t lock;
    bool failed;
};

struct ns {
    struct controller *controller;
    unsigned id;
};

static struct controller controllers[NVME_MAX];
static struct ns namespaces[64];
static unsigned controller_count, namespace_count;

static uint32_t reg32(struct controller *c, unsigned off) {
    return *(volatile uint32_t *)(c->regs + off);
}

static void write32(struct controller *c, unsigned off, uint32_t v) {
    *(volatile uint32_t *)(c->regs + off) = v;
}

static void write64(struct controller *c, unsigned off, uint64_t v) {
    *(volatile uint64_t *)(c->regs + off) = v;
}

static bool wait_ready(struct controller *c, bool ready, uint64_t timeout) {
    uint64_t start = tsc_uptime_us();
    while ((reg32(c, 0x1c) & 1) != (unsigned)ready) {
        if ((reg32(c, 0x1c) & 2) || tsc_uptime_us() - start > timeout)
            return false;
        __asm__ volatile("pause" ::: "memory");
    }
    return true;
}

static bool allocate_queue(struct queue *q, unsigned id) {
    q->sq_phys = pmm_alloc_pages(1);
    q->cq_phys = pmm_alloc_pages(1);
    if (!q->sq_phys || !q->cq_phys)
        return false;
    q->sq = phys_ptr(q->sq_phys);
    q->cq = phys_ptr(q->cq_phys);
    memset(q->sq, 0, 4096);
    memset((void *)q->cq, 0, 4096);
    q->phase = 1;
    q->id = id;
    return true;
}

static int submit(struct controller *c, struct queue *q, struct command *cmd, uint32_t *result) {
    if (c->failed)
        return -E_IO;
    cmd->cid = ++q->cid;
    q->sq[q->tail] = *cmd;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    q->tail = (q->tail + 1) % QUEUE_DEPTH;
    write32(c, 0x1000 + 2 * q->id * c->stride, q->tail);
    uint64_t start = tsc_uptime_us();
    while ((q->cq[q->head].status & 1) != q->phase) {
        if ((reg32(c, 0x1c) & 2) || tsc_uptime_us() - start > 5000000) {
            /* Quarantine the controller and DMA pages. A timed-out command
               may still DMA, so neither memory nor queue entries are reused. */
            c->failed = true;
            pr_err("nvme%u: command timeout/controller failure\n", c->index);
            return -E_TIMEDOUT;
        }
        if (sched_ready() && tsc_uptime_us() - start > 1000)
            sched_sleep(1000);
        else
            __asm__ volatile("pause" ::: "memory");
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    struct completion done = q->cq[q->head];
    if (++q->head == QUEUE_DEPTH) {
        q->head = 0;
        q->phase ^= 1;
    }
    write32(c, 0x1000 + (2 * q->id + 1) * c->stride, q->head);
    if (done.cid != cmd->cid || done.sqid != q->id) {
        c->failed = true;
        return -E_IO;
    }
    if (result)
        *result = done.result;
    if (done.status >> 1) {
        pr_err("nvme%u: opcode %x status %x\n", c->index, cmd->op, done.status >> 1);
        return -E_IO;
    }
    return 0;
}

static void data_prps(struct controller *c, struct command *cmd, size_t bytes) {
    cmd->prp1 = c->dma_phys;
    if (bytes <= 4096)
        cmd->prp2 = 0;
    else if (bytes <= 8192)
        cmd->prp2 = c->dma_phys + 4096;
    else {
        uint64_t *list = phys_ptr(c->prps_phys);
        for (unsigned i = 0; i < (bytes + 4095) / 4096 - 1; i++)
            list[i] = c->dma_phys + (i + 1) * 4096;
        cmd->prp2 = c->prps_phys;
    }
}

static int nvme_transfer(struct block_device *d, uint64_t lba, unsigned count, void *data,
                         bool write) {
    struct ns *ns = d->private;
    struct controller *c = ns->controller;

    mutex_lock(&c->lock);
    int error = 0;
    while (count) {
        unsigned n = min(count, c->max_transfer / d->sector_size);
        if (!n) {
            error = -E_IO;
            break;
        }
        size_t bytes = (size_t)n * d->sector_size;
        if (write)
            memcpy(c->dma, data, bytes);

        struct command cmd = {
            .op = write ? 1 : 2,
            .nsid = ns->id,
            .dword = {(uint32_t)lba, lba >> 32, (n - 1) | (write ? (1u << 30) : 0), 0, 0, 0}};
        data_prps(c, &cmd, bytes);
        error = submit(c, &c->io, &cmd, NULL);
        if (error)
            break;

        if (!write)
            memcpy(data, c->dma, bytes);
        data = (uint8_t *)data + bytes;
        lba += n;
        count -= n;
    }

    mutex_unlock(&c->lock);
    return error;
}

static int nvme_flush(struct block_device *d) {
    struct ns *ns = d->private;
    struct controller *c = ns->controller;
    mutex_lock(&c->lock);
    struct command cmd = {.op = 0, .nsid = ns->id};
    int e = submit(c, &c->io, &cmd, NULL);
    mutex_unlock(&c->lock);
    return e;
}

static int identify(struct controller *c, unsigned nsid, unsigned cns) {
    memset(c->dma, 0, 4096);
    struct command cmd = {
        .op = 6, .nsid = nsid, .prp1 = c->dma_phys, .dword = {cns, 0, 0, 0, 0, 0}};
    return submit(c, &c->admin, &cmd, NULL);
}

void nvme_probe(pci_device_header_t *h) {
    if (controller_count == NVME_MAX || (h->header_type & 0x7f) || (h->bar[0] & 1))
        return;

    uint64_t bar = h->bar[0] & ~15u;
    if ((h->bar[0] & 6) == 4)
        bar |= (uint64_t)h->bar[1] << 32;
    if (!bar)
        return;

    struct controller *c = &controllers[controller_count];
    c->index = controller_count++;

    /* PCI Command is 16 bits, avoiding W1C writes to Status. */
    volatile uint16_t *command = (void *)((uintptr_t)h + 4);
    *command = (*command | 6 | 0x400);
    c->regs = vmem_map_mmio(bar, 4096);
    if (!c->regs)
        return;

    uint64_t cap = *(volatile uint64_t *)c->regs;
    if ((cap & 0xffff) + 1 < QUEUE_DEPTH || ((cap >> 48) & 15) != 0 || !(cap & (1ULL << 37))) {
        pr_warn("nvme%u: unsupported queue/page/command set capability\n", c->index);
        return;
    }
    c->stride = 4u << ((cap >> 32) & 15);
    c->regs = vmem_map_mmio(bar, 0x1000 + 4 * c->stride);
    if (!c->regs)
        return;

    write32(c, 0x0c, 0xffffffff); /* mask interrupts, completion polling */
    uint64_t timeout = max(500000ULL, ((cap >> 24) & 255) * 500000ULL);
    write32(c, 0x14, reg32(c, 0x14) & ~1u);
    if (!wait_ready(c, false, timeout)) {
        c->failed = true;
        return;
    }

    if (!allocate_queue(&c->admin, 0) || !allocate_queue(&c->io, 1))
        return;
    c->dma_phys = pmm_alloc_pages(DMA_PAGES);
    c->prps_phys = pmm_alloc_pages(1);
    if (!c->dma_phys || !c->prps_phys)
        return;
    c->dma = phys_ptr(c->dma_phys);
    c->max_transfer = DMA_PAGES * 4096;

    write32(c, 0x24, (QUEUE_DEPTH - 1) | ((QUEUE_DEPTH - 1) << 16));
    write64(c, 0x28, c->admin.sq_phys);
    write64(c, 0x30, c->admin.cq_phys);
    write32(c, 0x14, 1 | (6u << 16) | (4u << 20));
    if (!wait_ready(c, true, timeout) || identify(c, 0, 1)) {
        c->failed = true;
        return;
    }

    uint8_t mdts = ((uint8_t *)c->dma)[77];
    if (mdts && mdts < 6)
        c->max_transfer = min(c->max_transfer, 4096u << mdts);

    struct command queues = {.op = 9, .dword = {7, 0, 0, 0, 0, 0}};
    if (submit(c, &c->admin, &queues, NULL))
        return; /* request one I/O queue */
    struct command cq = {
        .op = 5, .prp1 = c->io.cq_phys, .dword = {1 | ((QUEUE_DEPTH - 1) << 16), 1, 0, 0, 0, 0}};
    struct command sq = {.op = 1,
                         .prp1 = c->io.sq_phys,
                         .dword = {1 | ((QUEUE_DEPTH - 1) << 16), 1 | (1u << 16), 0, 0, 0, 0}};
    if (submit(c, &c->admin, &cq, NULL) || submit(c, &c->admin, &sq, NULL))
        return;

    uint32_t active[1024];
    if (identify(c, 0, 2))
        return;
    memcpy(active, c->dma, sizeof(active));
    for (unsigned i = 0; i < 1024 && active[i] && namespace_count < ARRAY_SIZE(namespaces); i++) {
        unsigned id = active[i];
        if (identify(c, id, 0))
            continue;
        uint8_t *b = c->dma;
        unsigned format = b[26] & 15;
        unsigned ds = b[128 + format * 4 + 2];
        uint64_t size = get_le64(b);
        if ((b[26] & 0xe0) || (b[29] & 7) || get_le16(b + 128 + format * 4) || ds < 9 || ds > 12 ||
            !size) {
            pr_warn("nvme%un%u: unsupported namespace LBA format\n", c->index, id);
            continue;
        }

        struct ns *ns = &namespaces[namespace_count++];
        ns->controller = c;
        ns->id = id;
        struct block_device d = {.sector_size = 1u << ds,
                                 .sectors = size,
                                 .private = ns,
                                 .transfer = nvme_transfer,
                                 .flush = nvme_flush};
        snprintf(d.name, sizeof(d.name), "nvme%un%u", c->index, id);
        if (block_register(&d))
            pr_info("%s: %llu MiB, %u byte sectors\n", d.name,
                    (unsigned long long)(size * d.sector_size >> 20), d.sector_size);
    }
}
