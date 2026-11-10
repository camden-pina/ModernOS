#ifndef COAST_KEYBOARD_STATE_ABI_H
#define COAST_KEYBOARD_STATE_ABI_H
#include <stdint.h>
#define OS_KEYBOARD_STATE_VERSION 1
#define OS_KEYBOARD_VALID 1u
#define OS_KEYBOARD_LOCK_CAPS 1u
#define OS_KEYBOARD_LOCK_NUM 2u
#define OS_KEYBOARD_LOCK_SCROLL 4u
/* Input-stream snapshot, accessible only to the existing display/input owner.
   sequence is its event watermark; source_generation changes on discontinuity.
   Invalid means unknown state, never an assertion that all keys are lifted. */
struct os_keyboard_snapshot {
    uint32_t version, size;
    uint64_t sequence, source_generation;
    uint8_t pressed[32];
    uint32_t modifiers, locks, flags, reserved;
};
#ifdef __cplusplus
static_assert(sizeof(struct os_keyboard_snapshot) == 72, "keyboard snapshot ABI");
#else
_Static_assert(sizeof(struct os_keyboard_snapshot) == 72, "keyboard snapshot ABI");
#endif
#endif
