/* Compare the production label against a wide rendering and verify clipping. */
#include "../../userland/desktop/render.c"
Desktop desk;
static uint32_t reference[512 * 160], actual[512 * 160];
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            printf("TEXT FAIL line=%d scale=%d font=%d: %s\n", __LINE__, desk.scale, (int)font,    \
                   #x);                                                                            \
            __builtin_trap();                                                                      \
        }                                                                                          \
    } while (0)
static void surface(uint32_t *pixels) {
    memset(pixels, 0, 512 * 160 * 4);
    desk.paint = pixels;
    desk.paint_stride = 512;
    desk.paint_x = desk.paint_y = 0;
    desk.width = 512;
    desk.height = 160;
    desk.clip = (Rect){0, 0, 512, 160};
}
int main(void) {
    const int scales[] = {100, 125, 150, 200};
    const char *strings[] = {"Files",     "UI-Validation", "Browser",
                             "Processes", "Workspace 3",   "caf\303\251"};
    for (int scale = 0; scale < 4; scale++) {
        desk.scale = scales[scale];
        desk.scale_index = scale;
        for (Font font = FONT_SMALL; font < FONT_COUNT; font++)
            for (unsigned i = 0; i < sizeof(strings) / sizeof(*strings); i++) {
                const char *text = strings[i];
                int measured = text_width(text, font);
                CHECK(measured > 0 && measured < 480);
                surface(reference);
                label(8, 4, text, font, 0xFFFFFF, 496);
                surface(actual);
                label(8, 4, text, font, 0xFFFFFF, measured);
                unsigned ink = 0;
                for (int y = 0; y < 160; y++)
                    for (int x = 0; x < 512; x++) {
                        uint32_t expected = x >= 8 && x < 8 + measured ? reference[y * 512 + x] : 0;
                        CHECK(actual[y * 512 + x] == expected);
                        ink += actual[y * 512 + x] != 0;
                    }
                CHECK(ink > 0);
                for (int width = 1; width < measured; width++) {
                    surface(actual);
                    label(8, 4, text, font, 0xFFFFFF, width);
                    for (int y = 0; y < 160; y++)
                        for (int x = 0; x < 512; x++)
                            if (x < 8 || x >= 8 + width)
                                CHECK(actual[y * 512 + x] == 0);
                    CHECK(desk.clip.x == 0 && desk.clip.w == 512);
                }
            }
    }
    puts("PASS production text rendering: final glyph at measured width, strict "
         "narrow-label/ellipsis clipping, all12 fonts and100/125/150/200% scales (ASan/UBSan)");
    return 0;
}
