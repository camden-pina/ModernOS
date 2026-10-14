/* Single polled queue for Intel I225/I226 and the 82576 DMA interface.
 * Register/descriptor facts: Intel I225/I226 Software User Manual; Intel's
 * upstream Linux igc definitions. This implementation owns all DMA memory;
 * lwIP's network mutex serializes it. No userspace MMIO or PHY capabilities.
 */
#include <net/igc.h>
#include <drivers/pci/pci.h>
#include <mm/pmm.h>
#include <mm/user.h>
#include <mm/vmem_io.h>
#include <string.h>
#include <tsc.h>
#include <printk.h>
#include <uapi/abi.h>

enum { RING = 128, BUFFER = 2048, BUFFER_PAGES = RING * BUFFER / PAGE_SIZE };
enum {
    CTRL = 0,
    STATUS = 8,
    CTRL_EXT = 0x18,
    MDIC = 0x20,
    EECD = 0x10,
    IMC = 0xd8,
    ICR = 0xc0,
    EIMC = 0x1528,
    RCTL = 0x100,
    TCTL = 0x400,
    RX_BASE = 0xc000,
    TX_BASE = 0xe000,
    RAL = 0x5400,
    RAH = 0x5404,
    SWSM = 0x5b50,
    SWFW = 0x5b5c,
    PHPM = 0xe14,
    MANC = 0x5820
};

struct rx_descriptor {
    union {
        struct {
            uint64_t packet, header;
        } read;
        struct {
            uint64_t metadata;
            uint32_t status;
            uint16_t length, vlan;
        } done;
    };
} __attribute__((packed));
struct tx_descriptor {
    uint64_t packet;
    uint32_t command, status;
} __attribute__((packed));
_Static_assert(sizeof(struct rx_descriptor) == 16 && sizeof(struct tx_descriptor) == 16,
               "Intel DMA descriptor ABI");
static volatile uint32_t *registers;
static volatile struct rx_descriptor *rx;
static volatile struct tx_descriptor *tx;
static uint64_t rx_physical, tx_physical, rx_buffers, tx_buffers;
static unsigned rx_head, tx_head;
static uint8_t mac[6];
static bool initialized;
static unsigned phy_address;

static uint32_t read_reg(unsigned offset) {
    return registers[offset / 4];
}

static void write_reg(unsigned offset, uint32_t value) {
    registers[offset / 4] = value;
}

static void fence(void) {
    __asm__ volatile("mfence" ::: "memory");
}

static void flush(void) {
    (void)read_reg(STATUS);
}

static void delay(unsigned us) {
    uint64_t end = tsc_uptime_us() + us;
    while (tsc_uptime_us() < end)
        __asm__ volatile("pause");
}

static bool wait_reg(unsigned offset, uint32_t mask, uint32_t value, unsigned us) {
    uint64_t end = tsc_uptime_us() + us;
    do {
        if ((read_reg(offset) & mask) == value)
            return true;
        __asm__ volatile("pause");
    } while (tsc_uptime_us() < end);
    return false;
}

static void semaphore_release(void) {
    write_reg(SWSM, read_reg(SWSM) & ~3U);
    flush();
}

static bool semaphore_acquire(void) {
    uint64_t end = tsc_uptime_us() + 50000;
    while (tsc_uptime_us() < end) {
        if (!(read_reg(SWSM) & 1)) {
            write_reg(SWSM, read_reg(SWSM) | 2);
            if (read_reg(SWSM) & 2)
                return true;
            semaphore_release();
        }
        delay(50);
    }
    return false;
}

static bool phy_acquire(void) {
    uint64_t end = tsc_uptime_us() + 200000;
    while (tsc_uptime_us() < end) {
        if (!semaphore_acquire())
            return false;
        uint32_t owners = read_reg(SWFW);
        if (!(owners & 0x00020002)) {
            write_reg(SWFW, owners | 2);
            semaphore_release();
            return true;
        }
        semaphore_release();
        delay(1000);
    }
    return false;
}

static bool phy_release(void) {
    if (!semaphore_acquire())
        return false;
    write_reg(SWFW, read_reg(SWFW) & ~2U);
    semaphore_release();
    return true;
}

static bool mdi(unsigned reg, uint16_t *value, bool writing) {
    if (reg > 31)
        return false;
    /* The integrated I225/I226 GPY PHY has MDIO address zero. */
    write_reg(MDIC, (phy_address << 21) | (reg << 16) | (writing ? 0x04000000U : 0x08000000U) |
                        (writing ? *value : 0));
    if (!wait_reg(MDIC, 0x10000000, 0x10000000, 10000))
        return false;
    uint32_t result = read_reg(MDIC);
    if ((result & 0x40000000) || ((result >> 16) & 31) != reg)
        return false;
    if (!writing)
        *value = result;
    return true;
}

static bool mdi_write(unsigned reg, uint16_t value) {
    return mdi(reg, &value, true);
}

static bool configure_phy(void) {
    /* Firmware management can intentionally retain ownership of a live PHY. */
    if (read_reg(MANC) & 0x40000)
        return true;
    if (!phy_acquire())
        return false;
    bool good = false;
    uint32_t control = read_reg(CTRL);
    write_reg(CTRL, control | 0x80000000U);
    flush();
    delay(100);
    write_reg(CTRL, control);
    flush();
    delay(100);
    if (!wait_reg(PHPM, 0x100, 0x100, 10000))
        goto done;
    uint16_t id1, id2, basic, advertise, gigabit, multigig;
    if (!mdi(2, &id1, false) || !mdi(3, &id2, false) || !id1 || id1 == 0xffff || id2 == 0xffff)
        goto done;
    if (!mdi(0, &basic, false) || !mdi(4, &advertise, false) || !mdi(9, &gigabit, false))
        goto done;
    /* No pause-frame offload; offer 10/100 and full-duplex 1000/2500. */
    if (!mdi_write(4, (advertise & ~0x0fe0U) | 0x01e0U) ||
        !mdi_write(9, (gigabit & ~0x0300U) | 0x0200U))
        goto done;
    if (!mdi_write(13, 7) || !mdi_write(14, 0x20) || !mdi_write(13, 0x4007) ||
        !mdi(14, &multigig, false) || !mdi_write(14, multigig | 0x80))
        goto done;
    if (!mdi_write(13, 0) || !mdi_write(0, (basic & ~0x4c00U) | 0x1200U))
        goto done;
    good = true;
done:
    if (!good)
        (void)mdi_write(13, 0);
    if (!phy_release()) {
        pr_warn("Network: I226 PHY ownership release failed\n");
        return false;
    }
    return good;
}

static bool stop_dma(volatile pci_device_header_t *pci) {
    write_reg(IMC, UINT32_MAX);
    write_reg(EIMC, UINT32_MAX);
    write_reg(RCTL, 0);
    write_reg(TCTL, 0);
    write_reg(RX_BASE + 0x28, 0);
    write_reg(TX_BASE + 0x28, 0);
    write_reg(CTRL, read_reg(CTRL) | 4);
    flush();
    bool stopped = wait_reg(STATUS, 0x80000, 0, 100000);
    if (!stopped) {
        write_reg(CTRL, read_reg(CTRL) | 0x04000000);
        flush();
        stopped = wait_reg(CTRL, 0x04000000, 0, 100000) && wait_reg(STATUS, 0x80000, 0, 100000);
    }
    *(volatile uint16_t *)&pci->command &= ~4U;
    return stopped;
}

bool igc_present(void) {
    return initialized;
}

bool igc_link(void) {
    return initialized && (read_reg(STATUS) & 2);
}

void igc_mac(uint8_t out[6]) {
    memcpy(out, mac, 6);
}

bool igc_probe(void *device) {
    volatile pci_device_header_t *pci = device;
    bool physical = pci->device_id == 0x125c || pci->device_id == 0x125b ||
                    pci->device_id == 0x15f3 || pci->device_id == 0x15f2;
    if (registers || pci->vendor_id != 0x8086 || (!physical && pci->device_id != 0x10c9) ||
        (pci->bar[0] & 1))
        return false;
    uint64_t base = pci->bar[0] & ~15U;
    if ((pci->bar[0] & 6) == 4)
        base |= (uint64_t)pci->bar[1] << 32;
    if (!base)
        return false;
    /* Bring a firmware-suspended PCI function back to D0. */
    bool seen[64] = {0};
    unsigned cap = *((volatile uint8_t *)pci + 0x34);
    while (cap >= 0x40 && cap <= 0xfc && !(cap & 3) && !seen[cap >> 2]) {
        seen[cap >> 2] = true;
        volatile uint8_t *p = (volatile uint8_t *)pci + cap;
        if (p[0] == 1 && cap <= 0xf8) {
            *(volatile uint16_t *)(p + 4) &= ~3U;
            delay(10000);
            break;
        }
        cap = p[1];
    }
    registers = vmem_map_mmio(base, 128 * 1024);
    if (!registers)
        return false;
    *(volatile uint16_t *)&pci->command |= 6U | (1U << 10);
    const char *stage = "DMA quiesce";
    if (!stop_dma(pci))
        goto failed;
    stage = "MAC reset";
    write_reg(CTRL, read_reg(CTRL) | 0x04000000);
    flush();
    if (!wait_reg(CTRL, 0x04000000, 0, 100000))
        goto failed;
    delay(10000);
    stage = "NVM auto-read";
    if (physical && !wait_reg(EECD, 0x200, 0x200, 100000))
        goto failed;
    write_reg(IMC, UINT32_MAX);
    write_reg(EIMC, UINT32_MAX);
    (void)read_reg(ICR);
    uint32_t low = read_reg(RAL), high = read_reg(RAH);
    stage = "MAC address";
    if (!(high & 0x80000000))
        goto failed;
    for (unsigned i = 0; i < 4; i++)
        mac[i] = low >> (i * 8);
    mac[4] = high;
    mac[5] = high >> 8;
    bool valid = false;
    for (unsigned i = 0; i < 6; i++)
        valid |= mac[i] != 0;
    if (!valid || (mac[0] & 1))
        goto failed;
    write_reg(CTRL, (read_reg(CTRL) & ~(4U | 1U | 0x1800U | 0x0300U | 0x18000000U)) | 0x40);
    stage = "PHY configuration";
    phy_address = physical ? 0 : 1;
    if (physical && !configure_phy())
        goto failed;
    if (!physical) {
        /* The 82576 emulator resets link state until PHY auto-negotiation
         * restarts; its Clause-22 PHY lives at address one. */
        uint16_t basic;
        if (!mdi(0, &basic, false) || !mdi_write(0, (basic & ~0x4c00U) | 0x1200U))
            goto failed;
    }
    for (unsigned i = 1; i < 16; i++) {
        write_reg(RAL + i * 8, 0);
        write_reg(RAH + i * 8, 0);
    }
    for (unsigned i = 0; i < 128; i++) {
        write_reg(0x5200 + i * 4, 0);
        write_reg(0xa000 + i * 4, 0);
    }
    if (physical) {
        write_reg(0x2404, 34U | (2U << 6));
        write_reg(0x3404, 20U | (4U << 24));
    }
    write_reg(0x5818, 0); /* One queue, no RSS/VMDq. */
    write_reg(0x5000, 0); /* lwIP validates checksums. */

    stage = "DMA allocation";
    rx_physical = pmm_alloc_pages(1);
    tx_physical = pmm_alloc_pages(1);
    rx_buffers = pmm_alloc_pages(BUFFER_PAGES);
    tx_buffers = pmm_alloc_pages(BUFFER_PAGES);
    if (!rx_physical || !tx_physical || !rx_buffers || !tx_buffers)
        goto failed;

    rx = phys_ptr(rx_physical);
    tx = phys_ptr(tx_physical);
    memset((void *)rx, 0, PAGE_SIZE);
    memset((void *)tx, 0, PAGE_SIZE);
    memset(phys_ptr(rx_buffers), 0, BUFFER_PAGES * PAGE_SIZE);
    memset(phys_ptr(tx_buffers), 0, BUFFER_PAGES * PAGE_SIZE);
    rx_head = tx_head = 0;
    for (unsigned i = 0; i < RING; i++) {
        rx[i].read.packet = rx_buffers + i * BUFFER;
        tx[i].status = 1;
    }

    write_reg(RX_BASE, rx_physical);
    write_reg(RX_BASE + 4, rx_physical >> 32);
    write_reg(RX_BASE + 8, RING * sizeof(*rx));
    write_reg(RX_BASE + 0x10, 0);
    write_reg(RX_BASE + 0x18, 0);
    write_reg(RX_BASE + 0xc, 2U | (1U << 25)); /* 2 KiB, advanced one-buffer descriptors. */

    write_reg(TX_BASE, tx_physical);
    write_reg(TX_BASE + 4, tx_physical >> 32);
    write_reg(TX_BASE + 8, RING * sizeof(*tx));
    write_reg(TX_BASE + 0x10, 0);
    write_reg(TX_BASE + 0x18, 0);
    fence();
    *(volatile uint16_t *)&pci->command |= 4;
    write_reg(RX_BASE + 0x28, (1U << 25) | 8U | (8U << 8) | (4U << 16));
    write_reg(TX_BASE + 0x28, (1U << 25) | 8U | (1U << 8) | (16U << 16));

    stage = "queue enable";
    if (!wait_reg(RX_BASE + 0x28, 1U << 25, 1U << 25, 10000) ||
        !wait_reg(TX_BASE + 0x28, 1U << 25, 1U << 25, 10000))
        goto failed;
    write_reg(0x410, 0x0060200a);
    write_reg(TCTL, 2U | 8U | (15U << 4) | (64U << 12));
    write_reg(RCTL, 2U | (1U << 15) | (1U << 26)); /* Broadcast + strip CRC, no jumbo frames. */
    write_reg(RX_BASE + 0x18, RING - 1);
    write_reg(CTRL_EXT, read_reg(CTRL_EXT) | 0x10000000);
    flush();

    initialized = true;
    pr_info("Network: Intel %s %04x MAC %02x:%02x:%02x:%02x:%02x:%02x (polled advanced DMA)\n",
            physical ? "I225/I226" : "82576", pci->device_id, mac[0], mac[1], mac[2], mac[3],
            mac[4], mac[5]);
    return true;

failed:
    pr_warn("Network: Intel initialization failed during %s (CTRL=%08x STATUS=%08x EECD=%08x)\n",
            stage, read_reg(CTRL), read_reg(STATUS), read_reg(EECD));
    if (stop_dma(pci)) {
        if (rx_physical)
            pmm_free_pages(rx_physical, 1);
        if (tx_physical)
            pmm_free_pages(tx_physical, 1);
        if (rx_buffers)
            pmm_free_pages(rx_buffers, BUFFER_PAGES);
        if (tx_buffers)
            pmm_free_pages(tx_buffers, BUFFER_PAGES);
        rx_physical = tx_physical = rx_buffers = tx_buffers = 0;
        registers = NULL;
        rx = NULL;
        tx = NULL;
    } else
        pr_warn("Network: retaining DMA memory because Intel controller did not quiesce\n");
    pr_warn("Network: Intel I225/I226/82576 initialization failed\n");
    return false;
}

long igc_send(const void *frame, size_t length) {
    if (!igc_link())
        return -E_NETDOWN;
    if (!frame || !length || length > 1514)
        return -E_MSGSIZE;
    volatile struct tx_descriptor *descriptor = &tx[tx_head];
    if (!(descriptor->status & 1))
        return -E_AGAIN;
    fence();
    memcpy(phys_ptr(tx_buffers + tx_head * BUFFER), frame, length);
    descriptor->packet = tx_buffers + tx_head * BUFFER;
    descriptor->command = 0x2b300000U | length; /* DEXT, advanced data, EOP/IFCS/RS. */
    descriptor->status = (uint32_t)length << 14;
    fence();
    tx_head = (tx_head + 1) % RING;
    write_reg(TX_BASE + 0x18, tx_head);
    return length;
}

long igc_receive(void *frame, size_t capacity) {
    if (!initialized)
        return -E_NETDOWN;
    volatile struct rx_descriptor *descriptor = &rx[rx_head];
    if (!(descriptor->done.status & 1))
        return -E_AGAIN;
    fence();
    uint32_t status = descriptor->done.status;
    unsigned length = descriptor->done.length;
    bool good = frame && (status & 2) && !(status & 0x80000000) && length >= 14 && length <= 1514 &&
                length <= capacity;
    if (good)
        memcpy(frame, phys_ptr(rx_buffers + rx_head * BUFFER), length);
    descriptor->read.header = 0;
    descriptor->read.packet = rx_buffers + rx_head * BUFFER;
    fence();
    write_reg(RX_BASE + 0x18, rx_head);
    rx_head = (rx_head + 1) % RING;
    return good ? (long)length : 0;
}
