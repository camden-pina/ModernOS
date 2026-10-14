#ifndef MODERNOS_WALLPAPER_PROTOCOL_H
#define MODERNOS_WALLPAPER_PROTOCOL_H
#include <stdint.h>

/* Private decoder pipe: a header followed by width * height ARGB8888 pixels.
   The desktop owns composition; the decoder never opens a display device. */
#define WALLPAPER_MAGIC 0x3150574dU
#define WALLPAPER_MAX_EDGE 8192U
#define WALLPAPER_MAX_PIXELS (16U * 1024U * 1024U)
#define WALLPAPER_MAX_FILE (64U * 1024U * 1024U)

struct wallpaper_packet {
    uint32_t magic, width, height;
    int32_t error;
};

_Static_assert(sizeof(struct wallpaper_packet) == 16, "wallpaper decoder header");
#endif
