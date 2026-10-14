#ifndef _HPET_H
#define _HPET_H

#include <stdint.h>

// Initialize the HPET timer via ACPI
_Bool hpet_init(void);

// Sleep for a given number of microseconds
void hpet_sleep_us(uint64_t microseconds);

// Sleep for a given number of milliseconds
void hpet_sleep_ms(uint64_t milliseconds);

// Get the current monotonic tick count
uint64_t hpet_get_ticks(void);

#endif
