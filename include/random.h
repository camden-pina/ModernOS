#ifndef MODERNOS_RANDOM_H
#define MODERNOS_RANDOM_H
#include <stddef.h>
#include <stdbool.h>

bool random_available(void);
long random_bytes(void *buffer, size_t size);
#endif
