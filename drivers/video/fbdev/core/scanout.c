#include <mm/user.h>
#include <mm/vmem_io.h>
#include <printk.h>
#include <string.h>
#include <video/fbcon.h>
#include <video/scanout.h>

/* QEMU standard VGA's documented flat Bochs DISPI MMIO interface.
   We preserve the firmware mode and only change the vertical viewport.
   Unsupported GPUs keep the firmware framebuffer backend. */
static struct boot_framebuffer *scanout_fb;
static volatile uint16_t *dispi;
static uint8_t *video;
static bool wc, synchronized;
static unsigned front;
static struct os_damage_rect previous[OS_DISPLAY_MAX_RECTS];
static unsigned previous_count;

void scanout_framebuffer(struct boot_framebuffer *fb) {
    scanout_fb = fb;
    uint64_t physical;
    if (!fb || !vmem_translate((uintptr_t)fb->address, &physical) ||
        phys_ptr(physical) != fb->address) {
        return;
    }
    /* Verify that the only video alias is the ordinary shared HHDM mapping. */
    bool candidate_wc = false;
    void *mapped = vmem_map_video(physical, fb->pitch * fb->height, &candidate_wc);
    if (mapped != fb->address) {
        return;
    }
    wc = candidate_wc;
}

void scanout_probe(pci_device_header_t *device) {
    if (dispi || !scanout_fb || device->vendor_id != 0x1234 || device->device_id != 0x1111 ||
        (device->header_type & 127) || !(device->command & 2) || (device->bar[0] & 15) != 8 ||
        (device->bar[2] & 15) != 0) {
        return;
    }
    uint64_t physical;
    if (!vmem_translate((uintptr_t)scanout_fb->address, &physical) ||
        physical != (device->bar[0] & ~15u)) {
        return;
    }
    uint64_t control = device->bar[2] & ~15u;
    if (!control) {
        return;
    }
    uint8_t *registers = vmem_map_mmio(control, 4096);
    if (!registers) {
        return;
    }
    volatile uint16_t *regs = (volatile uint16_t *)(registers + 0x500);
    uint16_t id = regs[0];
    uint64_t capacity = (uint64_t)regs[10] * 65536;
    uint64_t needed = scanout_fb->pitch * scanout_fb->height * 2;
    if (id < 0xb0c4 || id > 0xb0c5 || !(regs[4] & 1) || regs[1] != scanout_fb->width ||
        regs[2] != scanout_fb->height || regs[3] != 32 || regs[8] || regs[9] ||
        regs[6] * 4u != scanout_fb->pitch || regs[7] < scanout_fb->height * 2 ||
        capacity < needed || capacity > 256ULL * 1024 * 1024) {
        return;
    }
    bool candidate_wc = false;
    uint8_t *mapped = vmem_map_video(physical, needed, &candidate_wc);
    if (mapped != scanout_fb->address) {
        return;
    }
    wc = candidate_wc;
    video = mapped;
    dispi = regs;
    pr_info("Display: QEMU VBE buffered scanout, 2 pages, %s video mapping\n", wc ? "WC" : "UC");
}

void scanout_caps(struct os_display_caps *out) {
    *out = (struct os_display_caps){
        .flags = (dispi ? DISPLAY_CAP_PAGE_FLIP : 0) | (wc ? DISPLAY_CAP_WRITE_COMBINING : 0) |
                 (vmem_video_large_pages() ? DISPLAY_CAP_LARGE_PAGES : 0),
        .backend = dispi ? DISPLAY_BACKEND_QEMU_VBE : DISPLAY_BACKEND_LINEAR,
        .buffers = dispi ? 2 : 1};
}

bool scanout_streaming_copy(void) {
    return wc;
}

void scanout_reset(void) {
    if (dispi) {
        __asm__ volatile("sfence" ::: "memory");
        dispi[9] = 0;
    }
    front = 0;
    synchronized = false;
    previous_count = 0;
}

static struct os_damage_rect bounds(struct os_damage_rect a, struct os_damage_rect b) {
    unsigned x = min(a.x, b.x), y = min(a.y, b.y);
    return (struct os_damage_rect){x, y, max(a.x + a.width, b.x + b.width) - x,
                                   max(a.y + a.height, b.y + b.height) - y};
}

static uint64_t area(struct os_damage_rect r) {
    return (uint64_t)r.width * r.height;
}

static void repair_damage(struct os_display_batch *out, const struct os_display_batch *batch) {
    struct os_damage_rect regions[OS_DISPLAY_MAX_RECTS * 2];
    unsigned count = 0;
    for (unsigned step = 0; step < previous_count + batch->count; step++) {
        struct os_damage_rect r =
            step < previous_count ? previous[step] : batch->rects[step - previous_count];
        /* Collinear/tight overlaps merge without turning touching shadow
           strips into a huge bounding rectangle over unchanged pixels. */
        for (unsigned i = 0; i < count; i++) {
            struct os_damage_rect joined = bounds(regions[i], r);
            if (area(joined) <= area(regions[i]) + area(r)) {
                r = joined;
                regions[i] = regions[--count];
                i = (unsigned)-1;
            }
        }
        regions[count++] = r;
    }
    while (count > OS_DISPLAY_MAX_RECTS) {
        unsigned best_a = 0, best_b = 1;
        int64_t best = INT64_MAX;
        struct os_damage_rect combined = {0};
        for (unsigned a = 0; a < count; a++) {
            for (unsigned b = a + 1; b < count; b++) {
                struct os_damage_rect joined = bounds(regions[a], regions[b]);
                int64_t cost =
                    (int64_t)area(joined) - (int64_t)area(regions[a]) - (int64_t)area(regions[b]);
                if (cost < best) {
                    best = cost;
                    best_a = a;
                    best_b = b;
                    combined = joined;
                }
            }
        }
        regions[best_a] = combined;
        regions[best_b] = regions[--count];
    }
    out->count = count;
    memcpy(out->rects, regions, count * sizeof(regions[0]));
}

bool scanout_present(const struct address_space *space, const struct os_display_batch *batch) {
    if (!dispi) {
        return fbcon_present_batch(space, batch);
    }
    struct os_display_batch upload = {.pixels = batch->pixels, .stride = batch->stride};
    if (!synchronized) {
        upload.count = 1;
        upload.rects[0] = (struct os_damage_rect){0, 0, scanout_fb->width, scanout_fb->height};
    } else {
        /* The hidden page is one frame older. Repair its stale regions from
           the newly validated complete canvas, merged with this frame's damage. */
        repair_damage(&upload, batch);
    }
    unsigned next = front ^ 1;
    void *target = video + next * scanout_fb->pitch * scanout_fb->height;
    if (!fbcon_upload_batch(space, &upload, target)) {
        synchronized = false;
        return false;
    }
    __asm__ volatile("sfence" ::: "memory");
    dispi[9] = next * scanout_fb->height;
    if (dispi[9] != next * scanout_fb->height) {
        scanout_reset();
        return false;
    }
    front = next;
    synchronized = true;
    previous_count = batch->count;
    memcpy(previous, batch->rects, batch->count * sizeof(previous[0]));
    return true;
}
