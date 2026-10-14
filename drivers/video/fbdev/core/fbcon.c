#include <video/fbcon.h>
#include <video/scanout.h>
#include <font.h>
#include <console.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sync.h>
#include <mm/user.h>
#include <string.h>

static spinlock_t framebuffer_lock;
static bool graphics_active;

/* Standard Linux 16-color VGA palette */
static const uint32_t vga_palette[16] = {
    0x000000, /* 0: Black */
    0xAA0000, /* 1: Red */
    0x00AA00, /* 2: Green */
    0xAA5500, /* 3: Brown / Dark Yellow */
    0x0000AA, /* 4: Blue */
    0xAA00AA, /* 5: Magenta */
    0x00AAAA, /* 6: Cyan */
    0xAAAAAA, /* 7: Light Gray / Default text */
    0x555555, /* 8: Dark Gray / Bright Black */
    0xFF5555, /* 9: Bright Red */
    0x55FF55, /* 10: Bright Green */
    0xFFFF55, /* 11: Bright Yellow */
    0x5555FF, /* 12: Bright Blue */
    0xFF55FF, /* 13: Bright Magenta */
    0x55FFFF, /* 14: Bright Cyan */
    0xFFFFFF  /* 15: Bright White */
};

/* Terminal Engine State */
static struct boot_framebuffer *fb_ctx = NULL;
static const struct font_desc *current_font = NULL;
static size_t fb_cols = 0;
static size_t fb_rows = 0;
static size_t cursor_x = 0;
static size_t cursor_y = 0;

static uint8_t cur_fg_idx = 7; /* Light Gray */
static uint8_t cur_bg_idx = 0; /* Black */
static bool cur_bold = false;
static bool cur_reverse, pending_wrap;
static size_t scroll_top, scroll_bottom, saved_x, saved_y;

/* ANSI Parser State Machine */
enum ansi_state {
    ANSI_NORMAL,
    ANSI_ESC,
    ANSI_BRACKET,
    ANSI_PARAMS,
    ANSI_CHARSET,
    ANSI_OSC,
    ANSI_OSC_ESC
};

static enum ansi_state parser_state = ANSI_NORMAL;
static int ansi_params[8];
static int ansi_param_cnt = 0;

/* Color Packing helper using Limine's hardware masks */
static inline uint32_t pack_rgb(uint32_t hex) {
    if (!fb_ctx)
        return hex;

    uint8_t r = (hex >> 16) & 0xFF;
    uint8_t g = (hex >> 8) & 0xFF;
    uint8_t b = hex & 0xFF;

    if (fb_ctx->red_mask_size == 0 && fb_ctx->blue_mask_size == 0) {
        return (r << 16) | (g << 8) | b;
    }

    return ((uint32_t)r << fb_ctx->red_mask_shift) | ((uint32_t)g << fb_ctx->green_mask_shift) |
           ((uint32_t)b << fb_ctx->blue_mask_shift);
}

static inline uint32_t get_active_fg(void) {
    uint8_t idx = cur_reverse ? cur_bg_idx : cur_fg_idx;
    if (cur_bold && idx < 8)
        idx += 8;
    return pack_rgb(vga_palette[idx]);
}

static inline uint32_t get_active_bg(void) {
    return pack_rgb(vga_palette[cur_reverse ? cur_fg_idx : cur_bg_idx]);
}

/* Hardware line scrolling: shifts entire screen buffer up by one glyph height */
static void fbcon_scroll(void) {
    if (!fb_ctx || !current_font)
        return;

    uint8_t *fb = (uint8_t *)fb_ctx->address;
    size_t line_pitch = fb_ctx->pitch;
    size_t font_h_bytes = (size_t)current_font->height * line_pitch;
    size_t total_scroll_bytes = (fb_rows - 1) * font_h_bytes;

    /* 64-bit accelerated block transfer */
    uint64_t *dst64 = (uint64_t *)fb;
    const uint64_t *src64 = (const uint64_t *)(fb + font_h_bytes);
    size_t qwords = total_scroll_bytes / 8;
    for (size_t i = 0; i < qwords; i++) {
        dst64[i] = src64[i];
    }

    /* Clear bottom character row */
    uint32_t bg = get_active_bg();
    for (size_t y = (fb_rows - 1) * (size_t)current_font->height;
         y < fb_rows * (size_t)current_font->height; y++) {
        uint32_t *line = (uint32_t *)(fb + y * line_pitch);
        for (size_t x = 0; x < fb_cols * (size_t)current_font->width; x++) {
            line[x] = bg;
        }
    }

    cursor_y = fb_rows - 1;
}

void fbcon_clear(void) {
    if (!fb_ctx)
        return;
    uint8_t *fb = (uint8_t *)fb_ctx->address;
    uint32_t bg = get_active_bg();

    for (size_t y = 0; y < fb_ctx->height; y++) {
        uint32_t *line = (uint32_t *)(fb + y * fb_ctx->pitch);
        for (size_t x = 0; x < fb_ctx->width; x++) {
            line[x] = bg;
        }
    }
    cursor_x = 0;
    cursor_y = 0;
    pending_wrap = false;
}

static void fbcon_draw_glyph(size_t col, size_t row, char c, uint32_t fg, uint32_t bg) {
    if (!fb_ctx || !current_font || col >= fb_cols || row >= fb_rows)
        return;

    const uint8_t *font_bytes = (const uint8_t *)current_font->data;
    const uint8_t *glyph = &font_bytes[(unsigned char)c * current_font->height];
    uint8_t *fb = (uint8_t *)fb_ctx->address;

    size_t base_x = col * current_font->width;
    size_t base_y = row * current_font->height;

    for (size_t y = 0; y < (size_t)current_font->height; y++) {
        uint32_t *pixel = (uint32_t *)(fb + (base_y + y) * fb_ctx->pitch + base_x * 4);
        uint8_t bits = glyph[y];
        for (size_t x = 0; x < (size_t)current_font->width; x++) {
            pixel[x] = (bits & (0x80 >> x)) ? fg : bg;
        }
    }
}

static void terminal_scroll(bool reverse, size_t count) {
    size_t top = scroll_top, bottom = scroll_bottom;
    count = count < bottom - top + 1 ? count : bottom - top + 1;
    size_t stride = fb_ctx->pitch * current_font->height;
    uint8_t *pixels = fb_ctx->address;
    if (reverse)
        memmove(pixels + (top + count) * stride, pixels + top * stride,
                (bottom - top + 1 - count) * stride);
    else
        memmove(pixels + top * stride, pixels + (top + count) * stride,
                (bottom - top + 1 - count) * stride);
    size_t first = reverse ? top : bottom - count + 1;
    for (size_t y = first; y < first + count; y++)
        for (size_t x = 0; x < fb_cols; x++)
            fbcon_draw_glyph(x, y, ' ', get_active_fg(), get_active_bg());
}

static void terminal_index(bool reverse) {
    pending_wrap = false;
    if (reverse) {
        if (cursor_y == scroll_top)
            terminal_scroll(true, 1);
        else if (cursor_y)
            cursor_y--;
    } else {
        if (cursor_y == scroll_bottom)
            terminal_scroll(false, 1);
        else if (cursor_y + 1 < fb_rows)
            cursor_y++;
    }
}

static void terminal_csi(char final) {
    size_t n = ansi_params[0] > 0 ? (size_t)ansi_params[0] : 1;
    if (final == 'A')
        cursor_y = cursor_y > n ? cursor_y - n : 0;
    else if (final == 'B')
        cursor_y = cursor_y + n < fb_rows ? cursor_y + n : fb_rows - 1;
    else if (final == 'C')
        cursor_x = cursor_x + n < fb_cols ? cursor_x + n : fb_cols - 1;
    else if (final == 'D')
        cursor_x = cursor_x > n ? cursor_x - n : 0;
    else if (final == 'G')
        cursor_x = n <= fb_cols ? n - 1 : fb_cols - 1;
    else if (final == 'd')
        cursor_y = n <= fb_rows ? n - 1 : fb_rows - 1;
    else if (final == 'r') {
        size_t bottom = ansi_params[1] > 0 ? (size_t)ansi_params[1] - 1 : fb_rows - 1;
        if (n - 1 < bottom && bottom < fb_rows) {
            scroll_top = n - 1;
            scroll_bottom = bottom;
            cursor_x = cursor_y = 0;
        }
    } else if (final == 's') {
        saved_x = cursor_x;
        saved_y = cursor_y;
    } else if (final == 'u') {
        cursor_x = saved_x;
        cursor_y = saved_y;
    }
    pending_wrap = false;
}

static void handle_sgr(void) {
    if (ansi_param_cnt == 0) {
        cur_bold = false;
        cur_reverse = false;
        cur_fg_idx = 7;
        cur_bg_idx = 0;
        return;
    }

    for (int i = 0; i < ansi_param_cnt; i++) {
        int code = ansi_params[i];
        if (code == 0) {
            cur_bold = false;
            cur_reverse = false;
            cur_fg_idx = 7;
            cur_bg_idx = 0;
        } else if (code == 1) {
            cur_bold = true;
        } else if (code == 7) {
            cur_reverse = true;
        } else if (code == 27) {
            cur_reverse = false;
        } else if (code == 2 || code == 22) {
            cur_bold = false;
        } else if (code >= 30 && code <= 37) {
            cur_fg_idx = code - 30;
        } else if (code == 39) {
            cur_fg_idx = 7;
        } else if (code >= 40 && code <= 47) {
            cur_bg_idx = code - 40;
        } else if (code == 49) {
            cur_bg_idx = 0;
        } else if (code >= 90 && code <= 97) {
            cur_fg_idx = (code - 90) + 8;
        } else if (code >= 100 && code <= 107) {
            cur_bg_idx = (code - 100) + 8;
        }
    }
}

void fbcon_putc(char c) {
    /* ------------------------------------------------------------- */
    /* ANSI Escape Parser                                            */
    /* ------------------------------------------------------------- */
    if (parser_state == ANSI_CHARSET) {
        parser_state = ANSI_NORMAL;
        return;
    }

    if (parser_state == ANSI_OSC) {
        if (c == 7)
            parser_state = ANSI_NORMAL;
        else if (c == 27)
            parser_state = ANSI_OSC_ESC;
        return;
    }

    if (parser_state == ANSI_OSC_ESC) {
        parser_state = c == '\\' ? ANSI_NORMAL : ANSI_OSC;
        return;
    }

    if (parser_state == ANSI_NORMAL) {
        if (c == '\033') {
            parser_state = ANSI_ESC;
            return;
        }
    } else if (parser_state == ANSI_ESC) {
        if (c == '[') {
            parser_state = ANSI_BRACKET;
            ansi_param_cnt = 0;
            for (int i = 0; i < 8; i++)
                ansi_params[i] = 0;
            return;
        } else if (c == '(' || c == ')') {
            parser_state = ANSI_CHARSET;
            return;
        } else if (c == ']') {
            parser_state = ANSI_OSC;
            return;
        } else {
            parser_state = ANSI_NORMAL;
            if (c == '7') {
                saved_x = cursor_x;
                saved_y = cursor_y;
            } else if (c == '8') {
                cursor_x = saved_x;
                cursor_y = saved_y;
            } else if (c == 'D')
                terminal_index(false);
            else if (c == 'M')
                terminal_index(true);
            else if (c == 'E') {
                cursor_x = 0;
                terminal_index(false);
            }
            return;
        }
    } else if (parser_state == ANSI_BRACKET || parser_state == ANSI_PARAMS) {
        if (c >= '0' && c <= '9') {
            /* Bound parameters supplied by arbitrary userspace output. */
            ansi_params[ansi_param_cnt] = ansi_params[ansi_param_cnt] < 10000
                                              ? ansi_params[ansi_param_cnt] * 10 + (c - '0')
                                              : 10000;
            parser_state = ANSI_PARAMS;
            return;
        } else if (c == '?') {
            return;
        } else if (c == ';') {
            if (ansi_param_cnt < 7)
                ansi_param_cnt++;
            parser_state = ANSI_PARAMS;
            return;
        } else if (c == 'm') {
            if (parser_state == ANSI_PARAMS)
                ansi_param_cnt++;
            handle_sgr();
            parser_state = ANSI_NORMAL;
            return;
        } else if (c == 'J') {
            if (ansi_params[0] >= 2) {
                size_t x = cursor_x, y = cursor_y;
                fbcon_clear();
                cursor_x = x;
                cursor_y = y;
            } else
                for (size_t y = 0; y < fb_rows; y++)
                    for (size_t x = 0; x < fb_cols; x++) {
                        bool before = y < cursor_y || (y == cursor_y && x <= cursor_x);
                        if (ansi_params[0] == 1 ? before
                                                : !before || (y == cursor_y && x == cursor_x))
                            fbcon_draw_glyph(x, y, ' ', get_active_fg(), get_active_bg());
                    }
            pending_wrap = false;
            parser_state = ANSI_NORMAL;
            return;
        } else if (c == 'H' || c == 'f') {
            cursor_y = ansi_params[0] > 0 ? (size_t)ansi_params[0] - 1 : 0;
            cursor_x = ansi_param_cnt > 0 && ansi_params[1] > 0 ? (size_t)ansi_params[1] - 1 : 0;
            if (cursor_y >= fb_rows)
                cursor_y = fb_rows - 1;
            if (cursor_x >= fb_cols)
                cursor_x = fb_cols - 1;
            pending_wrap = false;
            parser_state = ANSI_NORMAL;
            return;
        } else if (c == 'K') {
            size_t first = ansi_params[0] ? 0 : cursor_x,
                   last = ansi_params[0] == 1 ? cursor_x + 1 : fb_cols;
            for (size_t x = first; x < last; x++)
                fbcon_draw_glyph(x, cursor_y, ' ', get_active_fg(), get_active_bg());
            pending_wrap = false;
            parser_state = ANSI_NORMAL;
            return;
        } else {
            terminal_csi(c);
            parser_state = ANSI_NORMAL;
            return;
        }
    }

    /* ------------------------------------------------------------- */
    /* Character Output & Wrapping                                   */
    /* ------------------------------------------------------------- */
    if (c == '\n') {
        cursor_x = 0;
        terminal_index(false);
        return;
    }

    if (c == '\r') {
        cursor_x = 0;
        pending_wrap = false;
        return;
    }

    if (c == '\t') {
        cursor_x = (cursor_x + 8) & ~7;
        if (cursor_x >= fb_cols) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= fb_rows)
                fbcon_scroll();
        }
        return;
    }

    if (c == '\b') {
        if (cursor_x > 0)
            cursor_x--;
        pending_wrap = false;
        return;
    }
    if ((unsigned char)c < 32 || c == 127)
        return;
    if (pending_wrap) {
        cursor_x = 0;
        terminal_index(false);
    }
    fbcon_draw_glyph(cursor_x, cursor_y, c, get_active_fg(), get_active_bg());
    cursor_x++;
    if (cursor_x >= fb_cols) {
        cursor_x = fb_cols - 1;
        pending_wrap = true;
    }
}

static void fbcon_console_write(struct console *con, const char *str, size_t len) {
    (void)con;
    uint64_t f = spin_lock_irqsave(&framebuffer_lock);
    for (size_t i = 0; !graphics_active && i < len; i++) {
        fbcon_putc(str[i]);
    }
    spin_unlock_irqrestore(&framebuffer_lock, f);
}

bool fbcon_display_info(struct os_display_info *info) {
    if (!fb_ctx)
        return false;
    *info = (struct os_display_info){.version = OS_DISPLAY_VERSION,
                                     .width = fb_ctx->width,
                                     .height = fb_ctx->height,
                                     .format = 1,
                                     .max_pixels = OS_DISPLAY_MAX_PIXELS};
    return true;
}

void fbcon_winsize(struct os_winsize *size) {
    *size = (struct os_winsize){.rows = fb_ctx ? fb_rows : 25,
                                .cols = fb_ctx ? fb_cols : 80,
                                .xpixel = fb_ctx ? fb_ctx->width : 0,
                                .ypixel = fb_ctx ? fb_ctx->height : 0};
}

void fbcon_graphics(bool enabled) {
    uint64_t f = spin_lock_irqsave(&framebuffer_lock);
    scanout_reset();
    graphics_active = enabled;
    parser_state = ANSI_NORMAL;
    cur_fg_idx = 7;
    cur_bg_idx = 0;
    cur_bold = false;
    cur_reverse = pending_wrap = false;
    scroll_top = 0;
    scroll_bottom = fb_rows - 1;
    /* Preserve the console until the first complete desktop frame replaces
       it. Clearing on acquisition adds a visible black flash. */
    if (!enabled) {
        fbcon_clear();
    }
    spin_unlock_irqrestore(&framebuffer_lock, f);
}

void fbcon_present(const struct os_display_rect *rect, const uint32_t *pixels) {
    uint64_t f = spin_lock_irqsave(&framebuffer_lock);
    scanout_reset();
    for (uint32_t y = 0; y < rect->height; y++) {
        uint32_t *line =
            (void *)((uint8_t *)fb_ctx->address + (rect->y + y) * fb_ctx->pitch + rect->x * 4);
        if (fb_ctx->red_mask_shift == 16 && fb_ctx->green_mask_shift == 8 &&
            fb_ctx->blue_mask_shift == 0)
            memcpy(line, pixels + (size_t)y * rect->width, rect->width * 4);
        else
            for (uint32_t x = 0; x < rect->width; x++)
                line[x] = pack_rgb(pixels[(size_t)y * rect->width + x]);
    }
    __asm__ volatile("sfence" ::: "memory");
    spin_unlock_irqrestore(&framebuffer_lock, f);
}

bool fbcon_present_batch(const struct address_space *space, const struct os_display_batch *batch) {
    scanout_reset();
    return fbcon_upload_batch(space, batch, fb_ctx->address);
}

bool fbcon_upload_batch(const struct address_space *space, const struct os_display_batch *batch,
                        void *target) {
    /* The caller owns the display mutex, so release/other presents cannot race.
       Suppressed console writers hold framebuffer_lock and never touch pixels
       while graphics is active. Avoid locking/disabling IRQs for every row. */
    uint64_t saved = spin_lock_irqsave(&framebuffer_lock);
    bool active = graphics_active;
    spin_unlock_irqrestore(&framebuffer_lock, saved);
    if (!active)
        return false;
    static uint32_t row[8192];
    bool native = fb_ctx->red_mask_shift == 16 && fb_ctx->green_mask_shift == 8 &&
                  fb_ctx->blue_mask_shift == 0;
    bool streaming = scanout_streaming_copy();
    bool direct = user_space_direct(space);
    extern void video_copy(void *, const void *, size_t);
    for (unsigned i = 0; i < batch->count; i++) {
        const struct os_damage_rect *r = &batch->rects[i];
        for (unsigned y = 0; y < r->height; y++) {
            uint64_t src = batch->pixels + ((uint64_t)(r->y + y) * batch->stride + r->x) * 4;
            uint32_t *dst = (void *)((uint8_t *)target + (r->y + y) * fb_ctx->pitch + r->x * 4);
            bool ok = true;
            if (direct) {
                if (native && streaming)
                    video_copy(dst, (const void *)(uintptr_t)src, r->width * 4);
                else
                    memcpy(native ? (void *)dst : row, (const void *)(uintptr_t)src, r->width * 4);
            } else {
                ok = native && streaming ? user_copy_video_validated(space, dst, src, r->width * 4)
                                         : user_copy_in_validated(space, native ? (void *)dst : row,
                                                                  src, r->width * 4);
            }
            if (ok && !native)
                for (unsigned x = 0; x < r->width; x++)
                    dst[x] = pack_rgb(row[x]);
            if (!ok)
                return false;
        }
    }
    __asm__ volatile("sfence" ::: "memory");
    return true;
}

static struct console fb_console_device = {
    .name = "fbcon",
    .write = fbcon_console_write,
    .flags = CON_ENABLED,
    .data = NULL,
    .next = NULL,
};

#include <printk.h>

void fbcon_init(struct boot_framebuffer *fb) {
    pr_info("fbcon cleared1");
    if (!fb || !fb->address || fb->bpp != 32 || fb->width < 8 || fb->height < 32 ||
        fb->width > 8192 || fb->height > 8192 || fb->pitch < fb->width * 4 ||
        fb->red_mask_size != 8 || fb->green_mask_size != 8 || fb->blue_mask_size != 8 ||
        fb->red_mask_shift > 24 || fb->green_mask_shift > 24 || fb->blue_mask_shift > 24)
        return;

    pr_info("fbcon cleared2");
    current_font = get_default_font((int)fb->width, (int)fb->height);

    fb_ctx = fb;
    scanout_framebuffer(fb);
    fb_cols = fb->width / (size_t)current_font->width;
    fb_rows = fb->height / (size_t)current_font->height;
    scroll_top = 0;
    scroll_bottom = fb_rows - 1;
    cursor_x = 0;
    cursor_y = 0;

    fbcon_clear();
    register_console(&fb_console_device);
}
