#include "desktop.h"
#include "font_data.h"
#include "pixel_blend.h"

int S(int n) {
    return (n * desk.scale + 50) / 100;
}

int clampi(int v, int low, int high) {
    return v < low ? low : v > high ? high : v;
}

bool contains(Rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

Rect intersect(Rect a, Rect b) {
    int x = MAX(a.x, b.x), y = MAX(a.y, b.y);

    return (Rect){x, y, MAX(0, MIN(a.x + a.w, b.x + b.w) - x),
                  MAX(0, MIN(a.y + a.h, b.y + b.h) - y)};
}

bool empty(Rect r) {
    return r.w <= 0 || r.h <= 0;
}

void *allocate(size_t bytes) {
    long p = syscall6(SYS_mmap, 0, bytes, 3, 0x22, -1, 0);
    return p < 0 ? NULL : (void *)p;
}

void deallocate(void *ptr, size_t bytes) {
    if (ptr) {
        call2(SYS_munmap, ptr, bytes);
    }
}

void copy_text(char *out, size_t size, const char *in) {
    if (!size) {
        return;
    }
    size_t n = MIN(strlen(in), size - 1);
    memmove(out, in, n);
    out[n] = 0;
}

void append_text(char *out, size_t size, const char *in) {
    size_t n = strlen(out);
    if (n < size) {
        copy_text(out + n, size - n, in);
    }
}

void format_uint(char *out, uint64_t v) {
    char b[32];
    int n = 0;
    do {
        b[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    int i = 0;
    while (n) {
        out[i++] = b[--n];
    }
    out[i] = 0;
}

void format_size(char *out, uint64_t v) {
    const char *unit = " B";
    if (v >= 1024ULL * 1024 * 1024) {
        v = (v + 536870912) / 1073741824;
        unit = " GB";
    } else if (v >= 1024 * 1024) {
        v = (v + 524288) / 1048576;
        unit = " MB";
    } else if (v >= 1024) {
        v = (v + 512) / 1024;
        unit = " KB";
    }
    format_uint(out, v);
    append_text(out, 32, unit);
}

const char *basename(const char *path) {
    const char *last = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' && p[1]) {
            last = p + 1;
        }
    }
    return last;
}

void format_date(char *out, int64_t epoch) {
    int64_t z = epoch / 86400 + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1, month = mp + (mp < 10 ? 3 : -9);
    static const char *names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    copy_text(out, 32, names[month - 1]);
    append_text(out, 32, " ");
    char number[24];
    format_uint(number, day);
    append_text(out, 32, number);
}

int join_path(char *out, const char *dir, const char *name) {
    size_t n = strlen(dir), m = strlen(name);
    if (n + m + 2 > OS_PATH_MAX) {
        return -E_NAMETOOLONG;
    }
    copy_text(out, OS_PATH_MAX, dir);
    if (n > 1) {
        out[n++] = '/';
    }
    memcpy(out + n, name, m + 1);
    return 0;
}

int normalize_path(char *out, const char *base, const char *path) {
    char raw[OS_PATH_MAX];
    if (!path[0]) {
        return -E_NOENT;
    }
    int e = path[0] == '/' ? (copy_text(raw, sizeof(raw), path),
                              strlen(path) >= sizeof(raw) ? -E_NAMETOOLONG : 0)
                           : join_path(raw, base, path);
    if (e) {
        return e;
    }
    unsigned offsets[256], depth = 0, used = 1;
    out[0] = '/';
    for (unsigned i = 0; raw[i];) {
        while (raw[i] == '/') {
            i++;
        }
        unsigned first = i;
        while (raw[i] && raw[i] != '/') {
            i++;
        }
        unsigned n = i - first;
        if (!n || (n == 1 && raw[first] == '.')) {
            continue;
        }
        if (n == 2 && raw[first] == '.' && raw[first + 1] == '.') {
            if (depth) {
                used = offsets[--depth];
            }
            continue;
        }

        if (n > OS_NAME_MAX || used + n + 1 >= OS_PATH_MAX) {
            return -E_NAMETOOLONG;
        }
        offsets[depth++] = used;
        if (used > 1) {
            out[used++] = '/';
        }
        memcpy(out + used, raw + first, n);
        used += n;
    }

    out[used] = 0;
    return 0;
}

uint64_t now_ms(void) {
    struct os_timespec ts;
    return call2(SYS_clock_gettime, 1, &ts) ? 0 : (uint64_t)ts.sec * 1000 + ts.nsec / 1000000;
}

void trace(const char *event, Window *w, const char *detail) {
    if (desk.trace) {
        printf("DESKTOP %s window=%d workspace=%d %s\n", event, w ? w->id : 0, desk.workspace + 1,
               detail ? detail : "");
    }
}

void trace_scroll(Window *w, const char *app, int position, int target) {
    if (desk.trace)
        printf("DESKTOP SCROLL window=%d workspace=%d %s position=%d target=%d time=%llu\n",
               w ? w->id : 0, desk.workspace + 1, app, position, target,
               (unsigned long long)now_ms());
}

static void surface_damage(Window *w, Rect r) {
    r = intersect(r, w->box);
    if (empty(r)) {
        return;
    }
    r.x -= w->box.x;
    r.y -= w->box.y;
    Rect a = w->surface_damage;
    if (empty(a)) {
        w->surface_damage = r;
    } else {
        int x = MIN(a.x, r.x), y = MIN(a.y, r.y);
        w->surface_damage =
            (Rect){x, y, MAX(a.x + a.w, r.x + r.w) - x, MAX(a.y + a.h, r.y + r.h) - y};
    }
}

static void damage(Rect r) {
    r = intersect(r, (Rect){0, 0, desk.width, desk.height});
    if (empty(r)) {
        return;
    }
    for (int i = 0; i < desk.dirty_count; i++) {
        Rect a = desk.dirty[i];
        if (r.x <= a.x + a.w + 2 && r.x + r.w + 2 >= a.x && r.y <= a.y + a.h + 2 &&
            r.y + r.h + 2 >= a.y) {
            int x = MIN(a.x, r.x), y = MIN(a.y, r.y);
            r = (Rect){x, y, MAX(a.x + a.w, r.x + r.w) - x, MAX(a.y + a.h, r.y + r.h) - y};
            desk.dirty[i] = desk.dirty[--desk.dirty_count];
            i = -1;
        }
    }

    if (desk.dirty_count == 32) {
        desk.dirty_count = 1;
        desk.dirty[0] = (Rect){0, 0, desk.width, desk.height};
    } else {
        desk.dirty[desk.dirty_count++] = r;
    }
}

void invalidate(Rect r) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &desk.windows[i];
        if (w->alive && !w->minimized && w->workspace == desk.workspace) {
            surface_damage(w, r);
        }
    }

    damage(r);
}

void damage_window(Window *w) {
    if (w && w->alive && !w->minimized && w->workspace == desk.workspace) {
        damage((Rect){w->box.x - S(34), w->box.y - S(14), w->box.w + S(68), w->box.h + S(62)});
    }
}

void invalidate_window(Window *w) {
    if (!w || !w->alive) {
        return;
    }
    w->surface_damage = (Rect){0, 0, w->box.w, w->box.h};
    damage_window(w);
}

void invalidate_all(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &desk.windows[i];
        if (w->alive) {
            w->surface_damage = (Rect){0, 0, w->box.w, w->box.h};
        }
    }

    desk.dirty_count = 1;
    desk.dirty[0] = (Rect){0, 0, desk.width, desk.height};
}

void clip_set(Rect r) {
    desk.clip = intersect(r, (Rect){0, 0, desk.width, desk.height});
}

uint32_t *paint_pixel(int x, int y) {
    return desk.paint + (size_t)(y - desk.paint_y) * desk.paint_stride + x - desk.paint_x;
}

static uint32_t mix(uint32_t bg, uint32_t fg, unsigned a) {
    unsigned b = 255 - a;
    return ((((bg & 0xff00ff) * b + (fg & 0xff00ff) * a + 0x800080) >> 8) & 0xff00ff) |
           ((((bg & 0x00ff00) * b + (fg & 0x00ff00) * a + 0x008000) >> 8) & 0x00ff00);
}

void fill(Rect r, uint32_t color) {
    r = intersect(r, desk.clip);
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = paint_pixel(r.x, y);
        for (int x = 0; x < r.w; x++) {
            row[x] = color;
        }
    }
}

void blend(Rect r, uint32_t color, unsigned alpha) {
    if (alpha >= 255) {
        fill(r, color);
        return;
    }
    r = intersect(r, desk.clip);
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = paint_pixel(r.x, y);
        for (int x = 0; x < r.w; x++) {
            row[x] = mix(row[x], color, alpha);
        }
    }
}

static int round_inset(int row, int h, int radius) {
    int dy = MIN(row, h - 1 - row);
    if (dy >= radius) {
        return 0;
    }
    int yy = 2 * (radius - dy) - 1, x = 0, rr = 4 * radius * radius;
    while (x < radius && (2 * (radius - x) - 1) * (2 * (radius - x) - 1) + yy * yy > rr) {
        x++;
    }
    return x;
}

void rounded(Rect r, int radius, uint32_t color) {
    radius = MIN(radius, MIN(r.w, r.h) / 2);
    for (int y = MAX(r.y, desk.clip.y); y < MIN(r.y + r.h, desk.clip.y + desk.clip.h); y++) {
        int inset = round_inset(y - r.y, r.h, radius);
        fill((Rect){r.x + inset, y, r.w - 2 * inset, 1}, color);
    }
}

void border(Rect r, int radius, uint32_t color) {
    for (int y = MAX(r.y, desk.clip.y); y < MIN(r.y + r.h, desk.clip.y + desk.clip.h); y++) {
        int inset = round_inset(y - r.y, r.h, radius);
        if (y == r.y || y == r.y + r.h - 1) {
            fill((Rect){r.x + inset, y, r.w - 2 * inset, 1}, color);
        } else {
            fill((Rect){r.x + inset, y, 1, 1}, color);
            fill((Rect){r.x + r.w - inset - 1, y, 1, 1}, color);
        }
    }
}

void line(int x1, int y1, int x2, int y2, uint32_t color) {
    int dx = x1 < x2 ? x2 - x1 : x1 - x2, sx = x1 < x2 ? 1 : -1, dy = y1 < y2 ? y1 - y2 : y2 - y1,
        sy = y1 < y2 ? 1 : -1, e = dx + dy;
    for (;;) {
        fill((Rect){x1, y1, 1, 1}, color);
        if (x1 == x2 && y1 == y2) {
            break;
        }
        int ee = e * 2;
        if (ee >= dy) {
            e += dy;
            x1 += sx;
        }
        if (ee <= dx) {
            e += dx;
            y1 += sy;
        }
    }
}

static unsigned next_char(const char **text) {
    const unsigned char *p = (const unsigned char *)*text;
    unsigned c = *p++;
    if (c >= 0xc2 && c <= 0xc3 && (*p & 0xc0) == 0x80) {
        c = ((c & 31) << 6) | (*p++ & 63);
    } else if (c >= 128) {
        while ((*p & 0xc0) == 0x80) {
            p++;
        }
        c = '?';
    }

    *text = (const char *)p;
    return c < 32 ? '?' : c;
}

static int text_width_limit(const char *text, size_t bytes, Font font) {
    unsigned sum = 0, index = desk.scale_index * 8 + font;
    const char *start = text;
    while (*text && (size_t)(text - start) < bytes) {
        unsigned c = next_char(&text);
        sum += gui_glyphs[index][c - 32].advance;
    }
    return (sum + 32) / 64;
}

int text_width(const char *text, Font font) {
    return text_width_limit(text, SIZE_MAX, font);
}

static void glyph(int x, int y, unsigned c, Font font, uint32_t color) {
    const struct Glyph *g = &gui_glyphs[desk.scale_index * 8 + font][c - 32];
    Rect r = {x + g->left, y + g->top, g->w, g->h}, visible = intersect(r, desk.clip);
    for (int yy = visible.y; yy < visible.y + visible.h; yy++) {
        for (int xx = visible.x; xx < visible.x + visible.w; xx++) {
            unsigned a = gui_font_pixels[g->offset + (yy - r.y) * g->w + xx - r.x];
            if (a) {
                uint32_t *p = paint_pixel(xx, yy);
                *p = a == 255 ? color : mix(*p, color, a);
            }
        }
    }
}

void label(int x, int y, const char *text, Font font, uint32_t color, int max_width) {
    if (max_width <= 0) {
        return;
    }
    int pen = 0, index = desk.scale_index * 8 + font, limit = max_width * 64;
    bool shortened = text_width(text, font) > max_width;
    int dots = text_width("..", font) * 64;
    while (*text) {
        unsigned c = next_char(&text);
        int advance = gui_glyphs[index][c - 32].advance;
        if (pen + advance > (shortened ? limit - dots : limit)) {
            break;
        }
        glyph(x + (pen + 32) / 64, y, c, font, color);
        pen += advance;
    }

    if (shortened) {
        glyph(x + (pen + 32) / 64, y, '.', font, color);
        pen += gui_glyphs[index]['.' - 32].advance;
        glyph(x + (pen + 32) / 64, y, '.', font, color);
    }
}

void button(Rect r, const char *text, Icon kind, bool primary, bool enabled, bool focused) {
    bool hover = enabled && contains(r, desk.mouse_x, desk.mouse_y);
    bool pressed = hover && (desk.buttons & 1);
    uint32_t bg = !enabled  ? THEME_TOOLBAR
                  : primary ? (hover ? THEME_ACCENT_HOVER : desk.accent)
                  : pressed ? desk.selection
                  : hover   ? theme_color(0xEDF1EF)
                            : THEME_TOOLBAR;
    rounded(r, S(5), bg);
    if (primary && focused) {
        border((Rect){r.x - 2, r.y - 2, r.w + 4, r.h + 4}, S(6), theme_color(0x96B5A9));
    }
    if (!primary) {
        border(r, S(5), focused ? desk.accent : theme_color(0xE5E9E8));
    }
    uint32_t fg = !enabled  ? theme_color(0xABB5B7)
                  : primary ? THEME_ON_ACCENT
                            : theme_color(0x617176);
    int x = r.x + S(10);
    if (kind < I_COUNT) {
        icon(x, r.y + (r.h - S(14)) / 2, kind, S(14), fg);
        x += S(21);
    }
    label(x, r.y + (r.h - S(14)) / 2, text, FONT_MEDIUM, fg, r.x + r.w - x - S(6));
}

void input_draw(Rect r, const char *value, const char *placeholder, int cursor, bool focused) {
    bool selected = cursor < 0;
    if (selected) {
        cursor = strlen(value);
    }
    rounded(r, S(5), THEME_WINDOW);
    border(r, S(5), focused ? desk.accent : theme_color(0xE5E9E8));
    Rect saved = desk.clip;
    clip_set(intersect(saved, (Rect){r.x + S(6), r.y + 2, r.w - S(12), r.h - 4}));
    int x = r.x + S(8), y = r.y + (r.h - S(14)) / 2;
    if (value[0]) {
        int advance = text_width_limit(value, cursor, FONT_NORMAL);
        int shift = MAX(0, advance - r.w + S(20));
        x -= shift;
        if (focused && selected) {
            fill((Rect){x, y, text_width(value, FONT_NORMAL), S(15)}, desk.selection);
        }
        label(x, y, value, FONT_NORMAL, theme_color(0x41525A), r.w + shift);
        if (focused) {
            fill((Rect){x + advance, y + S(2), 1, S(11)}, desk.accent);
        }
    } else {
        label(x, y, placeholder, FONT_NORMAL, theme_color(0x9BA5A8), r.w - S(16));
        if (focused) {
            fill((Rect){x, y + S(2), 1, S(11)}, desk.accent);
        }
    }

    desk.clip = saved;
}

bool input_key(char *value, size_t capacity, int *cursor, Key k) {
    size_t n = strlen(value);
    *cursor = clampi(*cursor, 0, n);
    if ((k.mods & MOD_CTRL) && (k.ascii == 'a' || k.ascii == 'A')) {
        *cursor = n;
        return true;
    }
    if (k.code == KEY_LEFT) {
        if (*cursor) {
            (*cursor)--;
        }
        return true;
    }

    if (k.code == KEY_RIGHT) {
        if (*cursor < (int)n) {
            (*cursor)++;
        }
        return true;
    }

    if (k.code == KEY_HOME) {
        *cursor = 0;
        return true;
    }
    if (k.code == KEY_END) {
        *cursor = n;
        return true;
    }
    if (k.code == KEY_BACKSPACE && *cursor) {
        memmove(value + *cursor - 1, value + *cursor, n - *cursor + 1);
        (*cursor)--;
        return true;
    }
    if (k.code == KEY_DELETE && *cursor < (int)n) {
        memmove(value + *cursor, value + *cursor + 1, n - *cursor);
        return true;
    }
    if (k.ascii >= 32 && k.ascii < 127 && !(k.mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) &&
        n + 1 < capacity) {
        memmove(value + *cursor + 1, value + *cursor, n - *cursor + 1);
        value[(*cursor)++] = k.ascii;
        return true;
    }

    return false;
}

void scrollbar(Rect viewport, int total, int scroll) {
    if (total <= viewport.h) {
        return;
    }
    int h = MAX(S(24), (int)((int64_t)viewport.h * viewport.h / total));
    int y = viewport.y + (int)((int64_t)scroll * (viewport.h - h) / (total - viewport.h));
    rounded((Rect){viewport.x + viewport.w - S(6), y, S(3), h}, S(2), theme_color(0xC3CECA));
}

static void cursor(void) {
    /* The tiny cursor rectangle is saved before this and restored after the
       synchronous display copy, leaving the composed canvas clean. */
    int x = desk.mouse_x, y = desk.mouse_y;
    for (int yy = 0; yy < S(19); yy++) {
        for (int xx = 0; xx < S(13); xx++) {
            bool edge = xx == 0 || yy == 0 || xx == (yy * 2) / 3;
            if (xx <= yy * 2 / 3 && yy < S(15) && x + xx < desk.width && y + yy < desk.height) {
                desk.output[(size_t)(y + yy) * desk.width + x + xx] = edge ? 0x1A2B38 : 0xF8FCFC;
            }
            if (yy >= S(10) && xx >= S(5) && xx <= S(7) && yy - xx < S(12) && x + xx < desk.width &&
                y + yy < desk.height) {
                desk.output[(size_t)(y + yy) * desk.width + x + xx] =
                    xx == S(5) ? 0x1A2B38 : 0xF8FCFC;
            }
        }
    }
}

static uint64_t render_time(void) {
    struct os_timespec t;
    if (call2(SYS_clock_gettime, 1, &t)) {
        return 0;
    }
    return t.sec * 1000000ULL + t.nsec / 1000;
}

void wm_free_surface(Window *w) {
    deallocate(w->surface, (size_t)w->surface_width * w->surface_height * 4);
    w->surface = NULL;
    deallocate(w->shadow_mask, w->shadow_bytes);
    w->shadow_mask = NULL;
    w->shadow_bytes = 0;
}

static void update_surface(Window *w) {
    bool resized = w->surface_width != w->box.w || w->surface_height != w->box.h;
    if (resized || !w->surface) {
        wm_free_surface(w);
        w->surface_width = w->box.w;
        w->surface_height = w->box.h;
        w->surface = allocate((size_t)w->box.w * w->box.h * 4);
        w->surface_damage = (Rect){0, 0, w->box.w, w->box.h};
    }

    bool focused = wm_focused() == w;
    if (focused != w->surface_focused || w->maximized != w->surface_maximized) {
        w->surface_damage = (Rect){0, 0, w->box.w, w->box.h};
        damage_window(w);
    }
    if (!w->surface || empty(w->surface_damage)) {
        return;
    }
    Rect saved = desk.clip, r = w->surface_damage;
    r.x += w->box.x;
    r.y += w->box.y;
    desk.paint = w->surface;
    desk.paint_stride = w->box.w;
    desk.paint_x = w->box.x;
    desk.paint_y = w->box.y;
    clip_set(r);
    wm_paint(w);
    desk.clip = saved;
    desk.paint = desk.scene;
    desk.paint_stride = desk.width;
    desk.paint_x = desk.paint_y = 0;
    w->surface_damage = (Rect){0};
    w->surface_focused = focused;
    w->surface_maximized = w->maximized;
    desk.surface_updates++;
}

void wm_draw(Window *w) {
    Rect b = w->box, outer = {b.x - S(34), b.y - S(14), b.w + S(68), b.h + S(62)};
    if (empty(intersect(desk.clip, outer))) {
        return;
    }
    if (!w->maximized &&
        (desk.clip.x < b.x || desk.clip.y < b.y || desk.clip.x + desk.clip.w > b.x + b.w ||
         desk.clip.y + desk.clip.h > b.y + b.h)) {
        window_shadow(w);
    }
    if (!w->surface) {
        wm_paint(w);
        return;
    }
    Rect r = intersect(desk.clip, b);
    int radius = w->maximized ? 0 : S(9);
    for (int y = r.y; y < r.y + r.h; y++) {
        int inset = round_inset(y - b.y, b.h, radius);
        int x = MAX(r.x, b.x + inset), end = MIN(r.x + r.w, b.x + b.w - inset);
        if (end > x) {
            memcpy(paint_pixel(x, y), w->surface + (size_t)(y - b.y) * b.w + x - b.x,
                   (end - x) * 4);
        }
    }
}

/* Only the exposed bands are cached: top/bottom span the whole width;
   middle rows contain the left and right bands, never the opaque interior. */
static size_t shadow_row_offset(const Window *w, int y) {
    int top = S(14), height = w->shadow_height - S(62);
    if (y < top)
        return (size_t)y * w->shadow_width;
    size_t offset = (size_t)top * w->shadow_width;
    if (y < top + height)
        return offset + (size_t)(y - top) * S(68);
    return offset + (size_t)height * S(68) + (size_t)(y - top - height) * w->shadow_width;
}

static void shadow_uniform(uint32_t *p, size_t count, uint32_t color, unsigned alpha) {
    pixels_blend(p, count, color, alpha);
}

static void shadow_blend(uint32_t *pixels, const uint8_t *alpha, size_t count, uint32_t color) {
    while (count--) {
        if (*alpha)
            *pixels = mix(*pixels, color, *alpha);
        pixels++;
        alpha++;
    }
}

void window_shadow(Window *w) {
    static uint8_t falloff[129], opacity[256][256];
    static bool ready;
    if (!ready) {
        for (unsigned i = 0; i <= 128; i++)
            falloff[i] = (uint64_t)i * i * (384 - 2 * i) * 255 / 2097152;
        for (unsigned y = 0; y < 256; y++)
            for (unsigned x = 0; x < 256; x++)
                opacity[y][x] = x * y * 96 / 65025;
        ready = true;
    }

    Rect b = w->box, outer = {b.x - S(34), b.y - S(14), b.w + S(68), b.h + S(62)};
    Rect r = intersect(outer, desk.clip);
    int radius = S(32), left = S(34), top = S(14);
    if (!w->shadow_mask || w->shadow_width != outer.w || w->shadow_height != outer.h ||
        w->shadow_scale != desk.scale) {
        deallocate(w->shadow_mask, w->shadow_bytes);
        w->shadow_width = outer.w;
        w->shadow_height = outer.h;
        w->shadow_scale = desk.scale;
        w->shadow_bytes = (size_t)outer.w * S(62) + (size_t)b.h * S(68);
        w->shadow_mask = allocate(w->shadow_bytes);
        if (w->shadow_mask) {
            for (int y = 0; y < outer.h; y++) {
                bool middle = y >= top && y < top + b.h;
                int dist = MIN(y - top - S(20), b.h - 1 + S(20) - (y - top));
                unsigned v = falloff[clampi((dist + radius) * 64 / radius, 0, 128)];
                uint8_t *row = w->shadow_mask + shadow_row_offset(w, y);
                int width = middle ? S(68) : outer.w;
                for (int column = 0; column < width; column++) {
                    int x = middle && column >= left ? column + b.w : column;
                    dist = MIN(x - left, b.w - 1 - (x - left));
                    unsigned h = falloff[clampi((dist + radius) * 64 / radius, 0, 128)];
                    row[column] = opacity[v][h];
                }
            }
        }
    }

    uint32_t shadow = THEME_SHADOW;
    for (int y = r.y; y < r.y + r.h; y++) {
        bool middle = y >= b.y && y < b.y + b.h;
        int dist = MIN(y - b.y - S(20), b.y + b.h - 1 + S(20) - y);
        unsigned v = falloff[clampi((dist + radius) * 64 / radius, 0, 128)];
        if (!v)
            continue;
        for (int side = 0; side < 2; side++) {
            int x = r.x, end = r.x + r.w;
            if (middle) {
                if (!side)
                    end = MIN(end, b.x);
                else
                    x = MAX(x, b.x + b.w);
            } else if (side)
                continue;
            if (end <= x)
                continue;
            uint32_t *pixels = paint_pixel(x, y);
            if (w->shadow_mask) {
                while (x < end) {
                    int chunk_end;
                    if (x >= b.x + radius && x < b.x + b.w - radius) {
                        chunk_end = MIN(end, b.x + b.w - radius);
                        shadow_uniform(pixels, chunk_end - x, shadow, opacity[v][255]);
                    } else {
                        chunk_end = x < b.x + radius ? MIN(end, b.x + radius) : end;
                        size_t offset = shadow_row_offset(w, y - outer.y) + x - outer.x;
                        if (middle && side)
                            offset -= b.w;
                        shadow_blend(pixels, w->shadow_mask + offset, chunk_end - x, shadow);
                    }

                    pixels += chunk_end - x;
                    x = chunk_end;
                }
            } else {
                /* Allocation failure retains the same appearance without a cache. */
                for (; x < end; x++, pixels++) {
                    dist = MIN(x - b.x, b.x + b.w - 1 - x);
                    unsigned h = falloff[clampi((dist + radius) * 64 / radius, 0, 128)];
                    unsigned a = opacity[v][h];
                    if (a)
                        *pixels = mix(*pixels, shadow, a);
                }
            }
        }
    }
}

/* Compose front to back, visiting lower layers only where they are visible.
   The central rectangle is fully opaque, including the title and body. The
   narrow side strips retain normal corner/shadow composition. This avoids
   rewriting wallpaper and covered windows underneath every moving window. */
static void compose_region(Rect r, const int *order, int layer, unsigned *budget) {
    if (empty(r)) {
        return;
    }
    if (layer < 0 || (*budget)++ >= 256) {
        for (int y = r.y; y < r.y + r.h; y++) {
            memcpy(desk.scene + (size_t)y * desk.width + r.x,
                   desk.wallpaper + (size_t)y * desk.width + r.x, r.w * 4);
        }
        clip_set(r);
        for (int i = 0; i <= layer; i++) {
            wm_draw(&desk.windows[order[i]]);
        }
        return;
    }

    Window *w = &desk.windows[order[layer]];
    int radius = w->maximized ? 0 : S(9);
    Rect opaque =
        intersect(r, (Rect){w->box.x + radius, w->box.y, w->box.w - radius * 2, w->box.h});
    if (empty(opaque)) {
        compose_region(r, order, layer - 1, budget);
        clip_set(r);
        wm_draw(w);
        return;
    }

    Rect visible[4] = {
        {r.x, r.y, r.w, opaque.y - r.y},
        {r.x, opaque.y + opaque.h, r.w, r.y + r.h - opaque.y - opaque.h},
        {r.x, opaque.y, opaque.x - r.x, opaque.h},
        {opaque.x + opaque.w, opaque.y, r.x + r.w - opaque.x - opaque.w, opaque.h},
    };

    for (unsigned i = 0; i < 4; i++) {
        if (!empty(visible[i])) {
            compose_region(visible[i], order, layer - 1, budget);
            clip_set(visible[i]);
            wm_draw(w);
        }
    }

    clip_set(opaque);
    wm_draw(w);
}

long desktop_present(void) {
    struct os_display_batch batch = {.pixels = (uintptr_t)desk.output, .stride = desk.width};
    Rect cursor_old = {desk.old_mouse_x, desk.old_mouse_y, S(15), S(21)},
         cursor_new = {desk.mouse_x, desk.mouse_y, S(15), S(21)};
    if (desk.mouse_x != desk.old_mouse_x || desk.mouse_y != desk.old_mouse_y) {
        damage(cursor_old);
        damage(cursor_new);
    }
    if (!desk.dirty_count) {
        return 0;
    }
    uint64_t started = render_time();
    if (started < desk.frame_at) {
        return 0;
    }
    /* Keep deadlines on a stable cadence. Reset after idle/overrun rather
       than accumulating one polling/scheduling delay on every moving frame. */
    if (!desk.frame_at || started - desk.frame_at >= 8333)
        desk.frame_at = started + 8333;
    else
        desk.frame_at += 8333;
    desk.paint = desk.scene;
    desk.paint_stride = desk.width;
    desk.paint_x = desk.paint_y = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &desk.windows[i];
        if (w->alive && !w->minimized && w->workspace == desk.workspace) {
            update_surface(w);
        }
    }

    int order[MAX_WINDOWS], count = 0;
    for (int j = 0; j < MAX_WINDOWS; j++) {
        Window *w = &desk.windows[j];
        if (w->alive && !w->minimized && w->workspace == desk.workspace) {
            order[count++] = j;
        }
    }

    for (int a = 1; a < count; a++) {
        int v = order[a], b = a;
        while (b && desk.windows[order[b - 1]].z > desk.windows[v].z) {
            order[b] = order[b - 1];
            b--;
        }
        order[b] = v;
    }

    int n = desk.dirty_count;
    Rect dirty[32];
    memcpy(dirty, desk.dirty, n * sizeof(Rect));
    desk.dirty_count = 0;
    for (int i = 0; i < n; i++) {
        Rect r = dirty[i];
        unsigned budget = 0;
        compose_region(r, order, count - 1, &budget);
        clip_set(r);
        taskbar_draw();
        if (desk.launcher) {
            launcher_draw();
        }
        if (desk.menu.open) {
            menu_draw();
        }
        if (desk.dialog.open) {
            dialog_draw();
        }
    }

    uint32_t cursor_saved[64 * 64];
    Rect saved_cursor = intersect(cursor_new, (Rect){0, 0, desk.width, desk.height});
    for (int y = 0; y < saved_cursor.h; y++) {
        memcpy(cursor_saved + y * 64,
               desk.scene + (size_t)(saved_cursor.y + y) * desk.width + saved_cursor.x,
               saved_cursor.w * 4);
    }
    cursor();
    for (int i = 0; i < n; i++) {
        Rect r = dirty[i];
        batch.rects[batch.count++] = (struct os_damage_rect){r.x, r.y, r.w, r.h};
        desk.presented_pixels += (uint64_t)r.w * r.h;
        if (r.w == desk.width && r.h == desk.height) {
            desk.full_frames++;
        }
    }

    desk.old_mouse_x = desk.mouse_x;
    desk.old_mouse_y = desk.mouse_y;
    uint64_t composed = render_time();
    long e = call3(SYS_display, desk.canvas_present ? DISPLAY_PRESENT_SWAP : DISPLAY_PRESENT_BATCH,
                   &batch, desk.canvas_present ? sizeof(batch) : OS_DISPLAY_LEGACY_BATCH_SIZE);
    uint64_t finished = render_time();
    for (int y = 0; y < saved_cursor.h; y++) {
        memcpy(desk.scene + (size_t)(saved_cursor.y + y) * desk.width + saved_cursor.x,
               cursor_saved + y * 64, saved_cursor.w * 4);
    }
    desk.compose_us += composed - started;
    desk.upload_us += finished - composed;
    if (desk.drag_moved) {
        desk.drag_frames++;
        desk.drag_us += finished - started;
        desk.drag_max_us = MAX(desk.drag_max_us, finished - started);
        if (desk.drag_frame_at) {
            uint64_t interval = started - desk.drag_frame_at;
            desk.drag_intervals++;
            desk.drag_interval_us += interval;
            desk.drag_interval_max = MAX(desk.drag_interval_max, interval);
            desk.drag_interval_histogram[MIN(interval / 1000, 127)]++;
        }

        desk.drag_frame_at = started;
    } else {
        desk.drag_frame_at = 0;
    }
    desk.drag_moved = false;
    desk.present_calls++;
    desk.frames++;
    clip_set((Rect){0, 0, desk.width, desk.height});
    return e;
}
