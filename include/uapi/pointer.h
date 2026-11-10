#ifndef COAST_POINTER_ABI_H
#define COAST_POINTER_ABI_H
#include <uapi/abi.h>
#include <uapi/pointer_caps.h>
/* Opt-in extended input preserves the original 32-byte raw application ABI. */
struct os_input_event_v2 {
    struct os_input_event event;
    uint64_t timestamp_us;
    uint16_t source_id, reserved;
    uint32_t capabilities;
};
_Static_assert(sizeof(struct os_input_event_v2) == 48, "extended input ABI");
#endif
