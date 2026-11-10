#ifndef COAST_GLASS_FILTER_H
#define COAST_GLASS_FILTER_H
#include <stdint.h>
/* One exact, separable box pass. Replicate edge samples; running sums keep
   work O(pixels), independent of radius. Two H/V rounds approximate a Gaussian. */
static inline void glass_box_pass(const uint32_t *source, uint32_t *target, int width, int height,
                                  int radius, int vertical) {
    int lines = vertical ? width : height, length = vertical ? height : width,
        stride = vertical ? width : 1;
    unsigned count = (unsigned)(radius * 2 + 1);
    for (int line = 0; line < lines; line++) {
        int start = vertical ? line : line * width;
        uint32_t red = 0, green = 0, blue = 0;
        for (int i = -radius; i <= radius; i++) {
            int sample = i < 0 ? 0 : i >= length ? length - 1 : i;
            uint32_t p = source[start + sample * stride];
            red += (p >> 16) & 255;
            green += (p >> 8) & 255;
            blue += p & 255;
        }
        for (int i = 0; i < length; i++) {
            target[start + i * stride] = ((red + count / 2) / count << 16) |
                                         ((green + count / 2) / count << 8) |
                                         ((blue + count / 2) / count);
            int old = i - radius, newer = i + radius + 1;
            if (old < 0)
                old = 0;
            if (newer >= length)
                newer = length - 1;
            uint32_t a = source[start + old * stride], b = source[start + newer * stride];
            red += ((b >> 16) & 255) - ((a >> 16) & 255);
            green += ((b >> 8) & 255) - ((a >> 8) & 255);
            blue += (b & 255) - (a & 255);
        }
    }
}
static inline uint32_t glass_lerp4(uint32_t a, uint32_t b, unsigned fraction) {
    unsigned inverse = 4 - fraction;
    return ((((a & 0xff00ff) * inverse + (b & 0xff00ff) * fraction + 0x020002) >> 2) & 0xff00ff) |
           ((((a & 0x00ff00) * inverse + (b & 0x00ff00) * fraction + 0x000200) >> 2) & 0x00ff00);
}
#endif
