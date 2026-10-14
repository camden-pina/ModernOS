#ifndef XHCI_H
#define XHCI_H

#include <drivers/usb/usb.h>

/* Host controller transport interface, called by USB core in usb.c.
 * Kernel code should call usb_init()/usb_poll() instead.
 * Initialization starts the controller only; USB core enumerates its ports.
 */
bool xhci_init_controller(pci_device_header_t *pci_hdr);
void xhci_poll(void);
bool xhci_is_running(void);
uint8_t xhci_root_port_count(void);
usb_device_t *xhci_open_root_device(uint8_t root_port);

#endif
