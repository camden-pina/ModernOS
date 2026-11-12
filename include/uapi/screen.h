#ifndef COAST_SCREEN_ABI_H
#define COAST_SCREEN_ABI_H
#include <stdint.h>
/* C++-safe subset of the display query ABI used by Qt's platform plugin. */
#define COAST_SCREEN_SYSCALL 402
#define COAST_SCREEN_INFO 0
struct coast_screen_info {
    uint32_t version, width, height, format, max_pixels, reserved[3];
};
#endif
