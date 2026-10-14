#ifndef USB_PLATFORM_H
#define USB_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/pci/pci.h>

#define USB_DMA_PAGE_SIZE 4096u

/* This implementation runs on one boot CPU with four-level paging.
 * ACPI tables and PMM must be initialized before pci_init().
 * PCI headers must be live, UC-mapped ECAM headers, not copied structures.
 */
bool usb_platform_prepare_pci(pci_device_header_t *pci_hdr);
void *usb_platform_map_mmio(uint64_t physical_base, size_t byte_length);
bool usb_platform_delay_ms(uint32_t milliseconds);
bool usb_platform_time_ms(uint64_t *milliseconds);
void usb_platform_relax(void);

/* Physical addresses are DMA addresses. Requires disabled IOMMU translation
 * or an existing identity DMA domain; these hooks do not configure an IOMMU.
 */
bool usb_platform_dma_ready(void);
uint64_t usb_platform_alloc_dma(size_t pages, bool supports_64bit, void **virtual_out);

#endif
