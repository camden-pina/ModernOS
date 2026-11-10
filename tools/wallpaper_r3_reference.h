/* Preserved r3 arithmetic oracle, extracted before changes. */
static uint32_t reference_color_mix(uint32_t a, uint32_t b, unsigned factor) {
    factor = MIN(factor, 255);
    unsigned inverse = 255 - factor;
    return ((((a & 0xff00ff) * inverse + (b & 0xff00ff) * factor + 0x800080) >> 8) & 0xff00ff) |
           ((((a & 0x00ff00) * inverse + (b & 0x00ff00) * factor + 0x008000) >> 8) & 0x00ff00);
}

static void reference_procedural(uint32_t *pixels, int width, int height, int index) {
    uint32_t start = THEME_WALLPAPER_DARK, mid = THEME_WALLPAPER_MID, end = THEME_WALLPAPER_TEAL,
             glow = THEME_WALLPAPER_GLOW, lavender = 0x777C97;
    if (index == 1) {
        start = 0x30344E;
        mid = 0x414663;
        end = 0x344353;
        glow = 0x686D8A;
        lavender = 0x7C7894;
    }

    if (index == 2) {
        start = 0x273E41;
        mid = 0x314B4D;
        end = 0x2A4147;
        glow = 0x597E78;
        lavender = 0x78928D;
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int u = x * 1024 / width, v = y * 1024 / height, t = (u + v) / 2;
            if (index == 0) {
                /* Evaluate at physical-pixel centers with subpixel precision.
                   A 1024-step coordinate grid visibly stair-stepped at HiDPI. */
                int64_t ux = ((int64_t)x * 2 + 1) * 32768 / width;
                int64_t vy = ((int64_t)y * 2 + 1) * 32768 / height;
                uint32_t c = reference_color_mix(0xA8D9E9, 0x5088C3, vy / (64 * 6));
                int64_t delta = ux - 380 * 64;
                int64_t curve = 620 * 64 - ux / 4 + delta * delta / (2300 * 64);
                const int offset[] = {-130, -20, 100, 255};
                const uint32_t colors[] = {0x829BC9, 0xEAA2AB, 0xD45C8A, 0x243E8C};
                for (int band = 0; band < 4; band++) {
                    int64_t distance = vy - curve - offset[band] * 64;
                    if (distance > -640)
                        c = reference_color_mix(c, colors[band], clampi((distance + 640) * 255 / 1280, 0, 255));
                }
                c = reference_color_mix(c, 0x12285B, clampi((vy - 700 * 64) / (3 * 64), 0, 100));
                pixels[(size_t)y * width + x] = c;
                continue;
            }
            uint32_t c =
                t < 512 ? reference_color_mix(start, mid, t / 2) : reference_color_mix(mid, end, (t - 512) / 2);
            int dx = u - 880, dy = v + 60;
            unsigned d = (dx * dx + dy * dy) / 512;
            unsigned a = 50 * 1024 / (1024 + d * d / 128);
            c = reference_color_mix(c, glow, a);

            dx = u + 200;
            dy = v - 960;
            d = (dx * dx + dy * dy) / 640;
            a = 37 * 1024 / (1024 + d * d / 180);
            c = reference_color_mix(c, lavender, a);

            dx = u - 480;
            dy = v - 640;
            d = (dx * dx + dy * dy) / 360;
            a = 9 * 1024 / (1024 + d * d / 120);
            c = reference_color_mix(c, 0x9CACAD, a);

            unsigned noise = ((unsigned)x * 1664525u + (unsigned)y * 1013904223u) >> 30;
            unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            if (noise == 0) {
                r += r < 255;
                g += g < 255;
                b += b < 255;
            }
            pixels[(size_t)y * width + x] = (r << 16) | (g << 8) | b;
        }
    }
}

static uint32_t reference_over(uint32_t source, uint32_t background) {
    unsigned alpha = source >> 24;
    if (alpha == 255)
        return source & 0xffffff;
    uint32_t color = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        unsigned a = (source >> shift) & 255, b = (background >> shift) & 255;
        color |= ((a * alpha + b * (255 - alpha) + 127) / 255) << shift;
    }
    return color;
}

static uint32_t reference_interpolate(uint32_t a, uint32_t b, unsigned factor) {
    uint32_t color = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        unsigned av = (a >> shift) & 255, bv = (b >> shift) & 255;
        color |= ((av * (65536 - factor) + bv * factor + 32768) >> 16) << shift;
    }
    return color;
}

static void reference_compose(uint32_t *out, int width, int height, Image *image, int mode, uint32_t bg) {
    int iw = image->width, ih = image->height, dw = width, dh = height;
    bool wide = (int64_t)iw * height >= (int64_t)ih * width;
    if (mode == WALLPAPER_CENTER) {
        dw = iw;
        dh = ih;
    } else if (mode == WALLPAPER_FILL) {
        if (wide)
            dw = ((int64_t)iw * height + ih - 1) / ih;
        else
            dh = ((int64_t)ih * width + iw - 1) / iw;
    } else if (mode == WALLPAPER_FIT) {
        if (wide)
            dh = MAX(1, (int64_t)ih * width / iw);
        else
            dw = MAX(1, (int64_t)iw * height / ih);
    }

    int left = (width - dw) / 2, top = (height - dh) / 2;
    for (int y = 0; y < height; y++) {
        int64_t fy = ((int64_t)(y - top) * 2 + 1) * ih * 32768 / dh - 32768;
        fy = MAX(0, MIN(fy, (int64_t)(ih - 1) * 65536));
        int sy = fy >> 16, next_y = MIN(sy + 1, ih - 1);
        for (int x = 0; x < width; x++) {
            uint32_t color = bg;
            if (x >= left && x < left + dw && y >= top && y < top + dh) {
                int64_t fx = ((int64_t)(x - left) * 2 + 1) * iw * 32768 / dw - 32768;
                fx = MAX(0, MIN(fx, (int64_t)(iw - 1) * 65536));
                int sx = fx >> 16, next_x = MIN(sx + 1, iw - 1);
                uint32_t a = reference_over(image->pixels[(size_t)sy * iw + sx], bg),
                         b = reference_over(image->pixels[(size_t)sy * iw + next_x], bg),
                         c = reference_over(image->pixels[(size_t)next_y * iw + sx], bg),
                         d = reference_over(image->pixels[(size_t)next_y * iw + next_x], bg);
                color = reference_interpolate(reference_interpolate(a, b, fx & 65535), reference_interpolate(c, d, fx & 65535),
                                    fy & 65535);
            }

            out[(size_t)y * width + x] = color;
        }
    }
}

