#ifndef USB_H
#define USB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/pci/pci.h>

#define USB_MAX_DEVICES 16u
#define USB_DESCRIPTOR_BUFFER_SIZE 4096u

#define USB_SPEED_FULL 1u
#define USB_SPEED_LOW 2u
#define USB_SPEED_HIGH 3u

#define USB_REQUEST_GET_DESCRIPTOR 6u
#define USB_REQUEST_SET_CONFIGURATION 9u
#define USB_DESCRIPTOR_DEVICE 1u
#define USB_DESCRIPTOR_CONFIGURATION 2u
#define USB_DESCRIPTOR_INTERFACE 4u
#define USB_DESCRIPTOR_ENDPOINT 5u

typedef struct usb_device usb_device_t;

typedef struct {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} usb_setup_packet_t;

typedef struct {
    uint8_t address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
} usb_endpoint_t;

typedef enum {
    USB_TRANSFER_SUCCESS,
    USB_TRANSFER_SHORT_PACKET,
    USB_TRANSFER_ERROR
} usb_transfer_status_t;

/* Data belongs to the host controller and is valid only during the callback.
 * Callbacks may submit the next IN transfer, but must not call usb_poll(),
 * usb_init(), or a blocking control transfer recursively.
 */
typedef void (*usb_interrupt_callback_t)(usb_device_t *device, usb_transfer_status_t status,
                                         const uint8_t *data, size_t length, void *context);

/* Transport operations used by USB core/class drivers. Kernel callers only
 * need usb_init()/usb_poll() and the keyboard/mouse event APIs.
 */
typedef struct {
    bool (*control)(usb_device_t *, const usb_setup_packet_t *, void *, uint16_t *);
    bool (*set_ep0_packet_size)(usb_device_t *, uint16_t);
    bool (*configure_interrupt_in)(usb_device_t *, const usb_endpoint_t *);
    bool (*submit_interrupt_in)(usb_device_t *, uint8_t, usb_interrupt_callback_t, void *);
    void (*close)(usb_device_t *);
} usb_host_ops_t;

struct usb_device {
    uint8_t id; /* Logical device ID; currently equal to the xHCI slot ID. */
    uint8_t root_port;
    uint8_t speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t ep0_max_packet_size;
    bool connected;
    uint8_t configuration; /* Successfully selected configuration, or zero. */
    const usb_host_ops_t *host_ops;
    void *host_data; /* Private controller state; class drivers leave it alone. */
    void (*on_disconnect)(usb_device_t *);
};

/* Call once after your kernel's memory, PCI, and platform hooks are ready.
 * pci_hdr must describe the xHCI controller found by your PCI discovery code.
 * true means the controller started; inspect class-driver counts separately.
 * One controller, boot-time enumeration, serialized polling.
 */
bool usb_init(pci_device_header_t *pci_hdr);
void usb_poll(void);
bool usb_is_running(void);

/* Shared USB operations for class drivers. Control data is ordinary CPU
 * memory; the controller performs DMA allocation/copies internally.
 * When setup.length > 0, data must address at least that many bytes.
 */
bool usb_control_transfer(usb_device_t *device, const usb_setup_packet_t *setup, void *data,
                          uint16_t *actual);
bool usb_get_descriptor(usb_device_t *device, uint8_t type, uint8_t index, void *data,
                        uint16_t length, uint16_t *actual);
bool usb_set_configuration(usb_device_t *device, uint8_t configuration);
bool usb_configure_interrupt_in(usb_device_t *device, const usb_endpoint_t *endpoint);
/* endpoint_address selects an independently configured interrupt IN pipe.
 * Only one transfer per pipe may be outstanding. All callers are serialized.
 */
bool usb_submit_interrupt_in(usb_device_t *device, uint8_t endpoint_address,
                             usb_interrupt_callback_t callback, void *context);

static inline uint16_t usb_read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

#endif
