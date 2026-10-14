#include <usb_input.h>
#include <drivers/usb/usb.h>
#include <drivers/usb/usb_keyboard.h>
#include <drivers/usb/usb_platform.h>
#include <printk.h>
#include <tty.h>
#include <video/display.h>

static pci_device_header_t *xhci_controller;
static bool start_attempted;
static bool controller_was_running;
static uint32_t last_dropped_events;

bool kernel_usb_register_xhci(pci_device_header_t *controller) {
    if (!controller || start_attempted || xhci_controller)
        return false;
    xhci_controller = controller;
    return true;
}

bool kernel_usb_input_start(void) {
    if (start_attempted)
        return usb_is_running();
    start_attempted = true;

    if (!xhci_controller) {
        pr_warn("USB: no xHCI controller registered by PCI discovery\n");
        return false;
    }
    if (!usb_platform_dma_ready()) {
        pr_warn("USB: kernel HHDM response unavailable\n");
        return false;
    }
    if (!usb_platform_delay_ms(0)) {
        pr_warn("USB: ACPI PM timer unavailable or not advancing\n");
        return false;
    }

    /* usb_init() initializes xHCI exactly once, discovers root-port devices,
     * selects HID boot-keyboard interfaces, and queues interrupt-IN reports.
     */
    if (!usb_init(xhci_controller)) {
        pr_warn("USB: initialization failed; see the preceding driver messages\n");
        return false;
    }

    controller_was_running = true;
    uint32_t keyboards = usb_keyboard_count();
    pr_info("USB: xHCI running, %u boot keyboard(s) available\n", keyboards);
    if (!keyboards)
        pr_warn("USB: no supported keyboard found; connect directly before boot\n");
    return true;
}

void kernel_usb_input_poll(void) {
    usb_poll();

    usb_keyboard_event_t event;
    while (usb_keyboard_read_event(&event)) {
        if (display_hid(event.usage, event.ascii, event.modifiers, event.pressed))
            continue;
        if (event.pressed && !event.ascii) {
            const char *sequence = NULL;
            switch (event.usage) {
            case 0x4f:
                sequence = "\033[C";
                break;
            case 0x50:
                sequence = "\033[D";
                break;
            case 0x51:
                sequence = "\033[B";
                break;
            case 0x52:
                sequence = "\033[A";
                break;
            case 0x4a:
                sequence = "\033[H";
                break;
            case 0x4d:
                sequence = "\033[F";
                break;
            case 0x4c:
                sequence = "\033[3~";
                break;
            case 0x4b:
                sequence = "\033[5~";
                break;
            case 0x4e:
                sequence = "\033[6~";
                break;
            }
            if (sequence)
                while (*sequence)
                    tty_input(*sequence++);
        }
        if (event.pressed && event.ascii) {
            char c = event.ascii;
            if ((event.modifiers & (USB_MOD_LEFT_CTRL | USB_MOD_RIGHT_CTRL)) &&
                ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
                c &= 31;
            tty_input(c);
        }
    }

    uint32_t dropped = usb_keyboard_dropped_events();
    if (dropped != last_dropped_events) {
        pr_warn("USB: keyboard queue overflow, total dropped events %u\n", dropped);
        last_dropped_events = dropped;
    }
    if (controller_was_running && !usb_is_running()) {
        controller_was_running = false;
        pr_warn("USB: controller stopped after an error\n");
    }
}
