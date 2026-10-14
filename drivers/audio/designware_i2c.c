/* Intel LPSS / Synopsys DesignWare register programming. No user MMIO or I/O
 * capabilities are exposed. FIFO and abort polling have bounded deadlines. */
#include <drivers/designware_i2c.h>
#include <drivers/pci/pci.h>
#include <mm/vmem_io.h>
#include <tsc.h>
#include <sync.h>
#include <printk.h>

static volatile uint8_t *i2c_registers;
static mutex_t i2c_mutex;

static uint32_t i2c_read(unsigned offset) {
    return *(volatile uint32_t *)(i2c_registers + offset);
}

static void i2c_write(unsigned offset, uint32_t value) {
    *(volatile uint32_t *)(i2c_registers + offset) = value;
}

static bool i2c_wait(unsigned offset, uint32_t mask, uint32_t value, unsigned microseconds) {
    uint64_t deadline = tsc_uptime_us() + microseconds;
    do {
        if ((i2c_read(offset) & mask) == value)
            return true;
        __asm__ volatile("pause");
    } while (tsc_uptime_us() < deadline);
    return false;
}

bool designware_i2c_probe(void *device) {
    volatile pci_device_header_t *pci = device;
    if (i2c_registers || pci->vendor_id != 0x8086 || pci->device_id != 0x7f4e || (pci->bar[0] & 1))
        return false;

    uint64_t physical = pci->bar[0] & ~15U;
    if ((pci->bar[0] & 6) == 4)
        physical |= (uint64_t)pci->bar[1] << 32;
    if (!physical)
        return false;

    bool seen[64] = {0};
    unsigned cap = *((volatile uint8_t *)pci + 0x34);
    while (cap >= 0x40 && cap <= 0xfc && !(cap & 3) && !seen[cap >> 2]) {
        seen[cap >> 2] = true;
        volatile uint8_t *p = (volatile uint8_t *)pci + cap;
        if (p[0] == 1 && cap <= 0xf8) {
            *(volatile uint16_t *)(p + 4) &= ~3U;
            break;
        }
        cap = p[1];
    }

    *(volatile uint16_t *)&pci->command |= 2U | (1U << 10);
    i2c_registers = vmem_map_mmio(physical, 4096);
    if (!i2c_registers)
        return false;

    i2c_write(0x204, 7); /* LPSS function and IDMA reset deassertion. */
    uint64_t deadline = tsc_uptime_us() + 1000;
    while (tsc_uptime_us() < deadline)
        __asm__ volatile("pause");
    if (i2c_read(0xfc) != 0x44570140)
        goto failed;

    i2c_write(0x6c, 0);
    if (!i2c_wait(0x9c, 1, 0, 10000))
        goto failed;
    i2c_write(0x30, 0);
    i2c_read(0x40);
    i2c_write(0x00, 0x63);

    /* 133-MHz LPSS input, conservative 100-kHz standard-mode bus. */
    i2c_write(0x14, 600);
    i2c_write(0x18, 750);
    i2c_write(0x7c, 40);
    i2c_write(0x38, 0);
    i2c_write(0x3c, 0);

    pr_info("Audio: Arrow Lake DesignWare I2C2 ready\n");
    return true;

failed:
    i2c_registers = NULL;
    return false;
}

bool designware_i2c_transfer(unsigned address, const uint8_t *write, size_t write_size,
                             uint8_t *read, size_t read_size) {
    if (!i2c_registers || address < 0x34 || address > 0x35 || !write || !write_size ||
        write_size > 129 || read_size > 2 || (read_size && !read))
        return false;

    mutex_lock(&i2c_mutex);
    bool success = false;
    i2c_write(0x6c, 0);
    if (!i2c_wait(0x9c, 1, 0, 10000))
        goto done;

    i2c_read(0x40);
    i2c_write(0x04, address);
    i2c_write(0x6c, 1);

    size_t sent = 0, received = 0, total = write_size + read_size;
    uint64_t deadline = tsc_uptime_us() + 30000;
    while (tsc_uptime_us() < deadline) {
        if (i2c_read(0x34) & (1U << 6)) {
            i2c_read(0x54);
            goto done;
        }

        if (sent < total && (i2c_read(0x70) & 2)) {
            uint32_t command = sent < write_size ? write[sent] : 1U << 8;
            if (read_size && sent == write_size)
                command |= 1U << 10;
            if (sent + 1 == total)
                command |= 1U << 9;
            i2c_write(0x10, command);
            sent++;
        }

        if (received < read_size && (i2c_read(0x70) & 8))
            read[received++] = i2c_read(0x10);

        if (sent == total && received == read_size && (i2c_read(0x34) & (1U << 9)) &&
            !(i2c_read(0x70) & (1U << 5))) {
            i2c_read(0x60);
            success = true;
            break;
        }

        __asm__ volatile("pause");
    }

done:
    i2c_write(0x6c, 0);
    i2c_wait(0x9c, 1, 0, 10000);
    mutex_unlock(&i2c_mutex);

    return success;
}
