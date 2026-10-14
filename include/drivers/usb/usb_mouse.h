#ifndef USB_MOUSE_H
#define USB_MOUSE_H

#include <drivers/usb/usb_hid.h>
#include <input/mouse.h>

#define USB_MOUSE_MAX_BINDINGS 16u

/* USB class lifecycle only; the consumer reads input/mouse.h events.
 * usb_init() calls init, which also initializes the generic mouse queue.
 * Count is the number of attached mouse interfaces, not USB devices.
 */
void usb_mouse_init(void);
bool usb_mouse_supports_interface(const usb_hid_interface_t *interface);
bool usb_mouse_bind(usb_device_t *device, const usb_hid_interface_t *interface);
void usb_mouse_disconnect(usb_device_t *device);
uint32_t usb_mouse_count(void);
uint64_t usb_mouse_invalid_reports(void);

/* Maps slot 1..USB_MAX_DEVICES and endpoint IN 1..15 to a unique input source.
 * Zero indicates invalid arguments. No bootloader or framebuffer dependency.
 */
uint16_t usb_mouse_source_id(uint8_t device_id, uint8_t endpoint_address);

#endif
