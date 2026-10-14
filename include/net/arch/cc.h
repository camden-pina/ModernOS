#ifndef MODERNOS_LWIP_CC_H
#define MODERNOS_LWIP_CC_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <printk.h>
#include <panic.h>

typedef intptr_t ssize_t;
#define SSIZE_MAX INTPTR_MAX
#define BYTE_ORDER 1234
#define LWIP_NO_CTYPE_H 1
#define LWIP_NO_UNISTD_H 1
#define LWIP_NO_STDINT_H 0
#define LWIP_NO_INTTYPES_H 1
#define X8_F "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"
#define LWIP_NO_LIMITS_H 0
#define LWIP_PLATFORM_DIAG(args) do { kprintf args; } while (0)
#define LWIP_PLATFORM_ASSERT(message) panic("lwIP: %s", message)
#define LWIP_RAND() network_random()

uint32_t network_random(void);
#endif
