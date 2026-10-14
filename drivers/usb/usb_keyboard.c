#include <drivers/usb/usb_keyboard.h>
#include <printk.h>
#include <string.h>

#define USB_KEY_QUEUE 256u

typedef struct {
    bool attached;
    bool caps_lock;
    usb_device_t *device;
    uint8_t endpoint_address;
    uint8_t previous_report[8];
} usb_keyboard_binding_t;

static struct {
    usb_keyboard_binding_t bindings[USB_MAX_DEVICES + 1][16];
    usb_keyboard_event_t keys[USB_KEY_QUEUE];
    uint16_t key_head;
    uint16_t key_tail;
    uint32_t dropped;
} keyboard;

static char usage_ascii(uint8_t usage, uint8_t modifiers, bool caps_lock) {
    bool shift = (modifiers & (USB_MOD_LEFT_SHIFT | USB_MOD_RIGHT_SHIFT)) != 0;
    if (usage >= 0x04 && usage <= 0x1d) {
        char c = (char)('a' + usage - 0x04);
        return (shift != caps_lock) ? (char)(c - 'a' + 'A') : c;
    }
    if (usage >= 0x1e && usage <= 0x27) {
        static const char normal[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        return shift ? shifted[usage - 0x1e] : normal[usage - 0x1e];
    }
    switch (usage) {
    case 0x28:
        return '\n';
    case 0x29:
        return 27;
    case 0x2a:
        return '\b';
    case 0x2b:
        return '\t';
    case 0x2c:
        return ' ';
    case 0x2d:
        return shift ? '_' : '-';
    case 0x2e:
        return shift ? '+' : '=';
    case 0x2f:
        return shift ? '{' : '[';
    case 0x30:
        return shift ? '}' : ']';
    case 0x31:
        return shift ? '|' : '\\';
    case 0x32:
        return shift ? '~' : '#';
    case 0x33:
        return shift ? ':' : ';';
    case 0x34:
        return shift ? '"' : '\'';
    case 0x35:
        return shift ? '~' : '`';
    case 0x36:
        return shift ? '<' : ',';
    case 0x37:
        return shift ? '>' : '.';
    case 0x38:
        return shift ? '?' : '/';
    default:
        return 0;
    }
}

static void emit_key(usb_keyboard_binding_t *dev, uint8_t usage, uint8_t mods, bool down) {
    uint16_t next = (uint16_t)((keyboard.key_head + 1u) % USB_KEY_QUEUE);
    if (next == keyboard.key_tail) {
        ++keyboard.dropped;
        return;
    }
    keyboard.keys[keyboard.key_head] =
        (usb_keyboard_event_t){.device_id = dev->device->id,
                               .endpoint_address = dev->endpoint_address,
                               .usage = usage,
                               .modifiers = mods,
                               .pressed = down,
                               .ascii = down ? usage_ascii(usage, mods, dev->caps_lock) : 0};
    keyboard.key_head = next;
}

static bool report_contains(const uint8_t report[8], uint8_t usage) {
    for (unsigned i = 2; i < 8; ++i)
        if (report[i] == usage)
            return true;
    return false;
}

static bool duplicate_usage(const uint8_t report[8], unsigned index) {
    for (unsigned i = 2; i < index; ++i)
        if (report[i] == report[index])
            return true;
    return false;
}

static void process_keyboard_report(usb_keyboard_binding_t *dev, const uint8_t report[8]) {
    uint8_t mods = report[0];
    uint8_t changed = mods ^ dev->previous_report[0];
    for (unsigned bit = 0; bit < 8; ++bit)
        if (changed & (1u << bit))
            emit_key(dev, (uint8_t)(0xe0u + bit), mods, (mods & (1u << bit)) != 0);
    dev->previous_report[0] = mods;

    /* ErrorRollOver, POSTFail, ErrorUndefined are not ordinary key usages.
     * Preserve the last valid six-key set until a valid report arrives.
     * Modifier changes are still reliable and are delivered above.
     */
    for (unsigned i = 2; i < 8; ++i)
        if (report[i] >= 1 && report[i] <= 3)
            return;

    for (unsigned i = 2; i < 8; ++i) {
        uint8_t key = dev->previous_report[i];
        if (key && !duplicate_usage(dev->previous_report, i) && !report_contains(report, key))
            emit_key(dev, key, mods, false);
    }
    /* Apply lock change before translating other newly pressed keys. */
    if (report_contains(report, 0x39) && !report_contains(dev->previous_report, 0x39))
        dev->caps_lock = !dev->caps_lock;
    for (unsigned i = 2; i < 8; ++i) {
        uint8_t key = report[i];
        if (key && !duplicate_usage(report, i) && !report_contains(dev->previous_report, key))
            emit_key(dev, key, mods, true);
    }
    memcpy(dev->previous_report, report, 8);
}

bool usb_keyboard_find_interface(const uint8_t *data, size_t size, uint8_t speed,
                                 usb_keyboard_interface_t *out) {
    if (!data || !out || size < 9 || data[0] != 9 || data[1] != 2 ||
        usb_read_le16(data + 2) != size || data[5] == 0)
        return false;
    bool candidate = false;
    bool found = false;
    uint8_t iface = 0;
    usb_keyboard_interface_t chosen = {0};
    for (size_t offset = 9; offset < size;) {
        if (size - offset < 2)
            return false;
        uint8_t len = data[offset];
        uint8_t type = data[offset + 1];
        if (len < 2 || len > size - offset)
            return false;
        const uint8_t *d = data + offset;
        if (type == 4) {
            if (len < 9)
                return false;
            iface = d[2];
            candidate = d[3] == 0 && d[4] != 0 && d[5] == 3 && d[6] == 1 && d[7] == 1;
        } else if (type == 5) {
            if (len < 7)
                return false;
            uint16_t packet = usb_read_le16(d + 4);
            uint16_t mps = packet & 0x7ffu;
            bool valid_packet = !(packet & 0xf800u) && mps >= 8 &&
                                ((speed == 2 && mps <= 8) || (speed == 1 && mps <= 64) ||
                                 (speed == 3 && mps <= 1024));
            bool valid_interval = d[6] != 0 && (speed != 3 || d[6] <= 16);
            if (!found && candidate && (d[2] & 0x80u) && (d[2] & 0x0fu) && !(d[2] & 0x70u) &&
                (d[3] & 3u) == 3u && valid_packet && valid_interval) {
                chosen = (usb_keyboard_interface_t){.configuration = data[5],
                                                    .interface_number = iface,
                                                    .endpoint = {.address = d[2],
                                                                 .attributes = d[3],
                                                                 .max_packet_size = mps,
                                                                 .interval = d[6]}};
                found = true;
            }
        }
        offset += len;
    }
    if (found)
        *out = chosen;
    return found;
}

void usb_keyboard_init(void) {
    memset(&keyboard, 0, sizeof(keyboard));
}

static void disconnect_binding(usb_keyboard_binding_t *binding) {
    if (!binding->attached)
        return;
    static const uint8_t released[8] = {0};
    process_keyboard_report(binding, released);
    binding->attached = false;
}

void usb_keyboard_disconnect(usb_device_t *device) {
    if (!device || !device->id || device->id > USB_MAX_DEVICES)
        return;
    for (unsigned i = 1; i < 16; ++i) {
        usb_keyboard_binding_t *binding = &keyboard.bindings[device->id][i];
        if (binding->device == device)
            disconnect_binding(binding);
    }
}

static void keyboard_report_complete(usb_device_t *device, usb_transfer_status_t status,
                                     const uint8_t *data, size_t length, void *context) {
    usb_keyboard_binding_t *binding = context;
    if (!binding->attached || binding->device != device)
        return;
    if (status == USB_TRANSFER_ERROR) {
        kprintf("USB keyboard %u: interrupt transfer failed\n\r", device->id);
        disconnect_binding(binding);
        return;
    }
    if (data && length >= 8)
        process_keyboard_report(binding, data);
    if (!device->connected || !usb_submit_interrupt_in(device, binding->endpoint_address,
                                                       keyboard_report_complete, binding))
        disconnect_binding(binding);
}

bool usb_keyboard_bind(usb_device_t *device, const usb_keyboard_interface_t *interface) {
    if (!device || !interface || !device->connected || !device->id ||
        device->id > USB_MAX_DEVICES || !(interface->endpoint.address & 0x80u) ||
        !(interface->endpoint.address & 15u) || (interface->endpoint.address & 0x70u) ||
        interface->endpoint.max_packet_size < 8)
        return false;
    usb_keyboard_binding_t *binding =
        &keyboard.bindings[device->id][interface->endpoint.address & 15u];
    if (binding->attached)
        return false;
    /* Controller endpoint configuration precedes SET_CONFIGURATION. */
    if (!usb_configure_interrupt_in(device, &interface->endpoint) ||
        !usb_set_configuration(device, interface->configuration))
        return false;
    usb_setup_packet_t setup = {.request_type = 0x21,
                                .request = 0x0b, /* HID SET_PROTOCOL */
                                .value = 0,      /* Boot protocol */
                                .index = interface->interface_number,
                                .length = 0};
    if (!usb_control_transfer(device, &setup, NULL, NULL))
        return false;

    memset(binding, 0, sizeof(*binding));
    binding->device = device;
    binding->endpoint_address = interface->endpoint.address;
    binding->attached = true;
    if (!usb_submit_interrupt_in(device, binding->endpoint_address, keyboard_report_complete,
                                 binding)) {
        binding->attached = false;
        return false;
    }
    kprintf("USB keyboard ready: port %u device %u interface %u endpoint %u\n\r", device->root_port,
            device->id, interface->interface_number, interface->endpoint.address);
    return true;
}

uint32_t usb_keyboard_count(void) {
    if (!usb_is_running())
        return 0;
    uint32_t count = 0;
    for (unsigned i = 1; i <= USB_MAX_DEVICES; ++i) {
        for (unsigned ep = 1; ep < 16; ++ep) {
            usb_keyboard_binding_t *binding = &keyboard.bindings[i][ep];
            if (binding->attached && binding->device->connected)
                ++count;
        }
    }
    return count;
}

bool usb_keyboard_read_event(usb_keyboard_event_t *out) {
    if (!out || keyboard.key_head == keyboard.key_tail)
        return false;
    *out = keyboard.keys[keyboard.key_tail];
    keyboard.key_tail = (uint16_t)((keyboard.key_tail + 1u) % USB_KEY_QUEUE);
    return true;
}

uint32_t usb_keyboard_dropped_events(void) {
    return keyboard.dropped;
}
