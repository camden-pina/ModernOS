#ifndef KERNEL_USB_INPUT_H
#define KERNEL_USB_INPUT_H

#include <stdbool.h>
#include <drivers/pci/pci.h>

/* PCI enumeration registers the first xHCI ECAM header; no hardware access
 * occurs here. The mapping must remain valid through USB startup.
 */
bool kernel_usb_register_xhci(pci_device_header_t *controller);

/* Matches the current kernel.c call. Timing/DMA/MMIO hooks remain owned by
 * the existing usb_platform.c; this bridge supplies no duplicate definitions.
 */
bool kernel_usb_input_start(void);

/* Serialized main-loop polling and existing keyboard console consumption.
 * Mouse samples remain queued for mouse_read_event() / mouse_read_events().
 */
void kernel_usb_input_poll(void);

#endif
