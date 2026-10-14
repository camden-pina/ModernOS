#ifndef MODERNOS_IGC_H
#define MODERNOS_IGC_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool igc_probe(void *pci_header);
bool igc_present(void);
bool igc_link(void);
void igc_mac(uint8_t address[6]);
long igc_send(const void *frame, size_t length);
long igc_receive(void *frame, size_t capacity);
#endif
