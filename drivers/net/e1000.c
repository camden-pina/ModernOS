#include <net/e1000.h>
#include <drivers/pci/pci.h>
#include <mm/pmm.h>
#include <mm/user.h>
#include <mm/vmem_io.h>
#include <string.h>
#include <tsc.h>
#include <printk.h>
#include <uapi/abi.h>

#define RING 128
#define BUFFER 2048

struct rx_desc {
    uint64_t address;
    uint16_t length, checksum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed));
struct tx_desc {
    uint64_t address;
    uint16_t length;
    uint8_t cso, command, status, css;
    uint16_t special;
} __attribute__((packed));
static volatile uint32_t *registers;
static volatile struct rx_desc *receive_ring;
static volatile struct tx_desc *transmit_ring;
static uint64_t receive_phys, transmit_phys, receive_buffers, transmit_buffers;
static unsigned receive_head, transmit_head;
static uint8_t address[6];

static uint32_t reg(unsigned offset) {
    return registers[offset / 4];
}

static void put(unsigned offset, uint32_t value) {
    registers[offset / 4] = value;
}

static void barrier(void) {
    __asm__ volatile("mfence" ::: "memory");
}

static void delay(unsigned us) {
    uint64_t end = tsc_uptime_us() + us;
    while (tsc_uptime_us() < end)
        __asm__ volatile("pause");
}

bool e1000_present(void) {
    return registers != NULL;
}

bool e1000_link(void) {
    return registers && (reg(8) & 2);
}

void e1000_mac(uint8_t out[6]) {
    memcpy(out, address, 6);
}

bool e1000_probe(void *pci_header) {
    volatile pci_device_header_t *pci = pci_header;
    if (registers || pci->vendor_id != 0x8086 ||
        (pci->device_id != 0x10d3 && pci->device_id != 0x100e))
        return false;
    uint32_t bar = pci->bar[0];
    if (bar & 1)
        return false;
    uint64_t physical = bar & ~15u;
    if ((bar & 6) == 4)
        physical |= (uint64_t)pci->bar[1] << 32;
    if (!physical)
        return false;
    registers = vmem_map_mmio(physical, 128 * 1024);
    if (!registers)
        return false;
    /* Polling only: the device never receives a live interrupt vector. */
    *(volatile uint16_t *)&pci->command |= 6 | (1u << 10);
    put(0xd8, UINT32_MAX);
    put(0, reg(0) | (1u << 26));
    uint64_t end = tsc_uptime_us() + 100000;
    while ((reg(0) & (1u << 26)) && tsc_uptime_us() < end)
        __asm__ volatile("pause");
    if (reg(0) & (1u << 26))
        goto failure;
    delay(1000);
    put(0xd8, UINT32_MAX);
    (void)reg(0xc0);
    uint32_t low = reg(0x5400), high = reg(0x5404);
    if (!(high & (1u << 31))) {
        for (unsigned i = 0; i < 3; i++) {
            unsigned shift = pci->device_id == 0x10d3 ? 2 : 8;
            unsigned done = pci->device_id == 0x10d3 ? 2 : 16;
            put(0x14, (i << shift) | 1);
            end = tsc_uptime_us() + 10000;
            while (!(reg(0x14) & done) && tsc_uptime_us() < end)
                __asm__ volatile("pause");
            if (!(reg(0x14) & done))
                goto failure;
            uint16_t word = reg(0x14) >> 16;
            address[i * 2] = word;
            address[i * 2 + 1] = word >> 8;
        }
        low = address[0] | ((uint32_t)address[1] << 8) | ((uint32_t)address[2] << 16) |
              ((uint32_t)address[3] << 24);
        high = address[4] | ((uint32_t)address[5] << 8) | (1u << 31);
    } else {
        for (unsigned i = 0; i < 4; i++)
            address[i] = low >> (i * 8);
        address[4] = high;
        address[5] = high >> 8;
    }

    bool valid = false;
    for (unsigned i = 0; i < 6; i++)
        valid |= address[i] != 0;
    if (!valid || (address[0] & 1))
        goto failure;

    put(0x5400, low);
    put(0x5404, high);
    for (unsigned i = 0; i < 128; i++)
        put(0x5200 + i * 4, 0);

    receive_phys = pmm_alloc_pages(1);
    transmit_phys = pmm_alloc_pages(1);
    receive_buffers = pmm_alloc_pages(RING * BUFFER / PAGE_SIZE);
    transmit_buffers = pmm_alloc_pages(RING * BUFFER / PAGE_SIZE);
    if (!receive_phys || !transmit_phys || !receive_buffers || !transmit_buffers)
        goto failure;

    receive_ring = phys_ptr(receive_phys);
    transmit_ring = phys_ptr(transmit_phys);
    memset((void *)receive_ring, 0, PAGE_SIZE);
    memset((void *)transmit_ring, 0, PAGE_SIZE);
    for (unsigned i = 0; i < RING; i++) {
        receive_ring[i].address = receive_buffers + i * BUFFER;
        transmit_ring[i].address = transmit_buffers + i * BUFFER;
        transmit_ring[i].status = 1;
    }

    put(0x2800, receive_phys);
    put(0x2804, receive_phys >> 32);
    put(0x2808, RING * sizeof(struct rx_desc));
    put(0x2810, 0);
    put(0x2818, RING - 1);

    put(0x3800, transmit_phys);
    put(0x3804, transmit_phys >> 32);
    put(0x3808, RING * sizeof(struct tx_desc));
    put(0x3810, 0);
    put(0x3818, 0);

    put(0x2828, (1u << 25) | (8u << 16) | (8u << 8) | 8);
    put(0x3828, (1u << 25) | (8u << 16) | (8u << 8) | 8);
    put(0x410, 0x0060200a);
    put(0x400, (1u << 1) | (1u << 3) | (15u << 4) | (64u << 12));
    put(0x100, (1u << 1) | (1u << 15) | (1u << 26)); /* 2048-byte buffers, strip CRC, broadcast */
    put(0, (reg(0) | (1u << 6)) & ~((1u << 3) | (1u << 7) | (1u << 31)));
    barrier();

    pr_info("Network: Intel Ethernet %04x MAC %02x:%02x:%02x:%02x:%02x:%02x (polled)\n",
            pci->device_id, address[0], address[1], address[2], address[3], address[4], address[5]);
    return true;

failure:
    put(0x100, 0);
    put(0x400, 0);
    put(0xd8, UINT32_MAX);

    if (receive_phys)
        pmm_free_pages(receive_phys, 1);
    if (transmit_phys)
        pmm_free_pages(transmit_phys, 1);
    if (receive_buffers)
        pmm_free_pages(receive_buffers, RING * BUFFER / PAGE_SIZE);
    if (transmit_buffers)
        pmm_free_pages(transmit_buffers, RING * BUFFER / PAGE_SIZE);

    registers = NULL;
    receive_phys = transmit_phys = receive_buffers = transmit_buffers = 0;
    pr_warn("Network: Intel Ethernet initialization failed\n");
    return false;
}

long e1000_send(const void *frame, size_t length) {
    if (!registers || !e1000_link())
        return -E_NETDOWN;
    if (!length || length > 1514)
        return -E_MSGSIZE;

    volatile struct tx_desc *descriptor = &transmit_ring[transmit_head];
    if (!(descriptor->status & 1))
        return -E_AGAIN;

    barrier();
    memcpy(phys_ptr(transmit_buffers + transmit_head * BUFFER), frame, length);
    descriptor->length = length;
    descriptor->command = 1 | 2 | 8;
    descriptor->status = 0;

    barrier();

    transmit_head = (transmit_head + 1) % RING;
    put(0x3818, transmit_head);
    return length;
}

long e1000_receive(void *frame, size_t capacity) {
    if (!registers)
        return -E_NETDOWN;

    volatile struct rx_desc *descriptor = &receive_ring[receive_head];
    if (!(descriptor->status & 1))
        return -E_AGAIN;

    barrier();
    unsigned length = descriptor->length;
    bool good = (descriptor->status & 2) && !descriptor->errors && length >= 14 && length <= 1514 &&
                length <= capacity;
    if (good)
        memcpy(frame, phys_ptr(receive_buffers + receive_head * BUFFER), length);

    descriptor->status = 0;

    barrier();
    put(0x2818, receive_head);
    receive_head = (receive_head + 1) % RING;
    return good ? (long)length : 0;
}
