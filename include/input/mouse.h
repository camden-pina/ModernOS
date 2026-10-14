#ifndef INPUT_MOUSE_H
#define INPUT_MOUSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MOUSE_MAX_SOURCES 256u
#define MOUSE_BUTTON_LEFT (1u << 0)
#define MOUSE_BUTTON_RIGHT (1u << 1)
#define MOUSE_BUTTON_MIDDLE (1u << 2)
#define MOUSE_BUTTON_BACK (1u << 3)
#define MOUSE_BUTTON_FORWARD (1u << 4)

#define MOUSE_EVENT_CONNECTED (1u << 0)
#define MOUSE_EVENT_DISCONNECTED (1u << 1)
#define MOUSE_EVENT_OVERRUN (1u << 2)

/* One complete input sample. Button bits correspond to HID button usages
 * 1..32; buttons 4/5 conventionally mean back/forward. +X is right, +Y down.
 * Wheel values are raw relative HID units; no acceleration or GUI policy.
 * Fixed-width fields keep a future character-device ABI straightforward.
 * This is a custom packet ABI, not the Linux evdev binary ABI.
 */
typedef struct {
    uint64_t sequence;
    uint16_t source_id;
    uint16_t flags;
    uint32_t buttons; /* Complete current state for this source. */
    uint32_t changed; /* Buttons that changed in this sample. */
    int32_t dx;
    int32_t dy;
    int32_t wheel;
    int32_t hwheel;
    uint32_t reserved;
} mouse_event_t;

_Static_assert(sizeof(mouse_event_t) == 40, "mouse event ABI size");

typedef struct {
    bool connected;
    uint32_t buttons;
} mouse_state_t;

/* Kernel consumer API. Read does not poll hardware. Empty means no event;
 * a future nonblocking device read can translate this to EAGAIN.
 * All producers and consumers must use the same serialized input context.
 * For SMP, IRQ-driven input, or multiple readers, add locking and per-open
 * queues in the input/VFS layer rather than calling this queue concurrently.
 */
bool mouse_read_event(mouse_event_t *out);
size_t mouse_read_events(mouse_event_t *out, size_t capacity);
bool mouse_get_state(uint16_t source_id, mouse_state_t *out);
uint64_t mouse_dropped_events(void);

/* Driver-facing API; initialize once before attaching any input sources.
 * Source zero is reserved. Overflow drops the oldest sample; the next read
 * carries OVERRUN. Resynchronize buttons using mouse_get_state(), since lost
 * relative motion cannot be reconstructed. Disconnect always publishes a
 * zero-button state even if the source had no buttons held.
 */
void mouse_init(void);
bool mouse_attach(uint16_t source_id);
void mouse_detach(uint16_t source_id);
void mouse_publish(uint16_t source_id, int32_t dx, int32_t dy, int32_t wheel, int32_t hwheel,
                   uint32_t buttons);

#endif
