#ifndef _PCI_H
#define _PCI_H

#include <stdint.h>

// Standard PCI Configuration Space Header (Type 0)
typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t revision_id;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t cache_line_size;
    uint8_t latency_timer;
    uint8_t header_type;
    uint8_t bist;
    uint32_t bar[6];
    uint32_t cardbus_cis_ptr;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint32_t expansion_rom_base_addr;
    uint8_t capabilities_ptr;
    uint8_t reserved[7];
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t min_grant;
    uint8_t max_latency;
} __attribute__((packed)) pci_device_header_t;

// Bit 4 in the PCI Status register indicates if a capabilities list exists
#define PCI_STATUS_CAPABILITIES_LIST 0x10

// PCI Capability IDs
#define PCI_CAP_ID_PM 0x01 // Power Management
#define PCI_CAP_ID_MSI 0x05 // Message Signaled Interrupts
#define PCI_CAP_ID_MSIX 0x11 // MSI-X

// Generic PCI Capability Header
typedef struct {
    uint8_t id;
    uint8_t next;
} __attribute__((packed)) pci_cap_hdr_t;

// MSI Capability Structure
typedef struct {
    uint8_t id;
    uint8_t next;
    uint16_t msg_ctrl;
    uint32_t msg_addr_low;
    // The structure changes depending on whether the device supports 64-bit addressing.
    // We will handle this programmatically in the code.
} __attribute__((packed)) pci_msi_cap_t;

// Add this to include/drivers/pci/pci.h
_Bool pci_enable_msi(pci_device_header_t *pci_hdr, uint8_t lapic_id, uint8_t idt_vector);

void pci_init(void);
/* Borrowed kernel mapping of one MCFG function; NULL if no allocation covers it. */
void *pci_config_space(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function);

#endif
