/* Intel High Definition Audio 1.0a: CORB/RIRB codec control and a bounded,
 * kernel-owned output DMA ring. 48-kHz signed 16-bit stereo; analog pins only.
 * Register and verb definitions follow Intel's published HDA specification. */
#include <drivers/audio.h>
#include <drivers/pci/pci.h>
#include <mm/pmm.h>
#include <mm/vmem_io.h>
#include <mm/user.h>
#include <sync.h>
#include <string.h>
#include <tsc.h>
#include <printk.h>
#include <uapi/abi.h>
#include <uapi/audio.h>

#define HDA_DESCRIPTORS 32U
#define HDA_BLOCK_BYTES (OS_AUDIO_MAX_FRAMES * 4U)
#define HDA_RING_BYTES (HDA_DESCRIPTORS * HDA_BLOCK_BYTES)

struct hda_descriptor {
    uint64_t address;
    uint32_t bytes, flags;
} __attribute__((packed));
static volatile uint8_t *registers;
static uint64_t command_phys, response_phys, table_phys, samples_phys;
static volatile uint32_t *commands;
static volatile uint64_t *responses;
static unsigned command_entries, response_entries, command_write, response_read, stream;
static unsigned codec, function_group, first_node, node_count;
static unsigned queued, position, owner;
static uint64_t played;
static bool running, starting_speakers;
static spinlock_t audio_lock;
static bool legion_speakers;
void aw88399_match(uint32_t, uint32_t);
bool aw88399_start(void);
void aw88399_stop(void);

static uint8_t r8(unsigned offset) {
    return *(volatile uint8_t *)(registers + offset);
}

static uint16_t r16(unsigned offset) {
    return *(volatile uint16_t *)(registers + offset);
}

static uint32_t r32(unsigned offset) {
    return *(volatile uint32_t *)(registers + offset);
}

static void w8(unsigned offset, uint8_t value) {
    *(volatile uint8_t *)(registers + offset) = value;
}

static void w16(unsigned offset, uint16_t value) {
    *(volatile uint16_t *)(registers + offset) = value;
}

static void w32(unsigned offset, uint32_t value) {
    *(volatile uint32_t *)(registers + offset) = value;
}

static void delay(unsigned us) {
    uint64_t end = tsc_uptime_us() + us;
    while (tsc_uptime_us() < end)
        __asm__ volatile("pause");
}

static bool wait_bit(unsigned offset, uint32_t bit, bool set, unsigned timeout) {
    uint64_t end = tsc_uptime_us() + timeout;
    do {
        if (!!(r32(offset) & bit) == set)
            return true;
        __asm__ volatile("pause");
    } while (tsc_uptime_us() < end);
    return false;
}

static uint32_t verb(unsigned node, unsigned operation, unsigned payload) {
    unsigned next = (command_write + 1) % command_entries;
    commands[next] = (codec << 28) | (node << 20) | (operation << 8) | payload;
    __asm__ volatile("mfence" ::: "memory");
    command_write = next;
    w16(0x48, next);

    uint64_t end = tsc_uptime_us() + 20000;
    do {
        unsigned write = r16(0x58) & 0xff;
        if (response_read != write) {
            response_read = (response_read + 1) % response_entries;
            __asm__ volatile("lfence" ::: "memory");
            uint64_t reply = responses[response_read];
            w8(0x5d, 1); /* Acknowledge the response count without enabling PCI interrupts. */
            if (!(reply & (1ULL << 36)) && ((reply >> 32) & 15) == codec)
                return reply;
        }
        __asm__ volatile("pause");
    } while (tsc_uptime_us() < end);

    return UINT32_MAX;
}

static uint32_t parameter(unsigned node, unsigned id) {
    return verb(node, 0xf00, id);
}

static unsigned ring_size(unsigned offset, unsigned *entries) {
    unsigned caps = r8(offset);
    if (caps & 0x40) {
        *entries = 256;
        return 2;
    }
    if (caps & 0x20) {
        *entries = 16;
        return 1;
    }
    if (caps & 0x10) {
        *entries = 2;
        return 0;
    }
    *entries = 0;
    return 0;
}

static unsigned connections(unsigned node, uint8_t result[64]) {
    unsigned caps = parameter(node, 0x0e), length = caps & 0x7f, wide = !!(caps & 0x80), count = 0,
             previous = 0;
    if (caps == UINT32_MAX || length > 64)
        return 0;
    unsigned per = wide ? 2 : 4, mask = wide ? 0x7fff : 0x7f, range = wide ? 0x8000 : 0x80;
    for (unsigned i = 0; i < length; i++) {
        uint32_t values = verb(node, 0xf02, i - i % per);
        if (values == UINT32_MAX)
            return 0;
        unsigned value = (values >> ((i % per) * (wide ? 16 : 8))) & (wide ? 0xffff : 0xff),
                 last = value & mask;
        unsigned start = value & range ? previous + 1 : last;
        if (last > 255 || start > last)
            return 0;
        for (unsigned n = start; n <= last; n++) {
            if (count == 64)
                return 0;
            result[count++] = n;
        }
        previous = last;
    }
    return count;
}

struct route {
    uint8_t nodes[16], choices[16];
    unsigned length;
};

static bool find_route(unsigned node, struct route *path, bool visited[256]) {
    if (node < first_node || node >= first_node + node_count || path->length == 16 || visited[node])
        return false;

    visited[node] = true;
    unsigned caps = parameter(node, 9), type = (caps >> 20) & 15;
    if (caps == UINT32_MAX)
        return false;

    unsigned at = path->length++;
    path->nodes[at] = node;
    if (type == 0 && !(caps & (1 << 9))) {
        unsigned pcm = parameter(caps & (1 << 4) ? node : function_group, 0x0a);
        if (pcm != UINT32_MAX && (pcm & (1 << 6)) && (pcm & (1 << 17)))
            return true;
    } else if (type == 2 || type == 3 || type == 4) {
        uint8_t list[64];
        unsigned count = connections(node, list);
        if (legion_speakers && node == 0x17) {
            for (unsigned i = 0; i < count; i++)
                if (list[i] == 2 && find_route(list[i], path, visited)) {
                    path->choices[at] = i;
                    return true;
                }
            path->length--;
            return false;
        }
        for (unsigned i = 0; i < count; i++)
            if (find_route(list[i], path, visited)) {
                path->choices[at] = i;
                return true;
            }
    }

    path->length--;
    return false;
}

static void unmute(unsigned node, unsigned caps, unsigned selected) {
    if (caps & (1 << 2)) {
        unsigned amp = parameter(caps & (1 << 3) ? node : function_group, 0x12);
        if (amp != UINT32_MAX)
            verb(node, 0x300, 0xb000 | (amp & 0x7f));
    }
    if (caps & (1 << 1)) {
        unsigned amp = parameter(caps & (1 << 3) ? node : function_group, 0x0d);
        uint8_t list[64];
        unsigned count = connections(node, list);
        if (amp != UINT32_MAX)
            for (unsigned i = 0; i < count && i < 16; i++)
                verb(node, 0x300, 0x7000 | (i << 8) | (amp & 0x7f) | (i == selected ? 0 : 0x80));
    }
}

static unsigned configure_codec(void) {
    uint32_t root = parameter(0, 4);
    if (root == UINT32_MAX)
        return 0;

    unsigned start = (root >> 16) & 0xff, count = root & 0xff;
    for (unsigned group = start; group < start + count && group < 256; group++)
        if ((parameter(group, 5) & 0xff) == 1) {
            function_group = group;
            verb(group, 0x705, 0);
            delay(1000);

            uint32_t vendor = parameter(0, 0), subsystem = verb(group, 0xf20, 0);
            legion_speakers = vendor == 0x10ec0287 && subsystem == 0x17aa3907;
            if (legion_speakers)
                aw88399_match(vendor, subsystem);

            uint32_t nodes = parameter(group, 4);
            if (nodes == UINT32_MAX)
                continue;
            first_node = (nodes >> 16) & 0xff;
            node_count = nodes & 0xff;
            if (first_node + node_count > 256)
                continue;

            unsigned outputs = 0;
            for (unsigned node = first_node; node < first_node + node_count; node++) {
                unsigned caps = parameter(node, 9);
                if (caps == UINT32_MAX || ((caps >> 20) & 15) != 4 || (caps & (1 << 9)))
                    continue;
                unsigned pin = parameter(node, 0x0c), config = verb(node, 0xf1c, 0),
                         device = (config >> 20) & 15;
                if (pin == UINT32_MAX || config == UINT32_MAX || !(pin & (1 << 4)) ||
                    ((config >> 30) & 3) == 1 || (device != 0 && device != 1 && device != 2))
                    continue;

                struct route path = {0};
                bool visited[256] = {false};
                if (!find_route(node, &path, visited))
                    continue;
                for (unsigned i = 0; i < path.length; i++) {
                    unsigned widget = path.nodes[i], wcaps = parameter(widget, 9);
                    if (wcaps & (1 << 10))
                        verb(widget, 0x705, 0);
                    if (i + 1 < path.length)
                        verb(widget, 0x701, path.choices[i]);
                    unmute(widget, wcaps, path.choices[i]);
                }

                unsigned dac = path.nodes[path.length - 1];
                if (verb(dac, 0x200, 0x11) == UINT32_MAX || verb(dac, 0x706, 0x10) == UINT32_MAX)
                    continue;
                verb(node, 0x707, 0x40 | (device == 2 ? 0x80 : 0));
                if (pin & (1 << 16))
                    verb(node, 0x70c, 2);

                pr_info("HDA: codec=%u pin=0x%x DAC=0x%x device=%u\n", codec, node, dac, device);
                outputs++;
            }
            if (outputs)
                return outputs;
        }
    return 0;
}

static bool reset_stream(void) {
    w8(stream, r8(stream) & ~2U);
    running = false;
    if (!wait_bit(stream, 2, false, 10000))
        return false;
    w8(stream, 1);
    if (!wait_bit(stream, 1, true, 10000))
        return false;
    w8(stream, 0);
    if (!wait_bit(stream, 1, false, 10000))
        return false;
    queued = position = 0;
    memset(phys_ptr(samples_phys), 0, HDA_RING_BYTES);
    w8(stream + 3, 0x1c);
    w8(stream + 2, 0x10);
    w16(stream + 0x12, 0x11);
    w32(stream + 8, HDA_RING_BYTES);
    w16(stream + 0xc, HDA_DESCRIPTORS - 1);
    w32(stream + 0x18, table_phys);
    w32(stream + 0x1c, table_phys >> 32);
    return true;
}

static void use_legacy_streams(void) {
    /* Intel HDA extension headers link from LLCH (0x14). PP capability
     * ID 3 exposes PPCTL at +4: GPROCEN routes streams through the DSP.
     * This PCM driver uses the HDA link directly. Bound and cycle-check
     * the capability walk within the controller's mapped register space. */
    unsigned offset = r16(0x14), seen[16] = {0}, count = 0;
    while (offset && count < 16) {
        if (offset < 0x100 || offset > 16384 - 8 || (offset & 3))
            break;
        for (unsigned i = 0; i < count; i++)
            if (seen[i] == offset)
                return;
        seen[count++] = offset;
        uint32_t capability = r32(offset);
        if (capability == UINT32_MAX)
            break;
        if (((capability >> 16) & 0xfff) == 3)
            w32(offset + 4, r32(offset + 4) & ~0xc0000000U);
        offset = capability & 0xffff;
    }
}

bool hda_probe(void *header) {
    volatile pci_device_header_t *pci = header;
    uint32_t bar = pci->bar[0];
    if (registers || (bar & 1) || !(bar & ~15U))
        return false;

    uint64_t physical = bar & ~15U;
    if ((bar & 6) == 4)
        physical |= (uint64_t)pci->bar[1] << 32;

    /* Restore PCI D0 when firmware has left the function asleep. */
    bool seen[64] = {false};
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

    registers = vmem_map_mmio(physical, 16384);
    if (!registers)
        return false;

    *(volatile uint16_t *)&pci->command |= 6U | (1U << 10);
    w32(0x20, 0);
    w8(0x4c, 0);
    w8(0x5c, 0);

    w32(8, r32(8) & ~1U);
    if (!wait_bit(8, 1, false, 100000))
        goto failed;
    delay(1000);
    w32(8, r32(8) | 1U);
    if (!wait_bit(8, 1, true, 100000))
        goto failed;
    delay(1000);

    if (pci->vendor_id == 0x8086)
        use_legacy_streams();
    unsigned gcap = r16(0);
    if (!(gcap & 0xf000))
        goto failed;
    stream = 0x80 + ((gcap >> 8) & 15) * 0x20;

    command_phys = pmm_alloc_pages(1);
    response_phys = pmm_alloc_pages(1);
    table_phys = pmm_alloc_pages(1);
    samples_phys = pmm_alloc_pages(HDA_RING_BYTES / PAGE_SIZE);
    if (!command_phys || !response_phys || !table_phys || !samples_phys ||
        command_phys > UINT32_MAX - PAGE_SIZE || response_phys > UINT32_MAX - PAGE_SIZE ||
        table_phys > UINT32_MAX - PAGE_SIZE || samples_phys > UINT32_MAX - HDA_RING_BYTES)
        goto failed;

    commands = phys_ptr(command_phys);
    responses = phys_ptr(response_phys);
    memset((void *)commands, 0, PAGE_SIZE);
    memset((void *)responses, 0, PAGE_SIZE);

    unsigned size = ring_size(0x4e, &command_entries);
    if (!command_entries)
        goto failed;
    w8(0x4e, size);
    size = ring_size(0x5e, &response_entries);
    if (!response_entries)
        goto failed;
    w8(0x5e, size);

    w32(0x40, command_phys);
    w32(0x44, 0);
    w32(0x50, response_phys);
    w32(0x54, 0);
    w16(0x48, 0);
    w16(0x4a, 0x8000);
    delay(100);
    w16(0x4a, 0);
    w16(0x58, 0x8000);
    command_write = response_read = 0;
    w16(0x5a, 1);
    w8(0x5d, 5);
    w8(0x5c, 3);
    w8(0x4c, 2);

    unsigned present = r16(0x0e), outputs = 0;
    for (codec = 0; codec < 15; codec++)
        if (present & (1U << codec))
            if ((outputs = configure_codec()))
                break;
    if (!outputs)
        goto failed;

    struct hda_descriptor *table = phys_ptr(table_phys);
    memset(table, 0, PAGE_SIZE);
    for (unsigned i = 0; i < HDA_DESCRIPTORS; i++)
        table[i] = (struct hda_descriptor){samples_phys + i * HDA_BLOCK_BYTES, HDA_BLOCK_BYTES, 0};
    if (!reset_stream())
        goto failed;

    pr_info("Audio: HDA %04x:%04x, %u analog outputs, 48000-Hz stereo signed 16-bit PCM\n",
            pci->vendor_id, pci->device_id, outputs);
    return true;

failed:
    w8(0x4c, 0);
    w8(0x5c, 0);
    if (stream)
        w8(stream, 0);

    /* DMA must acknowledge stopping before its memory can be reused. A
     * broken controller may ignore RUN; keep its buffers reserved in that
     * case even after disabling PCI bus mastering. */
    bool stopped = wait_bit(0x4c, 2, false, 10000) && wait_bit(0x5c, 2, false, 10000) &&
                   (!stream || wait_bit(stream, 2, false, 10000));
    if (!stopped) {
        w32(8, r32(8) & ~1U);
        stopped = wait_bit(8, 1, false, 100000);
    }

    *(volatile uint16_t *)&pci->command &= ~4U;
    if (stopped) {
        if (command_phys)
            pmm_free_pages(command_phys, 1);
        if (response_phys)
            pmm_free_pages(response_phys, 1);
        if (table_phys)
            pmm_free_pages(table_phys, 1);
        if (samples_phys)
            pmm_free_pages(samples_phys, HDA_RING_BYTES / PAGE_SIZE);
    } else
        pr_info("Audio: HDA DMA did not stop; retaining its reserved buffers\n");
    pr_info("Audio: HDA %04x:%04x has no supported analog playback route\n", pci->vendor_id,
            pci->device_id);

    command_phys = response_phys = table_phys = samples_phys = 0;
    registers = NULL;
    stream = 0;
    return false;
}

static void reap(void) {
    if (!running)
        return;

    unsigned current = r32(stream + 4) % HDA_RING_BYTES;
    unsigned bytes = (current + HDA_RING_BYTES - position) % HDA_RING_BYTES, frames = bytes / 4;
    frames = frames > queued ? queued : frames;
    unsigned tail = HDA_RING_BYTES - position, clear = frames * 4;
    memset((char *)phys_ptr(samples_phys) + position, 0, clear < tail ? clear : tail);
    if (clear > tail)
        memset(phys_ptr(samples_phys), 0, clear - tail);

    position = current & ~3U;
    queued -= frames;
    played += frames;

    if (!queued && !starting_speakers) {
        w8(stream, r8(stream) & ~2U);
        running = false;
    }
    w8(stream + 3, 0x1c);
}

void hda_tick(void) {
    if (!registers)
        return;
    uint64_t flags = spin_lock_irqsave(&audio_lock);
    reap();
    spin_unlock_irqrestore(&audio_lock, flags);
}

long hda_call(struct task *task, unsigned operation, uint64_t pointer, uint64_t frames) {
    if (operation > OS_AUDIO_RESET || (operation != OS_AUDIO_WRITE && frames))
        return -E_INVAL;

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
        info.queued_frames = queued;
    } else if (owner && owner != task->tgid && (queued || starting_speakers))
        result = -E_BUSY;
    else if (operation == OS_AUDIO_RESET) {
        if (!reset_stream())
            result = -E_IO;
        owner = 0;
        played = 0;
    } else if (queued + frames > HDA_RING_BYTES / 4 - OS_AUDIO_MAX_FRAMES)
        result = -E_AGAIN;
    else {
        if (!running) {
            if (!reset_stream())
                result = -E_IO;
            if (owner != task->tgid)
                played = 0;
            owner = task->tgid;
        }
        if (!result) {
            unsigned offset = (position + queued * 4) % HDA_RING_BYTES, bytes = frames * 4,
                     tail = HDA_RING_BYTES - offset;
            memcpy((char *)phys_ptr(samples_phys) + offset, samples, bytes < tail ? bytes : tail);
            if (bytes > tail)
                memcpy(phys_ptr(samples_phys), (char *)samples + tail, bytes - tail);
            __asm__ volatile("mfence" ::: "memory");
            queued += frames;
            w8(stream, r8(stream) | 2U);
            running = true;
            result = frames;
            if (legion_speakers)
                starting_speakers = true;
        }
    }
    spin_unlock_irqrestore(&audio_lock, flags);
    if (operation == OS_AUDIO_RESET && !result)
        aw88399_stop();
    if (operation == OS_AUDIO_WRITE && result > 0) {
        bool speakers = aw88399_start();
        flags = spin_lock_irqsave(&audio_lock);
        starting_speakers = false;
        reap();
        spin_unlock_irqrestore(&audio_lock, flags);
        if (!speakers)
            return -E_IO;
    }
    if (operation == OS_AUDIO_INFO && !user_copy_out(&task->space, pointer, &info, sizeof(info)))
        return -E_FAULT;
    return result;
}
