#ifndef KERNEL_8250_H
#define KERNEL_8250_H

#include <kernel.h>
#include <stdint.h>

#define COM1_PORT 0x3F8
#define COM2_PORT 0x2F8
#define COM3_PORT 0x3E8
#define COM4_PORT 0x2E8

int serial_port_init(uint16_t port);
int serial_port_write_char(uint16_t port, char ch);
void serial_port_write(const char *str);

#endif