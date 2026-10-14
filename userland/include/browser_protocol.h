#ifndef MODERNOS_BROWSER_PROTOCOL_H
#define MODERNOS_BROWSER_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>

/* Private, versioned pipe protocol. The desktop owns all window decorations,
 * stacking and workspaces. The engine can submit bounded client pixels only. */
#define BROWSER_MAGIC 0x3142574dU
#define BROWSER_MAX_PIXELS (16U * 1024U * 1024U)
#define BROWSER_MAX_TEXT 4096U
#define BROWSER_TEXT_KEY_BASE 0x1000

enum {
    BROWSER_FRAME = 1,
    BROWSER_TITLE,
    BROWSER_URL,
    BROWSER_NOTICE,
    BROWSER_STATUS,
    BROWSER_STATE,
    BROWSER_COPY,
    BROWSER_PRESENT,
    BROWSER_CANCEL_PAINT,
    BROWSER_KEY = 16,
    BROWSER_POINTER,
    BROWSER_RESIZE,
    BROWSER_QUIT,
    BROWSER_NAVIGATE,
    BROWSER_BACK,
    BROWSER_FORWARD,
    BROWSER_RELOAD,
    BROWSER_STOP,
    /* Desktop-to-broker geometry: width/height are the display, flags is its
     * available height, x/y are the outer window position, and canvas_width/
     * canvas_height are its outer dimensions. No payload or capability. */
    BROWSER_SCREEN
};

enum { BROWSER_CAN_BACK = 1, BROWSER_CAN_FORWARD = 2, BROWSER_LOADING = 4 };
/* Renderer-to-broker navigation metadata only; never desktop state flags. */
enum { BROWSER_HISTORY_TRAVERSE = 8U, BROWSER_HISTORY_REPLACE = 16U };
/* Trusted broker-to-renderer NAVIGATE only: the desktop requested this URL.
 * Renderer-sourced navigation packets cannot claim this provenance. */
enum { BROWSER_NAVIGATION_FROM_CLIENT = 32U };
struct browser_packet {
    uint32_t magic, type, bytes;
    int32_t x, y, width, height;
    uint32_t canvas_width, canvas_height, flags;
};

/* For desktop-to-broker POINTER only, canvas_width/canvas_height carry the
 * actual pointer's screen coordinates. They do not identify a paint canvas. */
/* Copy pixels inside this window's retained canvas. The payload contains the
 * source corner; the packet rectangle is the destination. Both are bounded
 * before touching pixels. This never names another window or a guest pointer. */
struct browser_copy_source {
    int32_t x, y;
};

static inline int browser_surface_rect_valid(const struct browser_packet *p) {
    return p->canvas_width >= 300 && p->canvas_width <= 4096 && p->canvas_height >= 200 &&
           p->canvas_height <= 4096 &&
           (uint64_t)p->canvas_width * p->canvas_height <= BROWSER_MAX_PIXELS && p->x >= 0 &&
           p->y >= 0 && p->width > 0 && p->height > 0 &&
           (uint64_t)p->x + p->width <= p->canvas_width &&
           (uint64_t)p->y + p->height <= p->canvas_height;
}

static inline int browser_surface_copy_valid(const struct browser_packet *p,
                                             const struct browser_copy_source *source) {
    return p->magic == BROWSER_MAGIC && p->type == BROWSER_COPY && p->bytes == sizeof(*source) &&
           !p->flags && browser_surface_rect_valid(p) && source->x >= 0 && source->y >= 0 &&
           (uint64_t)source->x + p->width <= p->canvas_width &&
           (uint64_t)source->y + p->height <= p->canvas_height;
}

static inline int browser_surface_copy(uint32_t *canvas, unsigned width, unsigned height,
                                       const struct browser_packet *p,
                                       const struct browser_copy_source *source) {
    if (!canvas || width != p->canvas_width || height != p->canvas_height ||
        !browser_surface_copy_valid(p, source))
        return 0;
    /* Reverse rows when the destination is below the source. memmove also
     * handles horizontal overlap within each row. */
    for (int row = 0; row < p->height; ++row) {
        int y = p->y > source->y ? p->height - row - 1 : row;
        __builtin_memmove(canvas + (uint64_t)(p->y + y) * width + p->x,
                          canvas + (uint64_t)(source->y + y) * width + source->x,
                          (uint64_t)p->width * sizeof(*canvas));
    }

    return 1;
}

static inline int browser_surface_present_valid(const struct browser_packet *p) {
    return p->magic == BROWSER_MAGIC && p->type == BROWSER_PRESENT && !p->bytes && !p->flags &&
           !p->x && !p->y && !p->width && !p->height && p->canvas_width >= 300 &&
           p->canvas_width <= 4096 && p->canvas_height >= 200 && p->canvas_height <= 4096 &&
           (uint64_t)p->canvas_width * p->canvas_height <= BROWSER_MAX_PIXELS;
}

static inline int browser_surface_cancel_valid(const struct browser_packet *p) {
    return p->magic == BROWSER_MAGIC && p->type == BROWSER_CANCEL_PAINT && !p->bytes && !p->flags &&
           !p->x && !p->y && !p->width && !p->height && !p->canvas_width && !p->canvas_height;
}

/* Native paints are committed on the desktop thread after PRESENT. Holding
 * bounded packets keeps the displayed canvas intact during pipe backpressure
 * without copying the entire retained canvas into a second backing store. */
struct browser_paint_packet {
    struct browser_paint_packet *next;
    struct browser_packet header;
    size_t allocation_bytes;
    unsigned char data[];
};

struct browser_paint {
    struct browser_paint_packet *first, *last;
    size_t bytes;
    unsigned count, width, height;
};

#define BROWSER_PAINT_MAX_BYTES ((size_t)BROWSER_MAX_PIXELS * 4 + 65536)
#define BROWSER_PAINT_MAX_PACKETS 256U

static inline int browser_surface_paint_queue(struct browser_paint *paint, unsigned width,
                                              unsigned height, int initialized,
                                              const struct browser_packet *p, const void *payload,
                                              void *(*alloc)(size_t)) {
    if (!payload || p->magic != BROWSER_MAGIC || p->flags || !browser_surface_rect_valid(p))
        return 0;
    if (paint->count && (paint->width != p->canvas_width || paint->height != p->canvas_height))
        return 0;
    if (p->type == BROWSER_COPY) {
        if (p->bytes != sizeof(struct browser_copy_source))
            return 0;
        struct browser_copy_source source;
        __builtin_memcpy(&source, payload, sizeof(source));
        if (!initialized || width != p->canvas_width || height != p->canvas_height ||
            !browser_surface_copy_valid(p, &source))
            return 0;
    } else if (p->type == BROWSER_FRAME) {
        if ((uint64_t)p->width * p->height * 4 != p->bytes)
            return 0;
        if (!paint->count &&
            (!initialized || width != p->canvas_width || height != p->canvas_height) &&
            (p->x || p->y || p->width != (int)p->canvas_width ||
             p->height != (int)p->canvas_height))
            return 0;
    } else
        return 0;
    size_t bytes = sizeof(struct browser_paint_packet) + p->bytes;
    if (paint->count >= BROWSER_PAINT_MAX_PACKETS || paint->bytes > BROWSER_PAINT_MAX_BYTES ||
        bytes > BROWSER_PAINT_MAX_BYTES - paint->bytes)
        return 0;
    struct browser_paint_packet *item = (struct browser_paint_packet *)alloc(bytes);
    if (!item)
        return 0;
    item->next = NULL;
    item->header = *p;
    item->allocation_bytes = bytes;
    __builtin_memcpy(item->data, payload, p->bytes);
    if (paint->last)
        paint->last->next = item;
    else
        paint->first = item;
    paint->last = item;
    paint->width = p->canvas_width;
    paint->height = p->canvas_height;
    paint->bytes += bytes;
    ++paint->count;
    return 1;
}

static inline int browser_surface_paint_ready(const struct browser_paint *paint,
                                              const struct browser_packet *p) {
    return browser_surface_present_valid(p) && paint->count && paint->width == p->canvas_width &&
           paint->height == p->canvas_height;
}

static inline int browser_surface_paint_apply(uint32_t *canvas, unsigned width, unsigned height,
                                              const struct browser_paint *paint,
                                              const struct browser_packet *present) {
    if (!canvas || width != paint->width || height != paint->height ||
        !browser_surface_paint_ready(paint, present))
        return 0;
    for (const struct browser_paint_packet *item = paint->first; item; item = item->next) {
        const struct browser_packet *p = &item->header;
        if (p->type == BROWSER_COPY) {
            struct browser_copy_source source;
            __builtin_memcpy(&source, item->data, sizeof(source));
            if (!browser_surface_copy(canvas, width, height, p, &source))
                return 0;
        } else
            for (int y = 0; y < p->height; ++y)
                __builtin_memcpy(canvas + (uint64_t)(p->y + y) * width + p->x,
                                 item->data + (uint64_t)y * p->width * 4, (size_t)p->width * 4);
    }

    return 1;
}

static inline void browser_surface_paint_cancel(struct browser_paint *paint,
                                                void (*release)(void *, size_t)) {
    struct browser_paint_packet *item = paint->first;
    while (item) {
        struct browser_paint_packet *next = item->next;
        release(item, item->allocation_bytes);
        item = next;
    }
    paint->first = NULL;
    paint->last = NULL;
    paint->bytes = 0;
    paint->count = 0;
    paint->width = 0;
    paint->height = 0;
}

/* Key codes follow libnsfb's stable ASCII/SDL-style enumeration. Printable
 * input uses TEXT_KEY_BASE + the actual ASCII character after the OS keymap,
 * keeping Caps Lock and punctuation independent of libnsfb's legacy keymap.
 * Pointer
 * flags encode the current left/middle/right state; y/x are client pixels. */
#endif
