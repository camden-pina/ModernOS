#include <drivers/usb/usb_mouse.h>
#include <drivers/usb/hid_mouse.h>
#include <printk.h>
#include <string.h>

typedef struct {
    bool attached;
    bool boot_protocol;
    usb_device_t *device;
    uint8_t endpoint_address;
    uint16_t source_id;
    uint32_t buttons;
    hid_mouse_layout_t layout;
} usb_mouse_binding_t;

static usb_mouse_binding_t bindings[USB_MOUSE_MAX_BINDINGS];
static uint64_t invalid_reports;
/* Enumeration is serialized. Runtime completion never uses this buffer. */
static uint8_t report_descriptor[USB_DESCRIPTOR_BUFFER_SIZE];

_Static_assert(USB_MAX_DEVICES * 16u <= MOUSE_MAX_SOURCES, "USB mouse source IDs");

uint16_t usb_mouse_source_id(uint8_t device_id, uint8_t endpoint_address) {
    if (!device_id || device_id > USB_MAX_DEVICES || !(endpoint_address & 0x80u) ||
        !(endpoint_address & 15u) || (endpoint_address & 0x70u))
        return 0;
    return (uint16_t)(((unsigned)device_id - 1u) * 16u + (endpoint_address & 15u));
}

bool usb_mouse_supports_interface(const usb_hid_interface_t *interface) {
    if (!interface)
        return false;
    bool boot_mouse = interface->subclass == USB_HID_SUBCLASS_BOOT &&
                      interface->protocol == USB_HID_PROTOCOL_MOUSE;
    bool generic_hid = interface->subclass == 0 && interface->protocol == 0;
    return (boot_mouse && interface->endpoint.max_packet_size >= 3) ||
           (generic_hid && interface->endpoint.max_packet_size != 0);
}

void usb_mouse_init(void) {
    memset(bindings, 0, sizeof(bindings));
    invalid_reports = 0;
    mouse_init();
}

static void disconnect_binding(usb_mouse_binding_t *binding) {
    if (!binding->attached)
        return;
    binding->attached = false;
    binding->buttons = 0;
    mouse_detach(binding->source_id);
}

void usb_mouse_disconnect(usb_device_t *device) {
    if (!device)
        return;
    for (unsigned i = 0; i < USB_MOUSE_MAX_BINDINGS; ++i)
        if (bindings[i].device == device)
            disconnect_binding(&bindings[i]);
}

static void mouse_report_complete(usb_device_t *device, usb_transfer_status_t status,
                                  const uint8_t *data, size_t length, void *context) {
    usb_mouse_binding_t *binding = context;
    if (!binding || !binding->attached || binding->device != device)
        return;
    if (status == USB_TRANSFER_ERROR) {
        kprintf("USB mouse %u endpoint %u: interrupt transfer failed\n\r", device->id,
                binding->endpoint_address);
        disconnect_binding(binding);
        return;
    }
    hid_mouse_sample_t sample;
    hid_mouse_decode_result_t result = hid_mouse_decode(&binding->layout, data, length, &sample);
    if (result == HID_MOUSE_DECODED) {
        binding->buttons = (binding->buttons & ~sample.button_mask) | sample.buttons;
        mouse_publish(binding->source_id, sample.dx, sample.dy, sample.wheel, sample.hwheel,
                      binding->buttons);
    } else if (result == HID_MOUSE_INVALID) {
        ++invalid_reports;
    }
    /* Short reports are normal for interrupt endpoints. The parser checks
     * the selected report's own length rather than requiring endpoint MPS.
     * Copy/decode is finished before rearming can overwrite the DMA buffer.
     */
    if (!device->connected ||
        !usb_submit_interrupt_in(device, binding->endpoint_address, mouse_report_complete, binding))
        disconnect_binding(binding);
}

bool usb_mouse_bind(usb_device_t *device, const usb_hid_interface_t *interface) {
    if (!device || !device->connected || !usb_mouse_supports_interface(interface))
        return false;
    uint16_t source = usb_mouse_source_id(device->id, interface->endpoint.address);
    if (!source)
        return false;
    usb_mouse_binding_t *binding = NULL;
    for (unsigned i = 0; i < USB_MOUSE_MAX_BINDINGS; ++i) {
        if (bindings[i].attached && bindings[i].source_id == source)
            return false;
        if (!bindings[i].attached && !binding)
            binding = &bindings[i];
    }
    if (!binding) {
        kprintf("USB mouse: interface binding limit reached\n\r");
        return false;
    }
    memset(binding, 0, sizeof(*binding));
    bool boot_mouse = interface->subclass == USB_HID_SUBCLASS_BOOT &&
                      interface->protocol == USB_HID_PROTOCOL_MOUSE;
    if (!usb_configure_interrupt_in(device, &interface->endpoint) ||
        !usb_set_configuration(device, interface->configuration))
        return false;
    bool parsed = false;
    if (interface->report_descriptor_length &&
        interface->report_descriptor_length <= sizeof(report_descriptor)) {
        uint16_t actual = 0;
        if (!usb_hid_get_report_descriptor(device, interface, report_descriptor, &actual))
            return false; /* USB transport errors are not parser fallbacks. */
        if (actual == interface->report_descriptor_length)
            parsed = hid_mouse_parse_descriptor(report_descriptor, actual, &binding->layout) &&
                     hid_mouse_layout_fits(&binding->layout, interface->endpoint.max_packet_size);
    }
    if (parsed) {
        /* Non-boot interfaces already use report protocol and must not
         * receive SET_PROTOCOL, which is only defined for boot interfaces.
         */
        if (boot_mouse && !usb_hid_set_protocol(device, interface->interface_number, true))
            return false;
    } else {
        if (!boot_mouse)
            return false;
        hid_mouse_boot_layout(&binding->layout);
        if (!usb_hid_set_protocol(device, interface->interface_number, false))
            return false;
        binding->boot_protocol = true;
    }
    if (!device->connected || !mouse_attach(source))
        return false;
    binding->device = device;
    binding->endpoint_address = interface->endpoint.address;
    binding->source_id = source;
    binding->attached = true;
    if (!usb_submit_interrupt_in(device, binding->endpoint_address, mouse_report_complete,
                                 binding)) {
        disconnect_binding(binding);
        return false;
    }
    kprintf("USB mouse ready: port %u device %u interface %u endpoint %u source %u (%s)\n\r",
            device->root_port, device->id, interface->interface_number, interface->endpoint.address,
            source, binding->boot_protocol ? "boot" : "report");
    return true;
}

uint32_t usb_mouse_count(void) {
    if (!usb_is_running())
        return 0;
    uint32_t count = 0;
    for (unsigned i = 0; i < USB_MOUSE_MAX_BINDINGS; ++i)
        if (bindings[i].attached && bindings[i].device->connected)
            ++count;
    return count;
}

uint64_t usb_mouse_invalid_reports(void) {
    return invalid_reports;
}
