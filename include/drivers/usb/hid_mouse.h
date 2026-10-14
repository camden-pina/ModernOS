#ifndef HID_MOUSE_REPORT_H
#define HID_MOUSE_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_MOUSE_MAX_REPORTS 16u
#define HID_MOUSE_MAX_FIELDS 64u
#define HID_MOUSE_MAX_REPORT_BYTES 1024u

typedef enum {
    HID_MOUSE_X,
    HID_MOUSE_Y,
    HID_MOUSE_WHEEL,
    HID_MOUSE_HWHEEL,
    HID_MOUSE_BUTTON
} hid_mouse_field_kind_t;

typedef struct {
    uint16_t bit_offset;
    uint8_t bit_size;
    uint8_t report;
    uint8_t kind;
    uint8_t button;
    bool is_signed;
    bool nullable;
    int64_t minimum;
    int64_t maximum;
} hid_mouse_field_t;

typedef struct {
    uint16_t input_bits; /* Does not include an optional report-ID byte. */
    uint8_t id;
    uint8_t axes;
    uint32_t button_mask;
    bool mouse;
} hid_mouse_report_t;

typedef struct {
    bool uses_report_ids;
    uint8_t report_count;
    uint8_t field_count;
    hid_mouse_report_t reports[HID_MOUSE_MAX_REPORTS];
    hid_mouse_field_t fields[HID_MOUSE_MAX_FIELDS];
} hid_mouse_layout_t;

typedef struct {
    int32_t dx, dy, wheel, hwheel;
    uint32_t buttons;
    uint32_t button_mask; /* Only the buttons supplied by this report ID. */
} hid_mouse_sample_t;

typedef enum { HID_MOUSE_IGNORED, HID_MOUSE_INVALID, HID_MOUSE_DECODED } hid_mouse_decode_result_t;

/* Pure HID parsing: no controller, allocation, polling, GUI, or USB calls.
 * Supports relative Mouse applications, report IDs, bit-packed signed axes,
 * buttons 1..32, Wheel, and Consumer AC Pan. Bounded tables reject descriptors
 * outside these limits. Absolute axes, arrays of buttons, delimiter usage
 * sets, long items, and wheel resolution-multiplier features are unsupported.
 */
bool hid_mouse_parse_descriptor(const uint8_t *data, size_t size, hid_mouse_layout_t *out);
void hid_mouse_boot_layout(hid_mouse_layout_t *out);
bool hid_mouse_layout_fits(const hid_mouse_layout_t *layout, size_t packet_size);
hid_mouse_decode_result_t hid_mouse_decode(const hid_mouse_layout_t *layout, const uint8_t *data,
                                           size_t length, hid_mouse_sample_t *out);

#endif
