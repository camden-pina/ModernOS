#ifndef COAST_E1000_H
#define COAST_E1000_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool e1000_probe(void *pci_header);
bool e1000_present(void);
int e1000_power_stop(void);
bool e1000_link(void);
void e1000_mac(uint8_t address[6]);
long e1000_send(const void *frame, size_t length);
long e1000_receive(void *frame, size_t capacity);
#endif
