#include "desktop.h"
#include "font_data.h"
#include "pixel_blend.h"
#include "text_input_model.h"

int S(int n) { return (n * desk.scale + 50) / 100; }

int clampi(int64_t v, int low, int high) {
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

bool empty(Rect r) { return r.w <= 0 || r.h <= 0; }

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
  unsigned doe = z - era * 146097,
           yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100),
           mp = (5 * doy + 2) / 153;
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
  return call2(SYS_clock_gettime, 1, &ts)
             ? 0
             : (uint64_t)ts.sec * 1000 + ts.nsec / 1000000;
}

void trace(const char *event, Window *w, const char *detail) {
  if (desk.trace) {
    printf("DESKTOP %s window=%d workspace=%d %s\n", event, w ? w->id : 0,
           desk.workspace + 1, detail ? detail : "");
  }
}

void trace_scroll(Window *w, const char *app, int position, int target) {
  if (desk.trace)
    printf("DESKTOP SCROLL window=%d workspace=%d %s position=%d target=%d "
           "time=%llu\n",
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
    w->surface_damage = (Rect){x, y, MAX(a.x + a.w, r.x + r.w) - x,
                               MAX(a.y + a.h, r.y + r.h) - y};
  }
}

/* Keep damage disjoint. Bounding an L-shaped border with one rectangle turns
   every resize into a multi-megapixel repaint and blends overlaps twice if
   callers try to retain the separate strips themselves. */
static int damage_subtract(Rect r,Rect cut,Rect out[4]) {
  Rect hit=intersect(r,cut);
  if(empty(hit)){out[0]=r;return 1;}
  Rect bands[4]={{r.x,r.y,r.w,hit.y-r.y},
    {r.x,hit.y+hit.h,r.w,r.y+r.h-hit.y-hit.h},
    {r.x,hit.y,hit.x-r.x,hit.h},
    {hit.x+hit.w,hit.y,r.x+r.w-hit.x-hit.w,hit.h}};
  int n=0;for(int i=0;i<4;i++)if(!empty(bands[i]))out[n++]=bands[i];
  return n;
}
void compositor_damage(Rect r) {
  r=intersect(r,(Rect){0,0,desk.width,desk.height});
  if(empty(r))return;
  Rect pending[32]={r};int count=1;
  for(int i=0;i<desk.dirty_count && count;i++) {
    Rect next[32];int n=0;
    for(int j=0;j<count;j++) {
      Rect pieces[4];int parts=damage_subtract(pending[j],desk.dirty[i],pieces);
      if(n+parts>32)goto full;
      for(int k=0;k<parts;k++)next[n++]=pieces[k];
    }
    memcpy(pending,next,n*sizeof(Rect));count=n;
  }
  if(desk.dirty_count+count>32)goto full;
  for(int i=0;i<count;i++)desk.dirty[desk.dirty_count++]=pending[i];
  /* Adjacent strips merge only when their exact union is rectangular. */
  for(int i=0;i<desk.dirty_count;i++)for(int j=i+1;j<desk.dirty_count;j++) {
    Rect a=desk.dirty[i],b=desk.dirty[j];
    if((a.x==b.x && a.w==b.w && (a.y+a.h==b.y || b.y+b.h==a.y)) ||
       (a.y==b.y && a.h==b.h && (a.x+a.w==b.x || b.x+b.w==a.x))) {
      int x=MIN(a.x,b.x),y=MIN(a.y,b.y);
      desk.dirty[i]=(Rect){x,y,MAX(a.x+a.w,b.x+b.w)-x,MAX(a.y+a.h,b.y+b.h)-y};
      desk.dirty[j]=desk.dirty[--desk.dirty_count];i=-1;break;
    }
  }
  return;
full:
  desk.dirty_count=1;desk.dirty[0]=(Rect){0,0,desk.width,desk.height};
}

void damage_native_resize(Window *w,Rect before) {
  Rect after=w->box;
  int right=MIN(before.x+before.w,after.x+after.w),bottom=MIN(before.y+before.h,after.y+after.h);
  int width=MAX(before.w,after.w),height=MAX(before.h,after.h);
  w->surface_damage=(Rect){0,0,after.w,S(WINDOW_HEADER)};
  compositor_damage((Rect){after.x-S(34),after.y-S(14),width+S(68),S(WINDOW_HEADER+48)});
  compositor_damage((Rect){right-S(48),after.y-S(14),
    MAX(before.w,after.w)-MIN(before.w,after.w)+S(96),height+S(76)});
  compositor_damage((Rect){after.x-S(34),bottom-S(48),width+S(68),
    MAX(before.h,after.h)-MIN(before.h,after.h)+S(110)});
}

void invalidate(Rect r) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *w = &desk.windows[i];
    if (w->alive && !w->minimized && w->workspace == desk.workspace) {
      surface_damage(w, r);
    }
  }

  compositor_damage(r);
}

void damage_window(Window *w) {
  if (w && w->alive && !w->minimized && w->workspace == desk.workspace) {
    compositor_damage((Rect){w->box.x - S(34), w->box.y - S(14),
                             w->box.w + S(68), w->box.h + S(62)});
  }
}

void invalidate_window(Window *w) {
  if (!w || !w->alive) {
    return;
  }
  w->surface_damage = (Rect){0, 0, w->box.w, w->box.h};
  if (w->id == -1) glass_reset();
  damage_window(w);
}

void invalidate_window_region(Window *w, Rect r) {
  if (!w || !w->alive || w->minimized || w->workspace != desk.workspace) return;
  r = intersect(r, w->box);
  surface_damage(w, r);
  compositor_damage(r);
}

void invalidate_all(void) {
  desk.material_epoch++;
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
  /* Cached surfaces use their own coordinates and may extend off screen.
     Their callers already intersect each child clip with the surface clip. */
  desk.clip = desk.paint_alpha ? r :
      intersect(r, (Rect){0, 0, desk.width, desk.height});
}

uint32_t *paint_pixel(int x, int y) {
  return desk.paint + (size_t)(y - desk.paint_y) * desk.paint_stride + x -
         desk.paint_x;
}

static uint32_t mix(uint32_t bg, uint32_t fg, unsigned a) {
  unsigned b = 255 - a;
  return ((((bg & 0xff00ff) * b + (fg & 0xff00ff) * a + 0x800080) >> 8) &
          0xff00ff) |
         ((((bg & 0x00ff00) * b + (fg & 0x00ff00) * a + 0x008000) >> 8) &
          0x00ff00);
}

/* Cached glass foreground is premultiplied ARGB. Opaque content still uses
   the existing fast memcpy path; alpha is needed only over material bands. */
static uint32_t paint_over(uint32_t bg, uint32_t fg, unsigned alpha) {
  if (!alpha)
    return bg;
  if (alpha >= 255)
    return desk.paint_alpha ? (fg | 0xff000000) : fg;
  uint32_t rgb = coverage_over(fg, bg, alpha);
  if (!desk.paint_alpha)
    return rgb;
  unsigned old = bg >> 24, out = alpha + (old * (255 - alpha) + 127) / 255;
  return rgb | (out << 24);
}

void fill(Rect r, uint32_t color) {
  if (desk.paint_alpha)
    color |= 0xff000000;
  r = intersect(r, desk.clip);
  for (int y = r.y; y < r.y + r.h; y++) {
    uint32_t *row = paint_pixel(r.x, y);
    for (int x = 0; x < r.w; x++) {
      row[x] = color;
    }
  }
}

void blend(Rect r, uint32_t color, unsigned alpha) {
  if (!alpha)
    return;
  if (alpha >= 255) {
    fill(r, color);
    return;
  }
  r = intersect(r, desk.clip);
  for (int y = r.y; y < r.y + r.h; y++) {
    uint32_t *row = paint_pixel(r.x, y);
    for (int x = 0; x < r.w; x++) {
      row[x] = paint_over(row[x], color, alpha);
    }
  }
}

void rounded(Rect r, int radius, uint32_t color) {
  radius = MAX(0, MIN(radius, MIN(r.w, r.h) / 2));
  Rect visible = intersect(r, desk.clip);
  for (int y = visible.y; y < visible.y + visible.h; y++) {
    int dy = MIN(y - r.y, r.y + r.h - 1 - y);
    if (dy >= radius || !radius) {
      fill((Rect){visible.x, y, visible.w, 1}, color);
      continue;
    }
    int left = MIN(visible.x + visible.w, r.x + radius);
    for (int x = visible.x; x < left; x++)
      blend((Rect){x, y, 1, 1}, color, rect_coverage(r, radius, x, y));
    int mid = MAX(visible.x, r.x + radius),
        end = MIN(visible.x + visible.w, r.x + r.w - radius);
    if (end > mid)
      fill((Rect){mid, y, end - mid, 1}, color);
    for (int x = MAX(visible.x, r.x + r.w - radius); x < visible.x + visible.w;
         x++)
      blend((Rect){x, y, 1, 1}, color, rect_coverage(r, radius, x, y));
  }
}

void border(Rect r, int radius, uint32_t color) {
  Rect v = intersect(r, desk.clip),
       inner = {r.x + 1, r.y + 1, r.w - 2, r.h - 2};
  radius = MAX(0, MIN(radius, MIN(r.w, r.h) / 2));
  for (int y = v.y; y < v.y + v.h; y++) {
    if (y >= r.y + radius + 1 && y < r.y + r.h - radius - 1) {
      fill((Rect){r.x, y, 1, 1}, color);
      fill((Rect){r.x + r.w - 1, y, 1, 1}, color);
      continue;
    }
    for (int x = v.x; x < v.x + v.w; x++) {
      unsigned outer = rect_coverage(r, radius, x, y);
      unsigned inside = rect_coverage(inner, MAX(0, radius - 1), x, y);
      if (outer > inside)
        blend((Rect){x, y, 1, 1}, color, outer - inside);
    }
  }
}

void line(int x1, int y1, int x2, int y2, uint32_t color) {
  int dx = x2 - x1, dy = y2 - y1;
  if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) {
    if (x1 > x2) {
      int t = x1;
      x1 = x2;
      x2 = t;
      t = y1;
      y1 = y2;
      y2 = t;
    }
    for (int x = x1; x <= x2; x++) {
      int64_t fy =
          (int64_t)y1 * 256 +
          (x2 == x1 ? 0 : (int64_t)(y2 - y1) * (x - x1) * 256 / (x2 - x1));
      int y = fy >> 8;
      unsigned fraction = fy & 255;
      blend((Rect){x, y, 1, 1}, color, 255 - fraction);
      if (fraction)
        blend((Rect){x, y + 1, 1, 1}, color, fraction);
    }
  } else {
    if (y1 > y2) {
      int t = x1;
      x1 = x2;
      x2 = t;
      t = y1;
      y1 = y2;
      y2 = t;
    }
    for (int y = y1; y <= y2; y++) {
      int64_t fx =
          (int64_t)x1 * 256 + (int64_t)(x2 - x1) * (y - y1) * 256 / (y2 - y1);
      int x = fx >> 8;
      unsigned fraction = fx & 255;
      blend((Rect){x, y, 1, 1}, color, 255 - fraction);
      if (fraction)
        blend((Rect){x + 1, y, 1, 1}, color, fraction);
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

int text_width_limit(const char *text, size_t bytes, Font font) {
  unsigned sum = 0, index = desk.scale_index * FONT_COUNT + font;
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
  const struct Glyph *g =
      &gui_glyphs[desk.scale_index * FONT_COUNT + font][c - 32];
  Rect r = {x + g->left, y + g->top, g->w, g->h},
       visible = intersect(r, desk.clip);
  for (int yy = visible.y; yy < visible.y + visible.h; yy++) {
    for (int xx = visible.x; xx < visible.x + visible.w; xx++) {
      unsigned a = gui_font_pixels[g->offset + (yy - r.y) * g->w + xx - r.x];
      if (a) {
        uint32_t *p = paint_pixel(xx, yy);
        *p = paint_over(*p, color, a);
      }
    }
  }
}

void label(int x, int y, const char *text, Font font, uint32_t color,
           int max_width) {
  if (max_width <= 0)
    return;
  int pen = 0, index = desk.scale_index * FONT_COUNT + font;
  /* text_width rounds 26.6 advances to the nearest pixel. Use the same
     boundary here, so a measured label retains its final glyph. */
  int limit = max_width * 64 + 31;
  bool shortened = text_width(text, font) > max_width;
  int dot_advance = gui_glyphs[index]['.' - 32].advance,
      dot_count = shortened ? 2 : 0;
  while (dot_count && (dot_count * dot_advance + 32) / 64 > max_width)
    dot_count--;
  int dots = dot_count * dot_advance;
  Rect saved = desk.clip;
  clip_set(intersect(saved, (Rect){x, saved.y, max_width, saved.h}));
  while (*text) {
    unsigned c = next_char(&text);
    int advance = gui_glyphs[index][c - 32].advance;
    if (pen + advance > limit - dots)
      break;
    glyph(x + (pen + 32) / 64, y, c, font, color);
    pen += advance;
  }
  for (int i = 0; i < dot_count; i++) {
    glyph(x + (pen + 32) / 64, y, '.', font, color);
    pen += dot_advance;
  }
  clip_set(saved);
}

void button(Rect r, const char *text, Icon kind, bool primary, bool enabled,
            bool focused) {
  bool hover = enabled && contains(r, desk.mouse_x, desk.mouse_y);
  bool pressed = hover && (desk.buttons & 1);
  uint32_t bg = !enabled  ? THEME_TOOLBAR
                : primary ? (hover ? THEME_ACCENT_HOVER : desk.accent)
                : pressed ? desk.selection
                : hover   ? theme_color(0xEDF1EF)
                          : THEME_TOOLBAR;
  rounded(r, S(5), bg);
  if (primary && focused) {
    border((Rect){r.x - 2, r.y - 2, r.w + 4, r.h + 4}, S(6),
           theme_color(0x96B5A9));
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
  label(x, r.y + (r.h - S(14)) / 2, text, FONT_MEDIUM, fg,
        r.x + r.w - x - S(6));
}

static void input_draw_style(Rect r, const char *value, const char *placeholder, int cursor,
                int anchor, bool focused, bool material) {
  unsigned font=material?FONT_TITLE:FONT_NORMAL;
  int height=material?20:14;
  if (cursor < 0) {
    cursor = strlen(value);
    anchor = 0;
  }
  cursor = input_position(value, cursor);
  anchor = input_position(value, anchor);
  if(!material) {
    rounded(r, S(5), THEME_WINDOW);
    border(r, S(5), focused ? desk.accent : theme_color(0xE5E9E8));
  }
  Rect saved = desk.clip;
  clip_set(intersect(saved, (Rect){r.x + S(6), r.y + 2, r.w - S(12), r.h - 4}));
  int x = r.x + S(8), y = r.y + (r.h - S(height)) / 2;
  if (value[0]) {
    int advance = text_width_limit(value, cursor, font);
    int shift = MAX(0, advance - r.w + S(20));
    x -= shift;
    if (focused && cursor != anchor) {
      int a = text_width_limit(value, MIN(cursor, anchor), font);
      int b = text_width_limit(value, MAX(cursor, anchor), font);
      fill((Rect){x + a, y, b - a, S(height+1)}, desk.selection);
    }
    label(x, y, value, font, theme_color(0x41525A), r.w + shift);
    if (focused) {
      fill((Rect){x + advance, y + S(2), 1, S(height-3)}, desk.accent);
    }
  } else {
    label(x, y, placeholder, font, theme_color(0x9BA5A8), r.w - S(16));
    if (focused) {
      fill((Rect){x, y + S(2), 1, S(height-3)}, desk.accent);
    }
  }

  desk.clip = saved;
}

void input_draw(Rect r,const char *value,const char *placeholder,int cursor,int anchor,bool focused) {
  input_draw_style(r,value,placeholder,cursor,anchor,focused,false);
}
void input_material_draw(Rect r,const char *value,const char *placeholder,int cursor,int anchor,bool focused) {
  input_draw_style(r,value,placeholder,cursor,anchor,focused,true);
}

bool input_key(char *value, size_t capacity, int *cursor, int *anchor, Key k) {
  if (!k.down)
    return false;
  *cursor = input_position(value, *cursor);
  *anchor = input_position(value, *anchor);
  int first = MIN(*cursor, *anchor), last = MAX(*cursor, *anchor);
  unsigned letter =
      k.code >= 4 && k.code <= 0x1d ? 'a' + k.code - 4 : k.ascii | 32;
  if ((k.mods & MOD_CTRL) && letter == 'a') {
    *cursor = strlen(value);
    *anchor = 0;
    return true;
  }
  if ((k.mods & MOD_CTRL) && (letter == 'c' || letter == 'x')) {
    if (last != first) {
      int error = clipboard_write(value + first, last - first);
      if (error)
        clipboard_error(wm_focused(), "Copy text", error);
      else {
        trace("CLIPBOARD_COPY", wm_focused(), "Text field selection");
        if (letter == 'x')
          input_replace(value, capacity, cursor, anchor, "", 0);
      }
    }
    return true;
  }
  if (((k.mods & MOD_CTRL) && letter == 'v') ||
      ((k.mods & MOD_SHIFT) && k.code == KEY_INSERT)) {
    char *copy;
    size_t length;
    int error = clipboard_read(&copy, &length);
    if (error) {
      clipboard_error(wm_focused(), "Paste text", error);
      return true;
    }
    /* Single-line controls never turn pasted line breaks into actions. */
    for (size_t i = 0; i < length; i++)
      if (copy[i] == '\r' || copy[i] == '\n' || copy[i] == '\t')
        copy[i] = ' ';
    if (length && !input_replace(value, capacity, cursor, anchor, copy, length))
      dialog_notice(
          wm_focused(), "Paste text",
          "The text does not fit in this field. The field was left unchanged.");
    clipboard_release(copy);
    trace("CLIPBOARD_PASTE", wm_focused(), "Text field");
    return true;
  }
  int target = *cursor;
  if (k.code == KEY_LEFT)
    target = !(k.mods & MOD_SHIFT) && first != last
                 ? first
                 : input_previous(value, *cursor);
  else if (k.code == KEY_RIGHT)
    target = !(k.mods & MOD_SHIFT) && first != last
                 ? last
                 : input_next(value, *cursor);
  else if (k.code == KEY_HOME)
    target = 0;
  else if (k.code == KEY_END)
    target = strlen(value);
  else if (k.code == KEY_BACKSPACE || k.code == KEY_DELETE) {
    if (first == last)
      *anchor = k.code == KEY_BACKSPACE ? input_previous(value, *cursor)
                                        : input_next(value, *cursor);
    input_replace(value, capacity, cursor, anchor, "", 0);
    return true;
  } else if (k.ascii >= 32 && k.ascii < 127 &&
             !(k.mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))) {
    char c = k.ascii;
    input_replace(value, capacity, cursor, anchor, &c, 1);
    return true;
  } else
    return false;
  *cursor = target;
  if (!(k.mods & MOD_SHIFT))
    *anchor = target;
  return true;
}

void scrollbar(Rect viewport, int total, int scroll) {
  if (total <= viewport.h) {
    return;
  }
  int h = MAX(S(24), (int)((int64_t)viewport.h * viewport.h / total));
  int y = viewport.y +
          (int)((int64_t)scroll * (viewport.h - h) / (total - viewport.h));
  rounded((Rect){viewport.x + viewport.w - S(6), y, S(3), h}, S(2),
          theme_color(0xC3CECA));
}

static uint64_t render_time(void) {
  struct os_timespec t;
  if (call2(SYS_clock_gettime, 1, &t)) {
    return 0;
  }
  return t.sec * 1000000ULL + t.nsec / 1000;
}

void wm_free_surface(Window *w) {
  glass_release(&w->glass[0]);
  glass_release(&w->glass[1]);
  deallocate(w->surface, (size_t)w->surface_stride * w->surface_rows * 4);
  w->surface = NULL;
  w->surface_stride = w->surface_rows = 0;
}

static void update_surface(Window *w) {
  /* Native clients already retain their complete RGB canvas. Cache only the
     decorations instead of repainting and alpha-marking a second body. */
  bool direct = w->app == APP_WORKBENCH || w->app==APP_QT || w->app==APP_CHROMIUM;
  int rows = direct ? S(WINDOW_HEADER) : w->box.h;
  bool resized = w->surface_width != w->box.w || w->surface_height != w->box.h;
  if (!w->surface || w->box.w > w->surface_stride || rows > w->surface_rows) {
    wm_free_surface(w);
    /* Reuse capacity across pointer samples instead of unmapping/mapping a
       multi-megabyte bitmap for every pixel of a live resize. */
    w->surface_stride = MIN(desk.width, (w->box.w + 127) & ~127);
    w->surface_rows = direct ? rows : MIN(desk.height, (rows + 127) & ~127);
    w->surface = allocate((size_t)w->surface_stride * w->surface_rows * 4);
  }
  if (resized || !w->surface) {
    w->surface_width = w->box.w;
    w->surface_height = w->box.h;
    w->surface_damage = (Rect){0, 0, w->box.w, w->box.h};
  }

  bool focused = wm_focused() == w, pointer_active = wm_pointer_active(w);
  if (focused != w->surface_focused || w->maximized != w->surface_maximized ||
      pointer_active != w->surface_pointer || w->fullscreen != w->surface_fullscreen) {
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
  desk.paint_stride = w->surface_stride;
  desk.paint_x = w->box.x;
  desk.paint_y = w->box.y;
  desk.paint_alpha = true;
  clip_set(direct ? intersect(r, (Rect){w->box.x, w->box.y, w->box.w,
                                       w->fullscreen ? 0 : rows}) : r);
  wm_paint(w);
  desk.paint_alpha = false;
  /* Browser/game images are copied as RGB by existing apps. Mark just the
     non-material body opaque without touching premultiplied glass text. */
  Rect sidebar = window_glass_rect(w, 1);
  for (int y = direct ? r.y + r.h : MAX(r.y, window_body(w).y); y < r.y + r.h; y++) {
    int left = r.x;
    if (!empty(sidebar) && y >= sidebar.y && y < sidebar.y + sidebar.h)
      left = MAX(left, sidebar.x + sidebar.w);
    for (int x = left; x < r.x + r.w; x++)
      *paint_pixel(x, y) |= 0xff000000;
  }
  w->surface_revision++;
  /* Backdrop filters depend on the pixels they sample, not on unrelated
     typing elsewhere in a large window. Preserve generations per region. */
  int tile_w=(w->box.w+SURFACE_GRID-1)/SURFACE_GRID;
  int tile_h=(w->box.h+SURFACE_GRID-1)/SURFACE_GRID;
  for (int y=(r.y-w->box.y)/tile_h; y<=(r.y+r.h-1-w->box.y)/tile_h; y++)
    for (int x=(r.x-w->box.x)/tile_w; x<=(r.x+r.w-1-w->box.x)/tile_w; x++)
      w->surface_tiles[y*SURFACE_GRID+x]=w->surface_revision;
  desk.clip = saved;
  desk.paint = desk.scene;
  desk.paint_stride = desk.width;
  desk.paint_x = desk.paint_y = 0;
  w->surface_damage = (Rect){0};
  w->surface_focused = focused;
  w->surface_pointer = pointer_active;
  w->surface_maximized = w->maximized;
  w->surface_fullscreen = w->fullscreen;
  desk.surface_updates++;
}

void wm_draw(Window *w) {
  Rect b = w->box, outer = {b.x - S(34), b.y - S(14), b.w + S(68), b.h + S(62)};
  if (empty(intersect(desk.clip, outer))) {
    return;
  }
  if (!w->maximized && !w->fullscreen) {
    window_shadow(w,S(WINDOW_CORNER));
  }
  Rect r = intersect(desk.clip, b);
  int radius = w->maximized || w->fullscreen ? 0 : S(WINDOW_CORNER);
  uint32_t corner_backdrop[4][18 * 18];
  for (int c = 0; c < 4; c++)
    for (int yy = 0; yy < radius; yy++)
      for (int xx = 0; xx < radius; xx++) {
        int px = (c & 1) ? b.x + b.w - 1 - xx : b.x + xx,
            py = (c & 2) ? b.y + b.h - 1 - yy : b.y + yy;
        if (contains(r, px, py))
          corner_backdrop[c][yy * 18 + xx] = *paint_pixel(px, py);
      }
  if (!w->surface) {
    /* Allocation failure must retain the same rounded silhouette. */
    wm_paint(w);
  } else {
    int cached_end = (w->app == APP_WORKBENCH || w->app==APP_QT || w->app==APP_CHROMIUM)
                         ? MIN(r.y + r.h, w->fullscreen ? b.y : b.y + S(WINDOW_HEADER))
                         : r.y + r.h;
    for (int y = r.y; y < cached_end; y++) {
      int x = r.x, end = r.x + r.w;
      if (end > x) {
        GlassCache *material = NULL;
        if (!w->fullscreen && y < w->box.y + S(WINDOW_HEADER))
          material = &w->glass[0];
        else if (!empty(w->glass[1].rect) && y >= w->glass[1].rect.y &&
                 y < w->glass[1].rect.y + w->glass[1].rect.h)
          material = &w->glass[1];
        const uint32_t *source = w->surface + (size_t)(y - b.y) * w->surface_stride + x - b.x;
        uint32_t *target = paint_pixel(x, y);
        if (!material)
          memcpy(target, source, (end - x) * 4);
        else {
          int left = MAX(x, material->rect.x),
              right = MIN(end, material->rect.x + material->rect.w);
          if (left > x)
            memcpy(target, source, (left - x) * 4);
          for (int px = left; px < right; px++) {
            uint32_t fg = source[px - x];
            uint32_t bg = material->pixels
                              ? material->pixels[(size_t)(y - material->rect.y) *
                                                     material->rect.w +
                                                 px - material->rect.x]
                              : THEME_TOOLBAR;
            target[px - x] = (fg >> 24) == 255 ? fg
                             : (fg >> 24)      ? glass_over(fg, bg)
                                               : bg;
          }
          if (right < end)
            memcpy(target + MAX(0, right - x), source + MAX(0, right - x),
                   (end - MAX(x, right)) * 4);
        }
      }
    }
    if (w->app == APP_WORKBENCH || w->app==APP_QT || w->app==APP_CHROMIUM) {
      Rect saved = desk.clip;
      clip_set(intersect(saved, window_body(w)));
      apps[w->app].draw(w);
      desk.clip = saved;
      if (!w->fullscreen)
        border(b, radius,
               wm_focused() == w ? THEME_WINDOW_BORDER : theme_color(0xE4E9E7));
    }
  }
  for (int c = 0; c < 4; c++)
    for (int yy = 0; yy < radius; yy++)
      for (int xx = 0; xx < radius; xx++) {
        int px = (c & 1) ? b.x + b.w - 1 - xx : b.x + xx,
            py = (c & 2) ? b.y + b.h - 1 - yy : b.y + yy;
        if (contains(r, px, py)) {
          uint32_t *target = paint_pixel(px, py);
          *target = coverage_over(*target, corner_backdrop[c][yy * 18 + xx],
                                  corner_coverage(radius, xx, yy));
        }
      }
}

/* Rounded shadow profiles are shared by all windows and Spotlight. Only the
   exposed perimeter is painted, including the cutouts INSIDE rounded corners.
   A rectangular hole here leaves a visible square notch against the shadow.
   Fixed-size profiles also avoid allocating/rebuilding a mask on live resize. */
enum { SHADOW_EXTENT = 96 };
typedef struct {
  int radius, blur, scale;
  uint8_t edge[121], corner[SHADOW_EXTENT + 1][SHADOW_EXTENT + 1];
} ShadowProfile;
static ShadowProfile shadow_profiles[2];

static unsigned shadow_falloff(int distance16, int blur) {
  unsigned t = clampi((blur * 16 - distance16) * 128 / (blur * 16), 0, 256);
  return (uint64_t)t * t * (768 - 2 * t) * 72 / 16777216;
}
static ShadowProfile *shadow_profile(int radius) {
  ShadowProfile *p = &shadow_profiles[radius == S(WINDOW_CORNER) ? 0 : 1];
  int blur = clampi(S(30), 1, 60);
  radius = clampi(radius, 0, 32);
  if (p->scale != desk.scale || p->radius != radius || p->blur != blur) {
    p->scale = desk.scale;
    p->radius = radius;
    p->blur = blur;
    for (int d = -blur; d <= blur; d++)
      p->edge[d + blur] = shadow_falloff(d * 16, blur);
    for (int y = 0; y <= radius + blur; y++)
      for (int x = 0; x <= radius + blur; x++) {
        float square = x * x + y * y, distance;
        __asm__("sqrtss %1,%0" : "=x"(distance) : "x"(square));
        p->corner[y][x] = shadow_falloff((int)(distance * 16 + .5f) - radius * 16, blur);
      }
  }
  return p;
}
static unsigned shadow_alpha(const ShadowProfile *p, Rect b, int x, int y) {
  int dx = MAX(b.x + p->radius - x, x - (b.x + b.w - 1 - p->radius));
  int dy = MAX(b.y + p->radius - y, y - (b.y + b.h - 1 - p->radius));
  if (dx > 0 && dy > 0) {
    if (dx > p->radius + p->blur || dy > p->radius + p->blur)
      return 0;
    return p->corner[dy][dx];
  }
  int distance = MAX(dx, dy) - p->radius;
  return p->edge[clampi(distance + p->blur, 0, 2 * p->blur)];
}
static void shadow_span(const ShadowProfile *p, Rect b, int x, int end, int y,
                        uint32_t color) {
  if (end <= x) return;
  int middle = b.x + p->radius + p->blur;
  int right = b.x + b.w - p->radius - p->blur;
  uint32_t *pixels = paint_pixel(x, y);
  for (; x < end && x < middle; x++, pixels++) {
    unsigned alpha = shadow_alpha(p, b, x, y);
    if (alpha) *pixels = mix(*pixels, color, alpha);
  }
  if (x < end && x < right) {
    int count = MIN(end, right) - x;
    pixels_blend(pixels, count, color, shadow_alpha(p, b, x, y));
    pixels += count;
    x += count;
  }
  for (; x < end; x++, pixels++) {
    unsigned alpha = shadow_alpha(p, b, x, y);
    if (alpha) *pixels = mix(*pixels, color, alpha);
  }
}
void window_shadow(Window *w, int radius) {
  Rect b = w->box, outer = {b.x - S(34), b.y - S(14), b.w + S(68), b.h + S(62)};
  Rect r = intersect(outer, desk.clip);
  radius = MIN(radius, MIN(b.w, b.h) / 2);
  if (empty(r)) return;
  if (r.x >= b.x && r.x + r.w <= b.x + b.w &&
      r.y >= b.y + radius && r.y + r.h <= b.y + b.h - radius)
    return;
  ShadowProfile *profile = shadow_profile(radius);
  uint32_t color = THEME_SHADOW;
  Rect caster = b;
  caster.y += S(18);
  for (int y = r.y; y < r.y + r.h; y++) {
    if (y < b.y || y >= b.y + b.h) {
      shadow_span(profile, caster, r.x, r.x + r.w, y, color);
      continue;
    }
    int cutout = y < b.y + radius || y >= b.y + b.h - radius ? radius : 0;
    shadow_span(profile, caster, r.x, MIN(r.x + r.w, b.x + cutout), y, color);
    shadow_span(profile, caster, MAX(r.x, b.x + b.w - cutout), r.x + r.w, y, color);
  }
}

/* Trace-only diagnostic, invoked before the next application tick. The
   retained native pixels and desktop state stay identical across both
   compositions, even if the client's own animations keep running. */
void desktop_verify_recompose(void) {
  desk.frame_at = 0;
  long error = desktop_present();
  size_t bytes = (size_t)desk.width * desk.height * 4;
  uint32_t *before = error ? NULL : allocate(bytes);
  if (!before) {
    printf("DESKTOP RECOMPOSE_CHECK error=%ld allocation=%u\n", error, !before);
    glass_reset();
    invalidate_all();
    return;
  }
  memcpy(before, desk.scene, bytes);
  glass_reset();
  invalidate_all();
  desk.frame_at = 0;
  error = desktop_present();
  uint64_t mismatches = 0;
  unsigned first_x = 0, first_y = 0;
  for (int y = 0; y < desk.height; y++)
    for (int x = 0; x < desk.width; x++) {
      size_t offset = (size_t)y * desk.width + x;
      if (before[offset] != desk.scene[offset]) {
        if (!mismatches) { first_x = x; first_y = y; }
        mismatches++;
      }
    }
  printf("DESKTOP RECOMPOSE_CHECK error=%ld pixels=%lu mismatches=%lu first=%u,%u same_retained_frame=1\n",
         error, (uint64_t)desk.width * desk.height, mismatches, first_x, first_y);
  deallocate(before, bytes);
}

/* Four disjoint bands, so translucent shadows are never blended twice. */
static int exposed_bands(Rect r, Rect opaque, Rect bands[4]) {
  Rect hit = intersect(r, opaque);
  if (empty(hit)) { bands[0]=r; return 1; }
  int count=0;
  Rect candidates[4]={{r.x,r.y,r.w,hit.y-r.y},
    {r.x,hit.y+hit.h,r.w,r.y+r.h-hit.y-hit.h},
    {r.x,hit.y,hit.x-r.x,hit.h},
    {hit.x+hit.w,hit.y,r.x+r.w-hit.x-hit.w,hit.h}};
  for (int i=0;i<4;i++) if (!empty(candidates[i])) bands[count++]=candidates[i];
  return count;
}

long desktop_present(void) {
  struct os_display_batch batch = {.pixels = (uintptr_t)desk.output,
                                   .stride = desk.width};
  Rect cursor_old = desk.cursor_rect, cursor_new = desktop_cursor_bounds();
  bool cursor_changed = desk.cursor_dirty ||
                        memcmp(&cursor_old, &cursor_new, sizeof(Rect));
  if (!desk.dirty_count && !cursor_changed) {
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
  int n = 0;
  Rect dirty[32];
  /* The retained scene never contains the cursor. Pointer-only updates need
     just two small uploads; leave window surfaces and backdrop filters alone. */
  if (desk.dirty_count) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
      Window *w = &desk.windows[i];
      if (w->alive && !w->minimized && w->workspace == desk.workspace) {
        update_surface(w);
      }
    }

    int order[MAX_WINDOWS], count = 0;
    for (int j = 0; j < MAX_WINDOWS; j++) {
      Window *w = &desk.windows[j];
      if (w->alive && !w->minimized && w->workspace == desk.workspace && !snap_window_hidden(w)) {
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

    for (int layer = 0; layer < count; layer++) {
      Window *w = &desk.windows[order[layer]];
      for (int index = 0; index < 2; index++) {
        Rect rect = window_glass_rect(w, index);
        glass_plan(&w->glass[index], rect, glass_layer_key(rect, order, layer));
        if (w->glass[index].pending)
          compositor_damage(desk.glass_disabled ? rect : w->glass[index].sample);
      }
    }
    glass_system_plan(order, count);
    glass_system_damage();
    /* An opaque native body replaces every pixel beneath it. Preserve all
       pending backdrop sample domains and the rounded edges, then avoid
       restoring wallpaper/drawing covered lower windows in its interior. */
    Rect opaque={0}; int opaque_layer=count;
    if (!desk.menu.open && !desk.dialog.open && !desk.launcher) {
      for (int layer=count-1;layer>=0;layer--) {
        Window *w=&desk.windows[order[layer]];
        /* Both clients arrive as fully composited RGB surfaces. Their body
           hides lower layers, even when the source buffer had alpha. */
        if (w->app!=APP_WORKBENCH && w->app!=APP_CHROMIUM) continue;
        Rect body=window_body(w);
        opaque=(Rect){body.x+S(WINDOW_CORNER),body.y+64,
          body.w-2*S(WINDOW_CORNER),body.h-128};
        opaque_layer=layer;
        for (int j=0;j<count;j++) for (int k=0;k<2;k++) {
          GlassCache *g=&desk.windows[order[j]].glass[k];
          if (!g->pending) continue;
          Rect bands[4];int n=exposed_bands(opaque,g->sample,bands);
          Rect largest={0};
          for (int i=0;i<n;i++) if ((int64_t)bands[i].w*bands[i].h>(int64_t)largest.w*largest.h)
            largest=bands[i];
          opaque=largest;
        }
        break;
      }
    }
    n = desk.dirty_count;
    memcpy(dirty, desk.dirty, n * sizeof(Rect));
    desk.dirty_count = 0;
    /* Every sample halo is reset before any layer is drawn: no previous-frame
       foreground, cursor or higher surface can leak into a backdrop filter. */
    for (int i = 0; i < n; i++) {
      Rect bands[4];int count=exposed_bands(dirty[i],opaque,bands);
      for (int j=0;j<count;j++) for (int y=bands[j].y;y<bands[j].y+bands[j].h;y++)
        memcpy(desk.scene+(size_t)y*desk.width+bands[j].x,
               desk.wallpaper+(size_t)y*desk.width+bands[j].x,bands[j].w*4);
    }
    for (int i = 0; i < n; i++) {
      Rect bands[4]; int pieces=exposed_bands(dirty[i], opaque, bands);
      for (int j=0;j<pieces;j++) { clip_set(bands[j]); files_desktop_draw(); }
    }
    for (int layer = 0; layer < count; layer++) {
      Window *w = &desk.windows[order[layer]];
      glass_build(&w->glass[0], 0);
      glass_build(&w->glass[1], w->app == APP_TERMINAL ? 6 : 1);
      for (int i = 0; i < n; i++) {
        Rect bands[4];int count=exposed_bands(dirty[i],layer<opaque_layer?opaque:(Rect){0},bands);
        for (int j=0;j<count;j++) {clip_set(bands[j]);wm_draw(w);}
      }
    }
    /* Complete each overlay across the full domain before a higher popup
       samples it; this also handles popups overlapping the Dock correctly. */
    for (int i = 0; i < n; i++) {
      clip_set(dirty[i]);
      if (!wm_fullscreen_active()) taskbar_draw();
    }
    if (desk.snap_open || desk.snap_preview || desk.snap_assist)
      for (int i = 0; i < n; i++) { clip_set(dirty[i]); snap_draw(); }
    if (desk.launcher)
      for (int i = 0; i < n; i++) {
        clip_set(dirty[i]);
        launcher_draw();
      }
    if (desk.menu.open)
      for (int i = 0; i < n; i++) {
        clip_set(dirty[i]);
        menu_draw();
      }
    if (desk.dialog.open)
      for (int i = 0; i < n; i++) {
        clip_set(dirty[i]);
        dialog_draw();
      }
  }

  memcpy(desk.dirty, dirty, n * sizeof(Rect));
  desk.dirty_count = n;
  if (cursor_changed) {
    compositor_damage(cursor_old);
    compositor_damage(cursor_new);
  }
  n = desk.dirty_count;
  memcpy(dirty, desk.dirty, n * sizeof(Rect));
  desk.dirty_count = 0;
  desk.cursor_rect = cursor_new;
  desk.cursor_dirty = false;
  if (!n) return 0;

  bool fading=transition_present();
  static uint32_t cursor_saved[128 * 128];
  Rect saved_cursor =
      intersect(cursor_new, (Rect){0, 0, desk.width, desk.height});
  for (int y = 0; y < saved_cursor.h; y++) {
    memcpy(cursor_saved + y * 128,
           desk.scene + (size_t)(saved_cursor.y + y) * desk.width +
               saved_cursor.x,
           saved_cursor.w * 4);
  }
  if (!game_pointer_captured())
    desktop_cursor_draw();
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
  long e =
      call3(SYS_display,
            desk.canvas_present ? DISPLAY_PRESENT_SWAP : DISPLAY_PRESENT_BATCH,
            &batch,
            desk.canvas_present ? sizeof(batch) : OS_DISPLAY_LEGACY_BATCH_SIZE);
  uint64_t finished = render_time();
  for (int y = 0; y < saved_cursor.h; y++) {
    memcpy(desk.scene + (size_t)(saved_cursor.y + y) * desk.width +
               saved_cursor.x,
           cursor_saved + y * 128, saved_cursor.w * 4);
  }
  if (fading) transition_restore();
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

void window_thumbnail(Window *w, Rect r) {
  if (game_thumbnail(w,r)) return;
  if (empty(r) || !w->surface || w->surface_width<=0 || w->surface_height<=0 || w->surface_rows<w->surface_height) return;
  int width=r.w,height=(int64_t)r.w*w->surface_height/w->surface_width;
  if (height>r.h) {height=r.h;width=(int64_t)height*w->surface_width/w->surface_height;}
  Rect dst={r.x+(r.w-width)/2,r.y+(r.h-height)/2,width,height};
  if (width<=0 || height<=0) return;
  Rect area=intersect(dst,desk.clip);
  for(int y=area.y;y<area.y+area.h;y++)for(int x=area.x;x<area.x+area.w;x++) {
    uint32_t p=w->surface[(size_t)((int64_t)(y-dst.y)*w->surface_height/height)*w->surface_stride+
      (int64_t)(x-dst.x)*w->surface_width/width];
    uint32_t *out=paint_pixel(x,y);*out=glass_over(p,*out);
  }
}
