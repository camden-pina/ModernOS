#ifndef KERNEL_IO_H
#define KERNEL_IO_H

#include <stdint.h>

void outb(uint16_t port, uint8_t data);
uint8_t inb(uint16_t port);

void outw(uint16_t port, uint16_t data);
uint16_t inw(uint16_t port);

void outdw(uint16_t port, uint32_t data);
uint32_t indw(uint16_t port);

unsigned long long read_msr(unsigned long long index);
void write_msr(unsigned long long index, unsigned long long value);

unsigned char io_read_8(unsigned port);
void io_write_8(unsigned port, unsigned char val);

#endif