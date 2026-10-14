#include <drivers/usb/usb.h>
#include <drivers/usb/xhci.h>
#include <drivers/usb/usb_keyboard.h>
#include <drivers/usb/usb_mouse.h>
#include <drivers/usb/usb_hid.h>
#include <printk.h>
#include <string.h>

static bool initialized;
static usb_device_t *devices[USB_MAX_DEVICES + 1];
/* Serialized enumeration permits one shared CPU-side descriptor buffer. */
static uint8_t descriptor_buffer[USB_DESCRIPTOR_BUFFER_SIZE];

bool usb_control_transfer(usb_device_t *device, const usb_setup_packet_t *setup, void *data,
                          uint16_t *actual) {
    if (actual)
        *actual = 0;
    if (!device || !setup || !device->connected || !device->host_ops ||
        !device->host_ops->control || (setup->length && !data))
        return false;
    return device->host_ops->control(device, setup, data, actual);
}

bool usb_get_descriptor(usb_device_t *device, uint8_t type, uint8_t index, void *data,
                        uint16_t length, uint16_t *actual) {
    usb_setup_packet_t setup = {.request_type = 0x80,
                                .request = USB_REQUEST_GET_DESCRIPTOR,
                                .value = (uint16_t)((type << 8) | index),
                                .index = 0,
                                .length = length};
    return usb_control_transfer(device, &setup, data, actual);
}

bool usb_set_configuration(usb_device_t *device, uint8_t configuration) {
    if (!device || !device->connected || !configuration)
        return false;
    if (device->configuration)
        return device->configuration == configuration;
    usb_setup_packet_t setup = {
        .request_type = 0, .request = USB_REQUEST_SET_CONFIGURATION, .value = configuration};
    if (!usb_control_transfer(device, &setup, NULL, NULL))
        return false;
    device->configuration = configuration;
    return true;
}

bool usb_configure_interrupt_in(usb_device_t *device, const usb_endpoint_t *endpoint) {
    return device && device->connected && endpoint && device->host_ops &&
           device->host_ops->configure_interrupt_in &&
           device->host_ops->configure_interrupt_in(device, endpoint);
}

bool usb_submit_interrupt_in(usb_device_t *device, uint8_t endpoint_address,
                             usb_interrupt_callback_t callback, void *context) {
    return device && device->connected && callback && device->host_ops &&
           device->host_ops->submit_interrupt_in &&
           device->host_ops->submit_interrupt_in(device, endpoint_address, callback, context);
}

static void device_disconnected(usb_device_t *device) {
    if (!device)
        return;
    device->connected = false;
    usb_keyboard_disconnect(device);
    usb_mouse_disconnect(device);
}

static void close_device(usb_device_t *device) {
    if (device->host_ops && device->host_ops->close)
        device->host_ops->close(device);
    devices[device->id] = NULL;
}

static bool enumerate_device(usb_device_t *device) {
    uint8_t *buffer = descriptor_buffer;
    uint16_t actual = 0;
    if (!usb_get_descriptor(device, USB_DESCRIPTOR_DEVICE, 0, buffer, 8, &actual) || actual != 8 ||
        buffer[0] != 18 || buffer[1] != USB_DESCRIPTOR_DEVICE)
        return false;
    uint16_t mps = buffer[7];
    uint8_t speed = device->speed;
    if ((speed == USB_SPEED_LOW && mps != 8) || (speed == USB_SPEED_HIGH && mps != 64) ||
        (speed == USB_SPEED_FULL && mps != 8 && mps != 16 && mps != 32 && mps != 64)) {
        kprintf("USB device %u: invalid EP0 packet size %u\n\r", device->id, mps);
        return false;
    }
    if (!device->host_ops->set_ep0_packet_size(device, mps))
        return false;
    if (!usb_get_descriptor(device, USB_DESCRIPTOR_DEVICE, 0, buffer, 18, &actual) ||
        actual != 18 || buffer[0] != 18 || buffer[1] != USB_DESCRIPTOR_DEVICE)
        return false;
    device->vendor_id = usb_read_le16(buffer + 8);
    device->product_id = usb_read_le16(buffer + 10);
    kprintf("USB device: vendor %04x product %04x\n\r", device->vendor_id, device->product_id);
    if (buffer[4] == 9) {
        kprintf("USB: hub support is not implemented\n\r");
        return false;
    }

    uint8_t configurations = buffer[17];
    for (unsigned config = 0; config < configurations; ++config) {
        if (!usb_get_descriptor(device, USB_DESCRIPTOR_CONFIGURATION, (uint8_t)config, buffer, 9,
                                &actual) ||
            actual != 9 || buffer[0] != 9 || buffer[1] != USB_DESCRIPTOR_CONFIGURATION)
            return false;
        uint16_t total = usb_read_le16(buffer + 2);
        if (total < 9 || total > USB_DESCRIPTOR_BUFFER_SIZE) {
            kprintf("USB: unsupported configuration size %u (limit 4096)\n\r", total);
            continue;
        }
        if (!usb_get_descriptor(device, USB_DESCRIPTOR_CONFIGURATION, (uint8_t)config, buffer,
                                total, &actual) ||
            actual != total)
            return false;

        /* Collect interfaces before class probes reuse descriptor storage.
         * Configure all candidate pipes first, then select the configuration
         * exactly once. A second SET_CONFIGURATION would reset live pipes.
         */
        usb_hid_interface_t interfaces[USB_HID_MAX_INTERFACES];
        size_t count = 0;
        if (!usb_hid_parse_configuration(buffer, total, speed, interfaces, USB_HID_MAX_INTERFACES,
                                         &count))
            return false;
        if (!count)
            continue;
        bool ready[USB_HID_MAX_INTERFACES] = {0};
        unsigned candidates = 0;
        for (size_t i = 0; i < count && xhci_is_running(); ++i) {
            usb_hid_interface_t *interface = &interfaces[i];
            bool boot_keyboard = interface->subclass == USB_HID_SUBCLASS_BOOT &&
                                 interface->protocol == USB_HID_PROTOCOL_KEYBOARD;
            if ((!boot_keyboard && !usb_mouse_supports_interface(interface)) ||
                (boot_keyboard && interface->endpoint.max_packet_size < 8))
                continue;
            ready[i] = usb_configure_interrupt_in(device, &interface->endpoint);
            candidates += ready[i];
        }
        if (!xhci_is_running())
            return false;
        if (!candidates)
            continue;
        if (!usb_set_configuration(device, interfaces[0].configuration))
            return false;
        unsigned bound = 0;
        for (size_t i = 0; i < count && device->connected && xhci_is_running(); ++i) {
            if (!ready[i])
                continue;
            usb_hid_interface_t *interface = &interfaces[i];
            if (interface->subclass == USB_HID_SUBCLASS_BOOT &&
                interface->protocol == USB_HID_PROTOCOL_KEYBOARD) {
                usb_keyboard_interface_t keyboard_interface = {
                    .configuration = interface->configuration,
                    .interface_number = interface->interface_number,
                    .endpoint = interface->endpoint};
                bound += usb_keyboard_bind(device, &keyboard_interface);
            } else {
                bound += usb_mouse_bind(device, interface);
            }
        }
        /* Endpoints are now live. Switching configurations requires explicit
         * teardown, so stop after this attempted configuration.
         */
        return bound != 0 && device->connected && xhci_is_running();
    }
    kprintf("USB port %u: no supported class driver found\n\r", device->root_port);
    return false;
}

bool usb_init(pci_device_header_t *pci_hdr) {
    if (initialized) {
        kprintf("USB: initialization already attempted\n\r");
        return usb_is_running();
    }
    initialized = true;
    memset(devices, 0, sizeof(devices));
    usb_keyboard_init();
    usb_mouse_init();
    if (!xhci_init_controller(pci_hdr))
        return false;

    for (unsigned port = 1; port <= xhci_root_port_count() && xhci_is_running(); ++port) {
        usb_device_t *device = xhci_open_root_device((uint8_t)port);
        if (!device)
            continue;
        device->on_disconnect = device_disconnected;
        devices[device->id] = device;
        if (!enumerate_device(device))
            close_device(device);
    }
    kprintf("USB: %u keyboard interface(s), %u mouse interface(s) ready\n\r", usb_keyboard_count(),
            usb_mouse_count());
    return usb_is_running();
}

void usb_poll(void) {
    if (usb_is_running())
        xhci_poll();
}

bool usb_is_running(void) {
    return initialized && xhci_is_running();
}
