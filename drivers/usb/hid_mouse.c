#include <drivers/usb/hid_mouse.h>
#include <string.h>

#define HID_STACK_SIZE 16u
#define HID_USAGE_RANGES 64u
#define HID_USAGE_MOUSE 0x00010002u
#define HID_USAGE_X 0x00010030u
#define HID_USAGE_Y 0x00010031u
#define HID_USAGE_WHEEL 0x00010038u
#define HID_USAGE_PAN 0x000c0238u

typedef struct {
    uint32_t usage_page;
    uint32_t report_size;
    uint32_t report_count;
    int64_t logical_min;
    int64_t logical_max;
    uint8_t report_id;
} hid_global_t;

typedef struct {
    uint32_t first, last;
} hid_usage_range_t;

typedef struct {
    hid_usage_range_t ranges[HID_USAGE_RANGES];
    unsigned count;
    bool pending_minimum;
    uint32_t minimum;
} hid_local_t;

static uint32_t item_unsigned(const uint8_t *data, unsigned size) {
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (uint32_t)data[i] << (i * 8u);
    return value;
}

static int64_t item_signed(uint32_t value, unsigned size) {
    unsigned bits = size * 8u;
    if (bits && (value & (1u << (bits - 1u))))
        return (int64_t)value - (1ll << bits);
    return value;
}

static uint32_t usage_value(uint32_t value, unsigned size, uint32_t page) {
    return size == 4 ? value : (page << 16) | value;
}

static bool append_usage(hid_local_t *local, uint32_t first, uint32_t last) {
    if (local->count == HID_USAGE_RANGES || first > last || (first >> 16) != (last >> 16))
        return false;
    local->ranges[local->count++] = (hid_usage_range_t){first, last};
    return true;
}

static uint32_t local_usage(const hid_local_t *local, uint32_t index) {
    uint32_t last = 0;
    for (unsigned i = 0; i < local->count; ++i) {
        uint32_t length = local->ranges[i].last - local->ranges[i].first + 1u;
        if (index < length)
            return local->ranges[i].first + index;
        index -= length;
        last = local->ranges[i].last;
    }
    return last; /* Variable fields repeat the final usage when necessary. */
}

static int report_index(hid_mouse_layout_t *layout, uint8_t id) {
    for (unsigned i = 0; i < layout->report_count; ++i)
        if (layout->reports[i].id == id)
            return (int)i;
    if (layout->report_count == HID_MOUSE_MAX_REPORTS)
        return -1;
    unsigned index = layout->report_count++;
    layout->reports[index].id = id;
    return (int)index;
}

static bool add_input(hid_mouse_layout_t *layout, const hid_global_t *global,
                      const hid_local_t *local, uint32_t flags, bool mouse_collection) {
    int index = report_index(layout, global->report_id);
    if (index < 0 || !global->report_size || !global->report_count)
        return false;
    hid_mouse_report_t *report = &layout->reports[index];
    uint64_t bits = (uint64_t)global->report_size * global->report_count;
    if (bits > HID_MOUSE_MAX_REPORT_BYTES * 8u - report->input_bits)
        return false;
    uint16_t start = report->input_bits;
    report->input_bits = (uint16_t)(report->input_bits + bits);
    if (!mouse_collection || (flags & 1u))
        return true; /* Constant/non-mouse input still consumes report bits. */

    for (uint32_t i = 0; i < global->report_count; ++i) {
        uint32_t usage = local_usage(local, i);
        hid_mouse_field_kind_t kind;
        uint8_t button = 0, axis = 0;
        switch (usage) {
        case HID_USAGE_X:
            kind = HID_MOUSE_X;
            axis = 1u << 0;
            break;
        case HID_USAGE_Y:
            kind = HID_MOUSE_Y;
            axis = 1u << 1;
            break;
        case HID_USAGE_WHEEL:
            kind = HID_MOUSE_WHEEL;
            axis = 1u << 2;
            break;
        case HID_USAGE_PAN:
            kind = HID_MOUSE_HWHEEL;
            axis = 1u << 3;
            break;
        default:
            if ((usage >> 16) != 9 || !(usage & 0xffffu) || (usage & 0xffffu) > 32)
                continue;
            kind = HID_MOUSE_BUTTON;
            button = (uint8_t)((usage & 0xffffu) - 1u);
            break;
        }
        if (!(flags & 2u) || global->report_size > 32 ||
            global->logical_min > global->logical_max ||
            layout->field_count == HID_MOUSE_MAX_FIELDS)
            return false;
        if (kind == HID_MOUSE_BUTTON) {
            if ((flags & 4u) || global->logical_min != 0 || global->logical_max != 1 ||
                (report->button_mask & (1u << button)))
                return false;
            report->button_mask |= 1u << button;
        } else {
            if (!(flags & 4u) || global->logical_min < INT32_MIN ||
                global->logical_max > INT32_MAX || (report->axes & axis))
                return false;
            report->axes |= axis;
        }
        bool is_signed = global->logical_min < 0;
        int64_t minimum = is_signed ? -(1ll << (global->report_size - 1u)) : 0;
        int64_t maximum =
            is_signed ? (1ll << (global->report_size - 1u)) - 1 : (1ll << global->report_size) - 1;
        if (global->logical_min < minimum || global->logical_max > maximum)
            return false;
        layout->fields[layout->field_count++] =
            (hid_mouse_field_t){.bit_offset = (uint16_t)(start + i * global->report_size),
                                .bit_size = (uint8_t)global->report_size,
                                .report = (uint8_t)index,
                                .kind = (uint8_t)kind,
                                .button = button,
                                .is_signed = is_signed,
                                .nullable = (flags & 0x40u) != 0,
                                .minimum = global->logical_min,
                                .maximum = global->logical_max};
        report->mouse = true;
    }
    return true;
}

bool hid_mouse_parse_descriptor(const uint8_t *data, size_t size, hid_mouse_layout_t *out) {
    if (!data || !out || !size)
        return false;

    memset(out, 0, sizeof(*out));
    hid_global_t global = {0}, stack[HID_STACK_SIZE];
    hid_local_t local = {0};
    bool collection_stack[HID_STACK_SIZE];
    unsigned global_depth = 0, collection_depth = 0;
    bool mouse_collection = false;

    for (size_t offset = 0; offset < size;) {
        uint8_t prefix = data[offset++];
        if (prefix == 0xfe) /* No defined HID 1.11 long items are needed here. */
            return false;
        unsigned length = prefix & 3u;
        if (length == 3)
            length = 4;
        if (length > size - offset)
            return false;

        uint32_t value = item_unsigned(data + offset, length);
        offset += length;
        unsigned type = (prefix >> 2) & 3u, tag = prefix >> 4;

        if (type == 1) { /* Global items survive Main items and collections. */
            switch (tag) {
            case 0:
                if (value > 0xffffu)
                    return false;
                global.usage_page = value;
                break;

            case 1:
                global.logical_min = item_signed(value, length);
                break;

            case 2:
                global.logical_max =
                    global.logical_min < 0 ? item_signed(value, length) : (int64_t)value;
                break;

            case 7:
                global.report_size = value;
                break;

            case 8:
                if (!value || value > 255)
                    return false;
                global.report_id = (uint8_t)value;
                out->uses_report_ids = true;
                break;

            case 9:
                global.report_count = value;
                break;

            case 10:
                if (length || global_depth == HID_STACK_SIZE)
                    return false;
                stack[global_depth++] = global;
                break;

            case 11:
                if (length || !global_depth)
                    return false;
                global = stack[--global_depth];
                break;

            default:
                break; /* Units and physical ranges do not change offsets. */
            }
        } else if (type == 2) { /* Local usages reset after every Main item. */
            uint32_t usage = usage_value(value, length, global.usage_page);
            switch (tag) {
            case 0:
                if (local.pending_minimum || !length || !append_usage(&local, usage, usage))
                    return false;
                break;

            case 1:
                if (local.pending_minimum || !length)
                    return false;
                local.minimum = usage;
                local.pending_minimum = true;
                break;

            case 2:
                if (!local.pending_minimum || !length ||
                    !append_usage(&local, local.minimum, usage))
                    return false;
                local.pending_minimum = false;
                break;

            case 10:
                return false; /* Delimiter alternative sets unsupported. */

            default:
                break;
            }
        } else if (type == 0) {
            if (local.pending_minimum)
                return false;
            switch (tag) {
            case 8:
                if (!length || !collection_depth ||
                    !add_input(out, &global, &local, value, mouse_collection))
                    return false;
                break;

            case 9:
            case 11:
                break; /* Output/Feature use independent bit offsets. */

            case 10:
                if (length != 1 || collection_depth == HID_STACK_SIZE)
                    return false;
                collection_stack[collection_depth++] = mouse_collection;
                if (value == 1) /* Application; nested physical groups inherit. */
                    mouse_collection = local_usage(&local, 0) == HID_USAGE_MOUSE;
                break;

            case 12:
                if (length || !collection_depth)
                    return false;
                mouse_collection = collection_stack[--collection_depth];
                break;

            default:
                return false;
            }
            memset(&local, 0, sizeof(local));
        } else {
            return false;
        }
    }
    if (collection_depth || global_depth || local.pending_minimum)
        return false;
    uint8_t axes = 0;
    for (unsigned i = 0; i < out->report_count; ++i) {
        if (out->uses_report_ids && out->reports[i].id == 0)
            return false;
        axes |= out->reports[i].axes;
    }
    return (axes & 3u) == 3u;
}

void hid_mouse_boot_layout(hid_mouse_layout_t *out) {
    memset(out, 0, sizeof(*out));
    out->report_count = 1;
    out->field_count = 5;
    out->reports[0] =
        (hid_mouse_report_t){.input_bits = 24, .axes = 3, .button_mask = 7, .mouse = true};
    for (unsigned i = 0; i < 3; ++i)
        out->fields[i] = (hid_mouse_field_t){.bit_offset = (uint16_t)i,
                                             .bit_size = 1,
                                             .kind = HID_MOUSE_BUTTON,
                                             .button = (uint8_t)i,
                                             .minimum = 0,
                                             .maximum = 1};
    for (unsigned i = 0; i < 2; ++i)
        out->fields[3 + i] = (hid_mouse_field_t){.bit_offset = (uint16_t)(8 + i * 8),
                                                 .bit_size = 8,
                                                 .kind = (uint8_t)(i ? HID_MOUSE_Y : HID_MOUSE_X),
                                                 .is_signed = true,
                                                 .minimum = -127,
                                                 .maximum = 127};
}

bool hid_mouse_layout_fits(const hid_mouse_layout_t *layout, size_t packet_size) {
    if (!layout || !layout->field_count || !layout->report_count)
        return false;
    for (unsigned i = 0; i < layout->report_count; ++i) {
        size_t bytes =
            (layout->reports[i].input_bits + 7u) / 8u + (layout->uses_report_ids ? 1u : 0u);
        if (bytes > packet_size)
            return false;
    }
    return true;
}

static int64_t field_value(const hid_mouse_field_t *field, const uint8_t *data) {
    uint32_t raw = 0;
    for (unsigned i = 0; i < field->bit_size; ++i) {
        unsigned bit = field->bit_offset + i;
        raw |= (uint32_t)((data[bit / 8u] >> (bit % 8u)) & 1u) << i;
    }
    if (field->is_signed && (raw & (1u << (field->bit_size - 1u))))
        return (int64_t)raw - (1ll << field->bit_size);
    return raw;
}

hid_mouse_decode_result_t hid_mouse_decode(const hid_mouse_layout_t *layout, const uint8_t *data,
                                           size_t length, hid_mouse_sample_t *out) {
    if (!layout || !data || !out || !length)
        return HID_MOUSE_INVALID;
    uint8_t id = 0;
    if (layout->uses_report_ids) {
        id = *data++;
        --length;
    }
    unsigned index;
    for (index = 0; index < layout->report_count; ++index)
        if (layout->reports[index].id == id)
            break;
    if (index == layout->report_count || !layout->reports[index].mouse)
        return HID_MOUSE_IGNORED;
    const hid_mouse_report_t *report = &layout->reports[index];
    if (length < (report->input_bits + 7u) / 8u)
        return HID_MOUSE_INVALID;
    *out = (hid_mouse_sample_t){.button_mask = report->button_mask};
    for (unsigned i = 0; i < layout->field_count; ++i) {
        const hid_mouse_field_t *field = &layout->fields[i];
        if (field->report != index)
            continue;
        int64_t value = field_value(field, data);
        if (value < field->minimum || value > field->maximum) {
            if (field->nullable) {
                if (field->kind == HID_MOUSE_BUTTON)
                    out->button_mask &= ~(1u << field->button);
                continue;
            }
            return HID_MOUSE_INVALID;
        }
        switch (field->kind) {
        case HID_MOUSE_X:
            out->dx = (int32_t)value;
            break;
        case HID_MOUSE_Y:
            out->dy = (int32_t)value;
            break;
        case HID_MOUSE_WHEEL:
            out->wheel = (int32_t)value;
            break;
        case HID_MOUSE_HWHEEL:
            out->hwheel = (int32_t)value;
            break;
        case HID_MOUSE_BUTTON:
            if (value)
                out->buttons |= 1u << field->button;
            break;
        }
    }
    return HID_MOUSE_DECODED;
}
