#include "desktop.h"
#include <wallpaper_protocol.h>

#define PREVIEW_WIDTH 192
#define PREVIEW_HEIGHT 112

typedef struct {
    uint32_t *pixels;
    int width, height;
} Image;

static Image active, previews[WALLPAPER_COUNT];
static char active_path[OS_PATH_MAX];
static bool generated;
static int generated_index, generated_mode, generated_background, generated_width, generated_height;
const char *const wallpaper_names[WALLPAPER_COUNT] = {
    "Mineral", "Dusk", "Moss", "Alpine Lake", "Desert Sunset", "Choose image..."};
const uint32_t wallpaper_backgrounds[3] = {0x18252D, 0x000000, 0xF1F3F0};
static const char *const paths[] = {"/usr/share/wallpapers/alpine-lake.png",
                                    "/usr/share/wallpapers/desert-sunset.png"};

static void release_image(Image *image) {
    deallocate(image->pixels, (size_t)image->width * image->height * 4);
    memset(image, 0, sizeof(*image));
}

static int receive(int fd, void *buffer, size_t bytes) {
    unsigned char *p = buffer;
    while (bytes) {
        long n = read(fd, p, bytes);
        if (n == -E_INTR)
            continue;
        if (n <= 0)
            return n ? n : -E_IO;
        p += n;
        bytes -= n;
    }

    return 0;
}

static int load_image(const char *path, Image *image) {
    struct os_stat st;
    int error = stat(path, &st);
    if (error)

        return error;
    if ((st.mode & 0170000) != S_IFREG || st.size < 8 || st.size > WALLPAPER_MAX_FILE)
        return -E_INVAL;
    int streams[2];
    error = call2(SYS_pipe2, streams, O_CLOEXEC);
    if (error)
        return error;
    int null = open("/dev/null", O_RDWR | O_CLOEXEC, 0);
    if (null < 0) {
        close(streams[0]);
        close(streams[1]);
        return null;
    }

    char *args[] = {"/bin/wallpaper-decode", (char *)path, NULL};
    struct os_spawn_io spec = {.fd = {null, streams[1], null}, .cwd = (uintptr_t)desk.home};
    int pid = call3(SYS_spawn_io, args[0], args, &spec);
    close(null);
    close(streams[1]);
    if (pid < 0) {
        close(streams[0]);
        return pid;
    }

    struct wallpaper_packet packet;
    error = receive(streams[0], &packet, sizeof(packet));
    if (!error) {
        if (packet.magic != WALLPAPER_MAGIC || packet.error > 0)
            error = -E_IO;
        else if (packet.error)
            error = packet.error;
        else if (!packet.width || !packet.height || packet.width > WALLPAPER_MAX_EDGE ||
                 packet.height > WALLPAPER_MAX_EDGE ||
                 (uint64_t)packet.width * packet.height > WALLPAPER_MAX_PIXELS)
            error = -E_INVAL;
    }

    if (!error) {
        image->width = packet.width;
        image->height = packet.height;
        size_t bytes = (size_t)packet.width * packet.height * 4;
        image->pixels = allocate(bytes);
        error = image->pixels ? receive(streams[0], image->pixels, bytes) : -E_NOMEM;
    }

    close(streams[0]);
    if (error)
        call2(SYS_kill, pid, 9);

    int status = 0, waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited == -E_INTR);
    if (!error && (waited < 0 || status))
        error = -E_IO;

    if (error)
        release_image(image);
    return error;
}

static uint32_t color_mix(uint32_t a, uint32_t b, unsigned factor) {
    factor = MIN(factor, 255);
    unsigned inverse = 255 - factor;
    return ((((a & 0xff00ff) * inverse + (b & 0xff00ff) * factor + 0x800080) >> 8) & 0xff00ff) |
           ((((a & 0x00ff00) * inverse + (b & 0x00ff00) * factor + 0x008000) >> 8) & 0x00ff00);
}

static void procedural(uint32_t *pixels, int width, int height, int index) {
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
            uint32_t c =
                t < 512 ? color_mix(start, mid, t / 2) : color_mix(mid, end, (t - 512) / 2);
            int dx = u - 880, dy = v + 60;
            unsigned d = (dx * dx + dy * dy) / 512;
            unsigned a = 50 * 1024 / (1024 + d * d / 128);
            c = color_mix(c, glow, a);

            dx = u + 200;
            dy = v - 960;
            d = (dx * dx + dy * dy) / 640;
            a = 37 * 1024 / (1024 + d * d / 180);
            c = color_mix(c, lavender, a);

            dx = u - 480;
            dy = v - 640;
            d = (dx * dx + dy * dy) / 360;
            a = 9 * 1024 / (1024 + d * d / 120);
            c = color_mix(c, 0x9CACAD, a);

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

static uint32_t over(uint32_t source, uint32_t background) {
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

static uint32_t interpolate(uint32_t a, uint32_t b, unsigned factor) {
    uint32_t color = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        unsigned av = (a >> shift) & 255, bv = (b >> shift) & 255;
        color |= ((av * (65536 - factor) + bv * factor + 32768) >> 16) << shift;
    }
    return color;
}

static void compose(uint32_t *out, int width, int height, Image *image, int mode, uint32_t bg) {
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
                uint32_t a = over(image->pixels[(size_t)sy * iw + sx], bg),
                         b = over(image->pixels[(size_t)sy * iw + next_x], bg),
                         c = over(image->pixels[(size_t)next_y * iw + sx], bg),
                         d = over(image->pixels[(size_t)next_y * iw + next_x], bg);
                color = interpolate(interpolate(a, b, fx & 65535), interpolate(c, d, fx & 65535),
                                    fy & 65535);
            }

            out[(size_t)y * width + x] = color;
        }
    }
}

static void make_preview(int index, Image *image) {
    Image *preview = &previews[index];
    if (!preview->pixels) {
        preview->width = PREVIEW_WIDTH;
        preview->height = PREVIEW_HEIGHT;
        preview->pixels = allocate(PREVIEW_WIDTH * PREVIEW_HEIGHT * 4);
    }
    if (!preview->pixels)
        return;
    if (index < 3)
        procedural(preview->pixels, PREVIEW_WIDTH, PREVIEW_HEIGHT, index);
    else
        compose(preview->pixels, PREVIEW_WIDTH, PREVIEW_HEIGHT, image, WALLPAPER_FILL,
                wallpaper_backgrounds[0]);
}

int wallpaper_set(int index, const char *path) {
    if (index < 0 || index >= WALLPAPER_COUNT)
        return -E_INVAL;
    if (index >= 3) {
        const char *target = index == WALLPAPER_CUSTOM ? path : paths[index - 3];
        if (!target || !*target || strlen(target) >= OS_PATH_MAX)
            return -E_INVAL;
        for (const char *p = target; *p; p++)
            if (*p == '\n' || *p == '\r')
                return -E_INVAL;
        /* Decode first; a missing or malformed image keeps the old desktop. */
        Image next = {0};
        int error = load_image(target, &next);
        if (error)
            return error;
        release_image(&active);
        active = next;
        copy_text(active_path, sizeof(active_path), target);
        if (index == WALLPAPER_CUSTOM)
            copy_text(desk.wallpaper_path, sizeof(desk.wallpaper_path), target);
        make_preview(index, &active);
    } else {
        release_image(&active);
        active_path[0] = 0;
    }
    desk.wallpaper_index = index;
    desk.wallpaper_status[0] = 0;
    generated = false;
    return 0;
}

void wallpaper_generate(void) {
    if (generated && generated_index == desk.wallpaper_index &&
        generated_mode == desk.wallpaper_mode &&
        generated_background == desk.wallpaper_background && generated_width == desk.width &&
        generated_height == desk.height)
        return;
    if (desk.wallpaper_index >= 3 && active.pixels)
        compose(desk.wallpaper, desk.width, desk.height, &active, desk.wallpaper_mode,
                wallpaper_backgrounds[desk.wallpaper_background]);
    else
        procedural(desk.wallpaper, desk.width, desk.height, MIN(desk.wallpaper_index, 2));
    generated = true;
    generated_index = desk.wallpaper_index;
    generated_mode = desk.wallpaper_mode;
    generated_background = desk.wallpaper_background;
    generated_width = desk.width;
    generated_height = desk.height;
}

void wallpaper_prepare_previews(void) {
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        if (previews[i].pixels)
            continue;
        if (i < 3)
            make_preview(i, NULL);
        else {
            const char *path = i == WALLPAPER_CUSTOM ? desk.wallpaper_path : paths[i - 3];
            if (!*path)
                continue;
            if (!strcmp(path, active_path) && active.pixels)
                make_preview(i, &active);
            else {
                Image image = {0};
                if (!load_image(path, &image))
                    make_preview(i, &image);
                release_image(&image);
            }
        }
    }
}

void wallpaper_draw_preview(int index, Rect r) {
    Image *image = &previews[index];
    if (!image->pixels || empty(r))
        return;
    Rect clipped = intersect(r, desk.clip);
    int dw = r.w, dh = r.h;
    if ((int64_t)image->width * r.h >= (int64_t)image->height * r.w)
        dw = ((int64_t)image->width * r.h + image->height - 1) / image->height;
    else
        dh = ((int64_t)image->height * r.w + image->width - 1) / image->width;
    int left = r.x + (r.w - dw) / 2, top = r.y + (r.h - dh) / 2;
    for (int y = clipped.y; y < clipped.y + clipped.h; y++) {
        int sy = (y - top) * image->height / dh;
        uint32_t *out = paint_pixel(clipped.x, y);
        for (int x = clipped.x; x < clipped.x + clipped.w; x++)
            *out++ = image->pixels[(size_t)sy * image->width + (x - left) * image->width / dw];
    }
}

void wallpaper_cleanup(void) {
    release_image(&active);
    for (int i = 0; i < WALLPAPER_COUNT; i++)
        release_image(&previews[i]);
    active_path[0] = 0;
    generated = false;
}
