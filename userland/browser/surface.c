#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Upstream internal headers require these public types in this order. */
// clang-format off
#include "browser_protocol.h"
#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"
#include "nsfb.h"
#include "plot.h"
#include "surface.h"

// clang-format on

static nsfb_bbox_t dirty;
static int have_dirty;
static struct browser_packet incoming;
static size_t received, text_received;
static char text[BROWSER_MAX_TEXT + 1];
static int last_buttons;
static nsfb_event_t pending[32];
static unsigned pending_count, pending_position;
extern void modernos_browser_action(unsigned action, const char *text);

static int write_all(const void *data, size_t count) {
    const unsigned char *at = data;
    while (count) {
        ssize_t n = write(1, at, count);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        at += n;
        count -= n;
    }

    return 0;
}

void modernos_browser_text(unsigned type, const char *value) {
    size_t count = strlen(value);
    if (count > BROWSER_MAX_TEXT) {
        count = BROWSER_MAX_TEXT;
    }
    struct browser_packet packet = {.magic = BROWSER_MAGIC, .type = type, .bytes = count};
    if (write_all(&packet, sizeof(packet)) || write_all(value, count)) {
        _exit(1);
    }
}

void modernos_browser_state(unsigned flags) {
    static unsigned previous = UINT32_MAX;
    if (flags == previous) {
        return;
    }
    struct browser_packet packet = {.magic = BROWSER_MAGIC, .type = BROWSER_STATE, .flags = flags};
    if (write_all(&packet, sizeof(packet))) {
        _exit(1);
    }
    previous = flags;
}

static int defaults(nsfb_t *nsfb) {
    nsfb->width = 800;
    nsfb->height = 520;
    nsfb->format = NSFB_FMT_XRGB8888;
    return select_plotters(nsfb) ? 0 : -1;
}

static int geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format) {
    (void)format;
    if (width <= 0) {
        width = nsfb->width;
    }
    if (height <= 0) {
        height = nsfb->height;
    }
    if (width < 300 || height < 200 || width > 4096 || height > 4096 ||
        (uint64_t)width * height > BROWSER_MAX_PIXELS) {
        return -1;
    }
    if (nsfb->ptr) {
        void *pixels = malloc((size_t)width * height * 4);
        if (!pixels) {
            return -1;
        }
        memset(pixels, 0xff, (size_t)width * height * 4);
        free(nsfb->ptr);
        nsfb->ptr = pixels;
    }

    nsfb->width = width;
    nsfb->height = height;
    nsfb->format = NSFB_FMT_XRGB8888;
    nsfb->linelen = width * 4;
    select_plotters(nsfb);
    dirty = (nsfb_bbox_t){0, 0, width, height};
    have_dirty = 1;
    return 0;
}

static int initialise(nsfb_t *nsfb) {
    nsfb->ptr = calloc((size_t)nsfb->width * nsfb->height, 4);
    nsfb->linelen = nsfb->width * 4;
    return nsfb->ptr ? 0 : -1;
}

static int finalise(nsfb_t *nsfb) {
    free(nsfb->ptr);
    nsfb->ptr = NULL;
    return 0;
}

static int update(nsfb_t *nsfb, nsfb_bbox_t *box) {
    nsfb_bbox_t b = box ? *box : (nsfb_bbox_t){0, 0, nsfb->width, nsfb->height};
    if (b.x0 < 0) {
        b.x0 = 0;
    }
    if (b.y0 < 0) {
        b.y0 = 0;
    }
    if (b.x1 > nsfb->width) {
        b.x1 = nsfb->width;
    }
    if (b.y1 > nsfb->height) {
        b.y1 = nsfb->height;
    }
    if (b.x1 <= b.x0 || b.y1 <= b.y0) {
        return 0;
    }
    if (!have_dirty) {
        dirty = b;
    } else {
        if (b.x0 < dirty.x0) {
            dirty.x0 = b.x0;
        }
        if (b.y0 < dirty.y0) {
            dirty.y0 = b.y0;
        }
        if (b.x1 > dirty.x1) {
            dirty.x1 = b.x1;
        }
        if (b.y1 > dirty.y1) {
            dirty.y1 = b.y1;
        }
    }

    have_dirty = 1;
    return 0;
}

static void flush(nsfb_t *nsfb) {
    if (!have_dirty) {
        return;
    }
    int width = dirty.x1 - dirty.x0, height = dirty.y1 - dirty.y0;
    struct browser_packet packet = {.magic = BROWSER_MAGIC,
                                    .type = BROWSER_FRAME,
                                    .bytes = (uint32_t)width * height * 4,
                                    .x = dirty.x0,
                                    .y = dirty.y0,
                                    .width = width,
                                    .height = height,
                                    .canvas_width = nsfb->width,
                                    .canvas_height = nsfb->height};
    if (write_all(&packet, sizeof(packet))) {
        _exit(1);
    }
    for (int y = dirty.y0; y < dirty.y1; y++) {
        if (write_all(nsfb->ptr + (size_t)y * nsfb->linelen + dirty.x0 * 4, (size_t)width * 4)) {
            _exit(1);
        }
    }
    have_dirty = 0;
}

static void queue(enum nsfb_event_type_e type, int value) {
    if (pending_count >= sizeof(pending) / sizeof(pending[0])) {
        return;
    }
    pending[pending_count++] = (nsfb_event_t){.type = type, .value.keycode = value};
}

static bool input(nsfb_t *nsfb, nsfb_event_t *event, int timeout) {
    if (pending_position < pending_count) {
        *event = pending[pending_position++];
        return true;
    }
    pending_position = pending_count = 0;
    /* Poll frequently enough for NetSurf's HTTP scheduler to progress. */
    if (timeout < 0 || timeout > 10) {
        timeout = 10;
    }
    struct pollfd descriptor = {0, POLLIN, 0};
    /* Consume queued input before another large blocking frame. In
       particular, Quit must reach the frontend so it can save cookies. */
    if (poll(&descriptor, 1, 0) <= 0) {
        flush(nsfb);
        if (poll(&descriptor, 1, timeout) <= 0)
            return false;
    }
    if (descriptor.revents & (POLLHUP | POLLERR)) {
        *event = (nsfb_event_t){.type = NSFB_EVENT_CONTROL, .value.controlcode = NSFB_CONTROL_QUIT};
        return true;
    }
    while (received < sizeof(incoming)) {
        ssize_t n = read(0, (char *)&incoming + received, sizeof(incoming) - received);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            return false;
        }
        if (n <= 0) {
            _exit(1);
        }
        received += n;
    }

    if (incoming.magic != BROWSER_MAGIC || incoming.bytes > BROWSER_MAX_TEXT) {
        _exit(1);
    }
    while (text_received < incoming.bytes) {
        ssize_t n = read(0, text + text_received, incoming.bytes - text_received);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            return false;
        }
        if (n <= 0) {
            _exit(1);
        }
        text_received += n;
    }

    text[text_received] = 0;
    received = text_received = 0;
    switch (incoming.type) {
    case BROWSER_QUIT:
        *event = (nsfb_event_t){.type = NSFB_EVENT_CONTROL, .value.controlcode = NSFB_CONTROL_QUIT};
        return true;
    case BROWSER_RESIZE:
        if (incoming.width < 300 || incoming.height < 200 || incoming.width > 4096 ||
            incoming.height > 4096 ||
            (uint64_t)incoming.width * incoming.height > BROWSER_MAX_PIXELS) {
            _exit(1);
        }
        *event = (nsfb_event_t){.type = NSFB_EVENT_RESIZE,
                                .value.resize = {incoming.width, incoming.height}};
        return true;
    case BROWSER_KEY:
        if (incoming.x < 0 || (incoming.x > 512 && (incoming.x < BROWSER_TEXT_KEY_BASE + 32 ||
                                                    incoming.x > BROWSER_TEXT_KEY_BASE + 126))) {
            return false;
        }
        *event = (nsfb_event_t){.type = incoming.flags ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP,
                                .value.keycode = incoming.x};
        return true;
    case BROWSER_POINTER: {
        unsigned changed = last_buttons ^ incoming.flags;
        const int buttons[] = {NSFB_KEY_MOUSE_1, NSFB_KEY_MOUSE_2, NSFB_KEY_MOUSE_3};
        for (unsigned i = 0; i < 3; i++) {
            if (changed & (1u << i)) {
                queue((incoming.flags & (1u << i)) ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP,
                      buttons[i]);
            }
        }

        last_buttons = incoming.flags & 7;
        int wheel = incoming.width;
        if (wheel > 8) {
            wheel = 8;
        }
        if (wheel < -8) {
            wheel = -8;
        }
        for (int i = 0; i < (wheel < 0 ? -wheel : wheel); i++) {
            int button = wheel > 0 ? NSFB_KEY_MOUSE_4 : NSFB_KEY_MOUSE_5;
            queue(NSFB_EVENT_KEY_DOWN, button);
            queue(NSFB_EVENT_KEY_UP, button);
        }
        *event = (nsfb_event_t){.type = NSFB_EVENT_MOVE_ABSOLUTE,
                                .value.vector = {incoming.x, incoming.y, 0}};
        return true;
    }
    case BROWSER_NAVIGATE:
    case BROWSER_BACK:
    case BROWSER_FORWARD:
    case BROWSER_RELOAD:
    case BROWSER_STOP:
        modernos_browser_action(incoming.type, text);
        return false;
    default:
        return false;
    }
}

static int cursor(nsfb_t *nsfb, struct nsfb_cursor_s *pointer) {
    (void)nsfb;
    (void)pointer;
    return 0;
}

const nsfb_surface_rtns_t modernos_routines = {.defaults = defaults,
                                               .initialise = initialise,
                                               .finalise = finalise,
                                               .geometry = geometry,
                                               .update = update,
                                               .input = input,
                                               .cursor = cursor};
NSFB_SURFACE_DEF(modernos, NSFB_SURFACE_MODERNOS, &modernos_routines)
