#ifndef KERNEL_STR_H
#define KERNEL_STR_H

#include <kernel.h>

typedef struct str {
    char *str;
    size_t len;
} str_t;

#endif