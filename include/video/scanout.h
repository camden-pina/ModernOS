#ifndef COAST_SCANOUT_H
#define COAST_SCANOUT_H
#include <drivers/pci/pci.h>
#include <boot/info.h>
#include <uapi/abi.h>

struct address_space;
void scanout_framebuffer(struct boot_framebuffer *);
void scanout_probe(pci_device_header_t *);
void scanout_caps(struct os_display_caps *);
bool scanout_streaming_copy(void);
void scanout_reset(void);
bool scanout_set_mode(uint32_t width, uint32_t height);
void scanout_mode_limits(struct os_display_mode *out);
bool scanout_present(const struct address_space *, const struct os_display_batch *);
#endif
