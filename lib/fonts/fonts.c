#include <font.h>
#include <stddef.h>

static const struct font_desc *fonts[] = {
    &font_vga_8x16,
};

#define NUM_FONTS (sizeof(fonts) / sizeof(fonts[0]))

const struct font_desc *find_font(const char *name) {
    if (!name)
        return NULL;

    for (size_t i = 0; i < NUM_FONTS; i++) {
        const char *a = fonts[i]->name;
        const char *b = name;

        while (*a && (*a == *b)) {
            a++;
            b++;
        }

        if (*a == '\0' && *b == '\0') {
            return fonts[i];
        }
    }
    return NULL;
}

#include <printk.h>

const struct font_desc *get_default_font(int xres, int yres) {
    // TODO:
    // Use xres and yres to dynamically select an appropriately sized font
    // based on display dimensions/DPI.
    (void)xres;
    (void)yres;

    pr_info("chk1");
    // pr_info("font_vga_8x16 %p", &font_vga_8x16);
    pr_info("chk2");
    /* Returns standard 8x16 console font by default */
    return &font_vga_8x16;
}
