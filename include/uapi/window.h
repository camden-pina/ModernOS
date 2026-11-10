#ifndef COAST_WINDOW_ABI_H
#define COAST_WINDOW_ABI_H
#include <stdint.h>
#include <uapi/keyboard.h>
/* v1 is a copied software bitmap plus an owned input queue. No GPU mapping. */
#define OS_WINDOW_SYSCALL 417
#define OS_WINDOW_VERSION 1
/* Opt-in client contract for a desktop that supplies full keyboard handoffs. */
#define OS_WINDOW_KEYBOARD_CLIENT_VERSION 2
/* Full keyboard handoffs and a physical-pixel surface. Desktop presentation
 * copies these pixels 1:1; source dimensions are independent of UI scale.
 * POINTER events use absolute physical surface coordinates; code1 is active,
 * code0 leaves the surface. v1/v2 retain their relative pointer convention. */
#define OS_WINDOW_PIXEL_CLIENT_VERSION 3
#define OS_WINDOW_SLOTS 4
#define OS_WINDOW_MAX_WIDTH 1024
#define OS_WINDOW_MAX_HEIGHT 768
#define OS_WINDOW_PIXEL_MAX_WIDTH 2560
#define OS_WINDOW_PIXEL_MAX_HEIGHT 1600
enum {
    OS_WINDOW_SERVER_START,
    OS_WINDOW_CREATE,
    OS_WINDOW_CLIENT_CLOSE,
    OS_WINDOW_PRESENT,
    OS_WINDOW_INPUT,
    OS_WINDOW_SERVER_NEXT,
    OS_WINDOW_SERVER_READ,
    OS_WINDOW_SERVER_SEND,
    OS_WINDOW_SERVER_FOCUS,
    OS_WINDOW_SERVER_CLOSE,
    OS_WINDOW_SERVER_KEYBOARD_GET,
    OS_WINDOW_SERVER_KEYBOARD_SET,
    OS_WINDOW_KEYBOARD_STATE,
    OS_WINDOW_SERVER_CONFIGURE,
    OS_WINDOW_GEOMETRY,
    OS_WINDOW_REQUEST_STATE,
    OS_WINDOW_SERVER_REQUEST,
    OS_WINDOW_CURSOR,
    OS_WINDOW_SERVER_CURSOR,
    OS_WINDOW_PRESENT_DAMAGE,
    OS_WINDOW_SERVER_READ_DAMAGE
};
enum {
    OS_WINDOW_KEY = 1,
    OS_WINDOW_POINTER,
    OS_WINDOW_OVERFLOW,
    OS_WINDOW_HID_KEY,
    OS_WINDOW_FOCUS,
    OS_WINDOW_CLOSE,
    OS_WINDOW_CONFIGURE
};
/* Additive physical-pixel geometry. Only the display owner requests sizes;
 * old front pixels survive until a complete client frame publishes a size.
 * Requests and presentation never resample the client's pixels. */
#define OS_WINDOW_STATE_MAXIMIZED 1u
#define OS_WINDOW_STATE_MINIMIZED 2u
#define OS_WINDOW_STATE_FULLSCREEN 4u
enum { OS_WINDOW_REQUEST_MAXIMIZE=1, OS_WINDOW_REQUEST_RESTORE,
       OS_WINDOW_REQUEST_MINIMIZE, OS_WINDOW_REQUEST_FULLSCREEN,
       OS_WINDOW_REQUEST_UNFULLSCREEN };
struct os_window_configure { uint32_t id, width, height, states; };
struct os_window_request { uint32_t id, action; };
/* Owned physical cursor pixels. Only the display owner reads these; the
 * desktop applies them while the pointer belongs to this focused surface.
 * Bitmap pixels are premultiplied ARGB, tightly copied without resampling.
 * A matching read sequence returns metadata without copying pixels again. */
#define OS_WINDOW_CURSOR_MAX 128u
enum { OS_WINDOW_CURSOR_DEFAULT, OS_WINDOW_CURSOR_BITMAP, OS_WINDOW_CURSOR_HIDDEN };
struct os_window_cursor {
    uint32_t id, mode, width, height, stride;
    int32_t hotspot_x, hotspot_y;
    uint32_t reserved;
    uint64_t pixels, sequence;
};
struct os_window_geometry {
    uint32_t id, width, height, states;
    uint32_t requested_width, requested_height, reserved0, reserved1;
    uint64_t sequence;
};
struct os_window_info {
    uint32_t version, id, client, width, height, reserved;
    uint64_t sequence;
    char title[64];
};
struct os_window_token {
    uint32_t id, reserved;
};
struct os_window_frame {
    uint32_t id, width, height, stride;
    uint64_t pixels, sequence;
};
/* Additive copied-region operations for physical-pixel clients. The frame
 * still describes a tightly packed complete bitmap; the rectangle selects
 * which rows/columns to transfer. PRESENT_DAMAGE publishes all selected
 * pixels atomically or leaves the front/sequence unchanged on failure.
 * The first frame and every size change must cover the entire bitmap.
 * READ_DAMAGE takes the sequence of the server's retained bitmap and returns
 * the union of changes since its last successful read. A stale sequence
 * forces a full copy; failed reads do not consume damage. No pixels outside
 * the returned rectangle are written. Existing full-frame operations retain
 * their ABI and behavior. Only the display owner can read either way. */
struct os_window_damage_frame {
    struct os_window_frame frame;
    uint32_t x, y, width, height;
};
struct os_window_events {
    uint32_t id, count;
    uint64_t events;
};
struct os_window_focus {
    uint32_t id, active;
};
/* Additive v1 handoff. Existing operations, records and queues are unchanged.
 * The display owner GETs the current generation/event watermark, then SETs
 * an authoritative source snapshot coherent through its source sequence.
 * It must not forward older source events again after that barrier.
 * Client reads require current focus/ownership; generation 0 means first read.
 * event_sequence covers the accepted window events reflected in the result;
 * state.sequence remains the original raw-input snapshot's source anchor. */
#define OS_WINDOW_KEYBOARD_VERSION 1
struct os_window_keyboard {
    uint32_t version, size, id, reserved;
    uint64_t generation, event_sequence;
    struct os_keyboard_snapshot state;
};
struct os_window_event {
    uint64_t sequence;
    uint32_t type, code;
    int32_t dx, dy, wheel;
    uint32_t buttons;
};
#ifdef __cplusplus
#define OS_WINDOW_ABI_ASSERT static_assert
#else
#define OS_WINDOW_ABI_ASSERT _Static_assert
#endif
OS_WINDOW_ABI_ASSERT(sizeof(struct os_window_info) == 96, "window info ABI");
OS_WINDOW_ABI_ASSERT(sizeof(struct os_window_frame) == 32, "window frame ABI");
OS_WINDOW_ABI_ASSERT(sizeof(struct os_window_damage_frame) == 48, "window damage frame ABI");
OS_WINDOW_ABI_ASSERT(sizeof(struct os_window_event) == 32, "window event ABI");
OS_WINDOW_ABI_ASSERT(sizeof(struct os_window_keyboard) == 104, "focused keyboard ABI");
OS_WINDOW_ABI_ASSERT(sizeof(struct os_window_configure) == 16, "configure ABI");
OS_WINDOW_ABI_ASSERT(sizeof(struct os_window_request) == 8, "state request ABI");
OS_WINDOW_ABI_ASSERT(sizeof(struct os_window_cursor) == 48, "physical cursor ABI");
OS_WINDOW_ABI_ASSERT(sizeof(struct os_window_geometry) == 40, "geometry ABI");
#undef OS_WINDOW_ABI_ASSERT
#endif
