#ifndef _LINUX_FONT_H
#define _LINUX_FONT_H

#include <stddef.h>
#include <stdint.h>

#define VGA8x16_IDX 0

struct font_desc {
    int idx;
    const char *name;
    int width;
    int height;
    const void *data;
    int pref;
};

extern const struct font_desc font_vga_8x16;

const struct font_desc *get_default_font(int xres, int yres);
const struct font_desc *find_font(const char *name);

#endif /* _LINUX_FONT_H */
