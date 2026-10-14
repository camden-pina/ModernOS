#ifndef MODERNOS_LWIP_STDIO_H
#define MODERNOS_LWIP_STDIO_H

/* lwIP only needs bounded formatting here. Use the kernel's formatter, never
 * host libc declarations or file/stdio objects in a freestanding build. */
#include <printk.h>
#endif
