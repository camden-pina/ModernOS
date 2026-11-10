#include "glass_filter.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t input[47 * 43], actual[47 * 43], expect[47 * 43], work[47 * 43];
static uint32_t seed = 17;
static uint32_t random_color(void) {
    seed = seed * 1664525u + 1013904223u;
    return seed & 0xffffff;
}
static void naive(int w, int h, int radius, int vertical) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned r = 0, g = 0, b = 0, n = 2 * radius + 1;
            for (int i = -radius; i <= radius; i++) {
                int sx = vertical ? x : x + i, sy = vertical ? y + i : y;
                if (sx < 0)
                    sx = 0;
                if (sx >= w)
                    sx = w - 1;
                if (sy < 0)
                    sy = 0;
                if (sy >= h)
                    sy = h - 1;
                uint32_t c = input[sy * w + sx];
                r += (c >> 16) & 255;
                g += (c >> 8) & 255;
                b += c & 255;
            }
            expect[y * w + x] = ((r + n / 2) / n << 16) | ((g + n / 2) / n << 8) | (b + n / 2) / n;
        }
}
int main(void) {
    const int sizes[][2] = {{1, 1}, {2, 1}, {1, 7}, {5, 9}, {17, 11}, {47, 43}};
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(*sizes); k++)
        for (int radius = 1; radius <= 6; radius++)
            for (int vertical = 0; vertical < 2; vertical++) {
                int w = sizes[k][0], h = sizes[k][1];
                for (int i = 0; i < w * h; i++)
                    input[i] = random_color();
                naive(w, h, radius, vertical);
                glass_box_pass(input, actual, w, h, radius, vertical);
                assert(!memcmp(actual, expect, w * h * 4));
            }
    for (int i = 0; i < 47 * 43; i++)
        input[i] = 0x557799;
    glass_box_pass(input, work, 47, 43, 3, 0);
    glass_box_pass(work, actual, 47, 43, 3, 1);
    for (int i = 0; i < 47 * 43; i++)
        assert(actual[i] == 0x557799);
    memset(input, 0, sizeof(input));
    input[21 * 47 + 23] = 0xffffff;
    glass_box_pass(input, work, 47, 43, 3, 0);
    glass_box_pass(work, actual, 47, 43, 3, 1);
    assert(actual[21 * 47 + 23] > 0 && actual[21 * 47 + 23] < 0x111111 &&
           actual[21 * 47 + 24] == actual[21 * 47 + 23]);
    assert(glass_lerp4(0x000000, 0xffffff, 0) == 0 &&
           glass_lerp4(0x000000, 0xffffff, 2) == 0x808080);
    puts("PASS production glass filter:72 exact naive-oracle comparisons, radius1..6, "
         "single-pixel/odd extents, replicated edges, constant preservation, impulse spread and "
         "bilinear interpolation (ASan/UBSan).");
}
