#ifndef USB_KEYBOARD_H
#define USB_KEYBOARD_H

#include <drivers/usb/usb.h>

#define USB_MOD_LEFT_CTRL (1u << 0)
#define USB_MOD_LEFT_SHIFT (1u << 1)
#define USB_MOD_LEFT_ALT (1u << 2)
#define USB_MOD_LEFT_GUI (1u << 3)
#define USB_MOD_RIGHT_CTRL (1u << 4)
#define USB_MOD_RIGHT_SHIFT (1u << 5)
#define USB_MOD_RIGHT_ALT (1u << 6)
#define USB_MOD_RIGHT_GUI (1u << 7)

typedef struct {
    uint8_t device_id;
    uint8_t endpoint_address; /* Distinguishes composite keyboard interfaces. */
    uint8_t usage;            /* USB HID Keyboard/Keypad usage, not PS/2 scancode. */
    uint8_t modifiers;
    bool pressed;
    char ascii; /* US layout on presses; zero for non-text keys/releases. */
} usb_keyboard_event_t;

/* Kernel input API. Call from the same context that calls usb_poll(). */
bool usb_keyboard_read_event(usb_keyboard_event_t *out);
uint32_t usb_keyboard_count(void);
uint32_t usb_keyboard_dropped_events(void);

/* USB core/class binding interface, not additional kernel initialization calls. */
typedef struct {
    uint8_t configuration;
    uint8_t interface_number;
    usb_endpoint_t endpoint;
} usb_keyboard_interface_t;

void usb_keyboard_init(void);
bool usb_keyboard_find_interface(const uint8_t *configuration, size_t size, uint8_t speed,
                                 usb_keyboard_interface_t *out);
bool usb_keyboard_bind(usb_device_t *device, const usb_keyboard_interface_t *interface);
void usb_keyboard_disconnect(usb_device_t *device);

#endif
