#ifndef DESIGNWARE_I2C_H
#define DESIGNWARE_I2C_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Kernel-only, polled transactions for the exact Arrow Lake I2C2 controller. */
bool designware_i2c_probe(void *pci);
bool designware_i2c_transfer(unsigned address, const uint8_t *write, size_t write_size,
                             uint8_t *read, size_t read_size);
#endif
