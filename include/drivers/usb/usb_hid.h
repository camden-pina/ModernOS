#ifndef USB_HID_H
#define USB_HID_H

#include <drivers/usb/usb.h>

#define USB_HID_MAX_INTERFACES 15u
#define USB_HID_CLASS 3u
#define USB_HID_SUBCLASS_BOOT 1u
#define USB_HID_PROTOCOL_KEYBOARD 1u
#define USB_HID_PROTOCOL_MOUSE 2u
#define USB_HID_DESCRIPTOR 0x21u
#define USB_HID_REPORT_DESCRIPTOR 0x22u
#define USB_HID_SET_PROTOCOL 0x0bu

typedef struct {
    uint8_t configuration;
    uint8_t interface_number;
    uint8_t subclass;
    uint8_t protocol;
    uint16_t report_descriptor_length;
    usb_endpoint_t endpoint;
} usb_hid_interface_t;

/* Collect alternate-setting-zero HID interfaces with one interrupt IN pipe.
 * The entire configuration is validated before any interface is used.
 * High-bandwidth HS endpoints, hubs, and nonzero alternate settings remain
 * outside this early stack's capabilities.
 */
bool usb_hid_parse_configuration(const uint8_t *data, size_t size, uint8_t speed,
                                 usb_hid_interface_t *out, size_t capacity, size_t *count);
bool usb_hid_get_report_descriptor(usb_device_t *device, const usb_hid_interface_t *interface,
                                   void *data, uint16_t *actual);
bool usb_hid_set_protocol(usb_device_t *device, uint8_t interface_number, bool report_protocol);

#endif
