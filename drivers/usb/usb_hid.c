#include <drivers/usb/usb_hid.h>
#include <string.h>

static bool valid_endpoint(const uint8_t *d, uint8_t speed, usb_endpoint_t *out) {
    uint16_t packet = usb_read_le16(d + 4);
    uint16_t mps = packet & 0x7ffu;
    if (!(d[2] & 0x80u) || !(d[2] & 15u) || (d[2] & 0x70u) || (d[3] & 3u) != 3u ||
        (packet & 0xf800u) || !mps || !d[6] ||
        !((speed == USB_SPEED_LOW && mps <= 8) || (speed == USB_SPEED_FULL && mps <= 64) ||
          (speed == USB_SPEED_HIGH && mps <= 1024 && d[6] <= 16)))
        return false;
    *out = (usb_endpoint_t){
        .address = d[2], .attributes = d[3], .max_packet_size = mps, .interval = d[6]};
    return true;
}

static bool append_interface(const usb_hid_interface_t *candidate, bool active,
                             usb_hid_interface_t *out, size_t capacity, size_t *count) {
    if (!active || !candidate->endpoint.address)
        return true;
    if (*count == capacity)
        return false;
    for (size_t i = 0; i < *count; ++i)
        if (out[i].interface_number == candidate->interface_number ||
            out[i].endpoint.address == candidate->endpoint.address)
            return false;
    out[(*count)++] = *candidate;
    return true;
}

bool usb_hid_parse_configuration(const uint8_t *data, size_t size, uint8_t speed,
                                 usb_hid_interface_t *out, size_t capacity, size_t *count) {
    if (count)
        *count = 0;
    if (!data || !out || !count || size < 9 || data[0] != 9 ||
        data[1] != USB_DESCRIPTOR_CONFIGURATION || usb_read_le16(data + 2) != size || !data[5])
        return false;
    size_t found = 0;
    bool active = false;
    bool have_hid = false;
    usb_hid_interface_t candidate = {0};
    for (size_t offset = 9; offset < size;) {
        if (size - offset < 2)
            return false;
        const uint8_t *d = data + offset;
        uint8_t length = d[0], type = d[1];
        if (length < 2 || length > size - offset)
            return false;
        if (type == USB_DESCRIPTOR_INTERFACE) {
            if (length < 9 || !append_interface(&candidate, active, out, capacity, &found))
                return false;
            active = d[3] == 0 && d[4] != 0 && d[5] == USB_HID_CLASS;
            have_hid = false;
            candidate = (usb_hid_interface_t){.configuration = data[5],
                                              .interface_number = d[2],
                                              .subclass = d[6],
                                              .protocol = d[7]};
        } else if (type == USB_HID_DESCRIPTOR && active) {
            if (length < 6 || have_hid || d[5] == 0 || 6u + (size_t)d[5] * 3u > length)
                return false;
            have_hid = true;
            for (unsigned i = 0; i < d[5]; ++i) {
                const uint8_t *entry = d + 6u + i * 3u;
                if (entry[0] == USB_HID_REPORT_DESCRIPTOR) {
                    if (candidate.report_descriptor_length)
                        return false;
                    candidate.report_descriptor_length = usb_read_le16(entry + 1);
                }
            }
        } else if (type == USB_DESCRIPTOR_ENDPOINT) {
            if (length < 7)
                return false;
            if (active && !candidate.endpoint.address)
                (void)valid_endpoint(d, speed, &candidate.endpoint);
        }
        offset += length;
    }
    if (!append_interface(&candidate, active, out, capacity, &found))
        return false;
    *count = found;
    return true;
}

bool usb_hid_get_report_descriptor(usb_device_t *device, const usb_hid_interface_t *interface,
                                   void *data, uint16_t *actual) {
    if (!interface || !interface->report_descriptor_length ||
        interface->report_descriptor_length > USB_DESCRIPTOR_BUFFER_SIZE)
        return false;
    usb_setup_packet_t setup = {.request_type = 0x81,
                                .request = USB_REQUEST_GET_DESCRIPTOR,
                                .value = USB_HID_REPORT_DESCRIPTOR << 8,
                                .index = interface->interface_number,
                                .length = interface->report_descriptor_length};
    return usb_control_transfer(device, &setup, data, actual);
}

bool usb_hid_set_protocol(usb_device_t *device, uint8_t interface_number, bool report_protocol) {
    usb_setup_packet_t setup = {.request_type = 0x21,
                                .request = USB_HID_SET_PROTOCOL,
                                .value = report_protocol ? 1 : 0,
                                .index = interface_number};
    return usb_control_transfer(device, &setup, NULL, NULL);
}
