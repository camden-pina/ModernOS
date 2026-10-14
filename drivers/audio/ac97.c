/* Intel ICH AC'97 PCM output. DMA sees kernel-owned, below-4-GiB buffers;
 * no user pointer or renderer-owned mapping is ever submitted to hardware. */
#include <drivers/audio.h>
#include <drivers/pci/pci.h>
#include <io.h>
#include <mm/pmm.h>
#include <mm/user.h>
#include <sync.h>
#include <string.h>
#include <tsc.h>
#include <printk.h>
#include <uapi/abi.h>
#include <uapi/audio.h>

#define AUDIO_RING 32U
#define AUDIO_BYTES (OS_AUDIO_MAX_FRAMES * 4U)

struct audio_descriptor {
    uint32_t address;
    uint16_t samples, control;
} __attribute__((packed));
static uint16_t mixer, busmaster;
static uint64_t table_phys, buffers_phys;
static volatile struct audio_descriptor *table;
static unsigned head, tail, count, owner;
static uint64_t played;
static spinlock_t audio_lock;

static bool reset_engine(void) {
    outb(busmaster + 0x1b, 0);
    outb(busmaster + 0x1b, 2);
    uint64_t deadline = tsc_uptime_us() + 10000;
    while ((inb(busmaster + 0x1b) & 2) && tsc_uptime_us() < deadline)
        __asm__ volatile("pause");
    if (inb(busmaster + 0x1b) & 2)
        return false;
    head = tail = count = 0;
    memset((void *)table, 0, PAGE_SIZE);
    for (unsigned i = 0; i < AUDIO_RING; i++)
        table[i].address = buffers_phys + i * AUDIO_BYTES;
    outdw(busmaster + 0x10, table_phys);
    outw(busmaster + 0x16, 0x1c);
    return true;
}

bool ac97_probe(void *pci_header) {
    volatile pci_device_header_t *pci = pci_header;
    if (busmaster || pci->vendor_id != 0x8086 ||
        (pci->device_id != 0x2415 && pci->device_id != 0x2425 && pci->device_id != 0x2445))
        return false;
    if (!(pci->bar[0] & 1) || !(pci->bar[1] & 1) || pci->bar[0] > 0xffff || pci->bar[1] > 0xffff)
        return false;
    mixer = pci->bar[0] & ~3U;
    busmaster = pci->bar[1] & ~3U;
    *(volatile uint16_t *)&pci->command |= 5U | (1U << 10); /* I/O + bus master; polling only */
    outdw(busmaster + 0x2c, indw(busmaster + 0x2c) | 2U);
    uint64_t deadline = tsc_uptime_us() + 100000;
    while (!(indw(busmaster + 0x30) & (1U << 8)) && tsc_uptime_us() < deadline)
        __asm__ volatile("pause");
    if (!(indw(busmaster + 0x30) & (1U << 8)))
        goto failed;
    outw(mixer, 0);
    outw(mixer + 2, 0);
    outw(mixer + 0x18, 0);
    table_phys = pmm_alloc_pages(1);
    buffers_phys = pmm_alloc_pages(AUDIO_RING * AUDIO_BYTES / PAGE_SIZE);
    if (!table_phys || !buffers_phys || table_phys > UINT32_MAX - PAGE_SIZE ||
        buffers_phys > UINT32_MAX - AUDIO_RING * AUDIO_BYTES)
        goto failed;
    table = phys_ptr(table_phys);
    if (!reset_engine())
        goto failed;
    pr_info("Audio: Intel AC97, 48000-Hz stereo signed 16-bit PCM (polled DMA)\n");
    return true;
failed:
    outb(busmaster + 0x1b, 0);
    if (table_phys)
        pmm_free_pages(table_phys, 1);
    if (buffers_phys)
        pmm_free_pages(buffers_phys, AUDIO_RING * AUDIO_BYTES / PAGE_SIZE);
    table_phys = buffers_phys = 0;
    table = NULL;
    mixer = busmaster = 0;
    return false;
}

static void reap(void) {
    if (!count)
        return;
    unsigned current = inb(busmaster + 0x14) & 31;
    while (count && head != current) {
        played += table[head].samples / 2;
        head = (head + 1) & 31;
        count--;
    }
    if (count && (inw(busmaster + 0x16) & 1) && !inw(busmaster + 0x18)) {
        played += table[head].samples / 2;
        head = (head + 1) & 31;
        count--;
    }
    outw(busmaster + 0x16, 0x1c);
}

long ac97_call(struct task *task, unsigned operation, uint64_t pointer, uint64_t frames) {
    if (operation > OS_AUDIO_RESET || (operation != OS_AUDIO_WRITE && frames))
        return -E_INVAL;
    if (!busmaster)
        return -E_NODEV;
    int16_t samples[OS_AUDIO_MAX_FRAMES * 2];
    if (operation == OS_AUDIO_WRITE) {
        if (!frames || frames > OS_AUDIO_MAX_FRAMES)
            return -E_INVAL;
        if (!user_copy_in(&task->space, samples, pointer, frames * 4))
            return -E_FAULT;
    } else if (operation == OS_AUDIO_RESET && pointer)
        return -E_INVAL;
    struct os_audio_info info = {.version = 1, .rate = 48000, .channels = 2, .sample_bits = 16};
    uint64_t flags = spin_lock_irqsave(&audio_lock);
    reap();
    long result = 0;
    if (operation == OS_AUDIO_INFO) {
        info.played_frames = played;
        for (unsigned i = 0; i < count; i++)
            info.queued_frames += table[(head + i) & 31].samples / 2;
        if (count) {
            unsigned remaining = inw(busmaster + 0x18) / 2;
            unsigned full = table[head].samples / 2;
            if (remaining <= full)
                info.queued_frames -= full - remaining;
        }
    } else if (owner && owner != task->tgid && count)
        result = -E_BUSY;
    else if (operation == OS_AUDIO_RESET) {
        if (!reset_engine())
            result = -E_IO;
        owner = 0;
        played = 0;
    } else if (count >= AUDIO_RING - 1)
        result = -E_AGAIN;
    else {
        if (!count || owner != task->tgid) {
            if (!reset_engine())
                result = -E_IO;
            if (owner != task->tgid)
                played = 0;
            owner = task->tgid;
        }
        if (!result) {
            memcpy(phys_ptr(buffers_phys + tail * AUDIO_BYTES), samples, frames * 4);
            table[tail].samples = frames * 2;
            table[tail].control = 0;
            __asm__ volatile("mfence" ::: "memory");
            outb(busmaster + 0x15, tail);
            tail = (tail + 1) & 31;
            count++;
            /* Updating LVI extends an active DMA stream. Writing RUN again
             * restarts descriptor fetching in ICH emulation and drops the
             * current buffer, making queued audio appear to have played. */
            if (!(inb(busmaster + 0x1b) & 1))
                outb(busmaster + 0x1b, 1);
            result = frames;
        }
    }
    spin_unlock_irqrestore(&audio_lock, flags);
    if (operation == OS_AUDIO_INFO && !user_copy_out(&task->space, pointer, &info, sizeof(info)))
        return -E_FAULT;
    return result;
}
