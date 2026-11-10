#ifndef COAST_POINTER_CAPABILITIES_H
#define COAST_POINTER_CAPABILITIES_H
#include <stdint.h>
#define COAST_SYSCALL_POINTER_INFO 416
#define OS_POINTER_MOUSE (1u << 0)
#define OS_POINTER_TOUCHPAD (1u << 1)
#define OS_POINTER_RELATIVE (1u << 2)
#define OS_POINTER_WHEEL (1u << 3)
#define OS_POINTER_HWHEEL (1u << 4)
struct os_pointer_inventory {
    uint32_t version, size;
    uint32_t mice, relative_touchpads, unsupported_touchpads;
    uint32_t reserved[3];
};
#endif
