#include <video/display.h>
#include <video/fbcon.h>
#include <video/scanout.h>
#include <sched.h>
#include <tty.h>
#include <mm/pmm.h>
#include <string.h>

static mutex_t display_mutex;
static spinlock_t event_lock;
static unsigned owner, head, tail, dropped;
static struct {
    unsigned owner;
    uint64_t root, generation, pixels, stride;
} canvas;

static uint64_t sequence;
static struct os_input_event events[256];

static void publish(struct os_input_event event) {
    uint64_t f = spin_lock_irqsave(&event_lock);
    if (owner) {
        unsigned next = (head + 1) % 256;
        if (next == tail) {
            tail = (tail + 1) % 256;
            dropped++;
        }
        event.sequence = ++sequence;
        events[head] = event;
        head = next;
    }
    spin_unlock_irqrestore(&event_lock, f);
}

void display_key(uint8_t key) {
    publish((struct os_input_event){.type = INPUT_KEY, .code = key});
}

bool display_hid(uint8_t usage, uint8_t ascii, uint8_t modifiers, bool pressed) {
    uint64_t f = spin_lock_irqsave(&event_lock);
    bool active = owner != 0;
    spin_unlock_irqrestore(&event_lock, f);
    if (active)
        publish((struct os_input_event){.type = INPUT_HID_KEY,
                                        .code = usage,
                                        .dx = ascii,
                                        .dy = modifiers,
                                        .buttons = pressed});
    return active;
}

void display_pointer(const mouse_event_t *event) {
    publish((struct os_input_event){.type = INPUT_POINTER,
                                    .code = event->flags,
                                    .dx = event->dx,
                                    .dy = event->dy,
                                    .wheel = event->wheel,
                                    .buttons = event->buttons});
}

void display_drop(unsigned pid) {
    mutex_lock(&display_mutex);
    uint64_t f = spin_lock_irqsave(&event_lock);
    bool release = owner == pid;
    if (release) {
        owner = 0;
        head = tail = dropped = 0;
        canvas.owner = 0;
    }
    spin_unlock_irqrestore(&event_lock, f);
    if (release) {
        fbcon_graphics(false);
        /* A display owner can exit or fault without restoring raw console
           input. Recover here, while its foreground ownership is still held. */
        tty_reset_config(pid);
    }
    mutex_unlock(&display_mutex);
}

long display_call(struct task *t, unsigned op, uint64_t pointer, uint64_t size) {
    struct os_display_info info;
    if (!fbcon_display_info(&info))
        return -E_NOTSUP;

    if (op == DISPLAY_INFO) {
        if (size != sizeof(info))
            return -E_INVAL;
        return user_copy_out(&t->space, pointer, &info, sizeof(info)) ? 0 : -E_FAULT;
    }

    if (op == DISPLAY_CAPS) {
        struct os_display_caps caps;
        scanout_caps(&caps);
        if (size != sizeof(caps))
            return -E_INVAL;
        return user_copy_out(&t->space, pointer, &caps, sizeof(caps)) ? 0 : -E_FAULT;
    }

    if (op > DISPLAY_PRESENT_SWAP)
        return -E_INVAL;
    if (!tty_is_foreground(t->pid))
        return -E_PERM;

    if (op == DISPLAY_RELEASE) {
        if (pointer || size)
            return -E_INVAL;
        mutex_lock(&display_mutex);
        bool owns = owner == t->pid;
        mutex_unlock(&display_mutex);
        if (!owns)
            return -E_PERM;
        display_drop(t->pid);
        return 0;
    }

    mutex_lock(&display_mutex);
    long result = 0;
    if (op == DISPLAY_ACQUIRE) {
        if (pointer || size)
            result = -E_INVAL;
        else if (owner)
            result = -E_BUSY;
        else {
            uint64_t f = spin_lock_irqsave(&event_lock);
            owner = t->pid;
            head = tail = dropped = 0;
            spin_unlock_irqrestore(&event_lock, f);
            fbcon_graphics(true);
        }
    } else if (owner != t->pid)
        result = -E_PERM;
    else if (op == DISPLAY_PRESENT_BATCH || op == DISPLAY_PRESENT_SWAP) {
        struct os_display_batch batch = {0};
        if (size != sizeof(batch) && size != OS_DISPLAY_LEGACY_BATCH_SIZE)
            result = -E_INVAL;
        else if (!user_copy_in(&t->space, &batch, pointer, size))
            result = -E_FAULT;
        else if (!batch.count || batch.count > (size - 16) / sizeof(struct os_damage_rect) ||
                 batch.stride < info.width || batch.stride > 8192)
            result = -E_INVAL;
        else {
            /* Validate the entire batch before any framebuffer writes. Each
               rectangle may reference a stride, but no padding is exposed. */
            uint64_t total = 0;
            bool complete = canvas.owner == t->pid && canvas.root == t->space.root &&
                            canvas.generation == t->space.vm->generation &&
                            canvas.pixels == batch.pixels && canvas.stride == batch.stride;
            if (op == DISPLAY_PRESENT_SWAP && !complete) {
                complete = user_access(
                    &t->space, batch.pixels,
                    ((uint64_t)(info.height - 1) * batch.stride + info.width) * 4, false);
                if (!complete)
                    result = -E_FAULT;
                else {
                    canvas.owner = t->pid;
                    canvas.root = t->space.root;
                    canvas.generation = t->space.vm->generation;
                    canvas.pixels = batch.pixels;
                    canvas.stride = batch.stride;
                }
            }

            for (unsigned i = 0; i < batch.count && !result; i++) {
                const struct os_damage_rect *r = &batch.rects[i];
                if (!r->width || !r->height || r->x >= info.width || r->y >= info.height ||
                    r->width > info.width - r->x || r->height > info.height - r->y)
                    result = -E_INVAL;
                else {
                    total += (uint64_t)r->width * r->height;
                    uint64_t offset = ((uint64_t)r->y * batch.stride + r->x) * 4;
                    uint64_t bytes = ((uint64_t)(r->height - 1) * batch.stride + r->width) * 4;
                    if (total > 2ULL * info.width * info.height ||
                        batch.pixels > UINT64_MAX - offset ||
                        (!complete && !user_access(&t->space, batch.pixels + offset, bytes, false)))
                        result = -E_FAULT;
                }
            }

            /* Buffered presentation may repair stale damage from a previous
               frame. Require the complete canvas, including strided rows. */
            if (!result && !(op == DISPLAY_PRESENT_SWAP ? scanout_present(&t->space, &batch)
                                                        : fbcon_present_batch(&t->space, &batch)))
                result = -E_FAULT;
        }
    } else {
        struct os_display_rect rect;
        if (size != sizeof(rect))
            result = -E_INVAL;
        else if (!user_copy_in(&t->space, &rect, pointer, sizeof(rect)))
            result = -E_FAULT;
        else if (!rect.width || !rect.height || rect.x >= info.width || rect.y >= info.height ||
                 rect.width > info.width - rect.x || rect.height > info.height - rect.y ||
                 (uint64_t)rect.width * rect.height > info.max_pixels)
            result = -E_INVAL;
        else {
            size_t bytes = (size_t)rect.width * rect.height * 4;
            if (!user_access(&t->space, rect.pixels, bytes, false))
                result = -E_FAULT;
            else {
                uint32_t *pixels = kmalloc(bytes);
                if (!pixels)
                    result = -E_NOMEM;
                else {
                    if (!user_copy_in(&t->space, pixels, rect.pixels, bytes))
                        result = -E_FAULT;
                    else
                        fbcon_present(&rect, pixels);
                    kfree(pixels);
                }
            }
        }
    }

    mutex_unlock(&display_mutex);
    return result;
}

long display_input(struct task *t, uint64_t pointer, uint64_t capacity) {
    if (!capacity || capacity > 64)
        return -E_INVAL;
    if (!user_access(&t->space, pointer, capacity * sizeof(struct os_input_event), true))
        return -E_FAULT;
    struct os_input_event buffer[64];
    unsigned n = 0;
    uint64_t f = spin_lock_irqsave(&event_lock);
    if (owner != t->pid || !tty_is_foreground(t->pid)) {
        spin_unlock_irqrestore(&event_lock, f);
        return -E_PERM;
    }
    if (dropped) {
        buffer[n++] = (struct os_input_event){
            .sequence = ++sequence, .type = INPUT_OVERFLOW, .code = dropped};
        dropped = 0;
    }
    while (tail != head && n < capacity) {
        buffer[n++] = events[tail];
        tail = (tail + 1) % 256;
    }
    spin_unlock_irqrestore(&event_lock, f);
    if (!n)
        return -E_AGAIN;
    return user_copy_out(&t->space, pointer, buffer, n * sizeof(buffer[0])) ? (long)n : -E_FAULT;
}
