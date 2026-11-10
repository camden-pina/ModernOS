#include "desktop.h"
#include "glass_filter.h"

/* Rendered RGB backdrops, never wallpaper-only imitations. Window caches hold
   only the chrome/sidebar bands. Foreground surfaces retain premultiplied
   alpha. All sample rectangles (including the full filter halo) are
   reconstructed from the wallpaper and lower windows before this layer is
   sampled. */
static GlassCache system_glass[5];
static bool system_active[5];
static uint32_t *scratch_a, *scratch_b;
static size_t scratch_bytes;
static uint64_t clock_us(void) {
  struct os_timespec t;
  return call2(SYS_clock_gettime, 1, &t)
             ? 0
             : (uint64_t)t.sec * 1000000 + t.nsec / 1000;
}
static int blur_radius(void) { return MAX(2,MIN(8,(S(12)+3)/4+(desk.glass_style==0?-1:desk.glass_style==2?2:0))); }
static Rect sample_rect(Rect r) {
  int pad = 2 * blur_radius() * 4 + 4;
  int x = (r.x - pad) & ~3, y = (r.y - pad) & ~3;
  int right = (r.x + r.w + pad + 3) & ~3, bottom = (r.y + r.h + pad + 3) & ~3;
  return (Rect){x, y, right - x, bottom - y};
}
void glass_release(GlassCache *c) {
  deallocate(c->pixels, c->bytes);
  memset(c, 0, sizeof(*c));
}
void glass_plan(GlassCache *c, Rect rect, uint64_t key) {
  if (empty(rect)) {
    if (c->pixels)
      glass_release(c);
    return;
  }
  if (!c->pixels || c->key != key || memcmp(&c->rect, &rect, sizeof(rect)))
    c->pending = true;
  c->rect = rect;
  c->key = key;
  c->sample = sample_rect(rect);
}
static uint32_t tint(uint32_t p, uint32_t color, unsigned alpha) {
  unsigned inverse = 255 - alpha;
  return ((((p & 0xff00ff) * inverse + (color & 0xff00ff) * alpha + 0x800080) >>
           8) &
          0xff00ff) |
         ((((p & 0x00ff00) * inverse + (color & 0x00ff00) * alpha + 0x008000) >>
           8) &
          0x00ff00);
}
void glass_build(GlassCache *c, int role) {
  if (!c->pending || empty(c->rect))
    return;
  uint64_t begin = clock_us();
  size_t bytes = (size_t)c->rect.w * c->rect.h * 4;
  if (c->bytes != bytes || !c->pixels) {
    deallocate(c->pixels, c->bytes);
    c->pixels = allocate(bytes);
    c->bytes = c->pixels ? bytes : 0;
  }
  uint32_t color = desk.dark_mode ? 0x27272C : 0xF9F9FC;
  unsigned opacity = desk.dark_mode ? 154 : 158;
  if (role == 1)
    opacity = desk.dark_mode ? 155 : 142;
  /* Shared hue; the thin menu bar keeps more of its wallpaper backdrop. */
  if (role == 2 || role == 3 || role == 7) {
    color = desk.dark_mode ? 0x24252F : 0xF7F8FF;
    opacity = desk.dark_mode ? 110 : 90;
  }
  if (role == 2)
    opacity = desk.dark_mode ? 64 : 48;
  if (role >= 4 && role != 7)
    opacity = desk.dark_mode ? 184 : 205;
  if (role == 6) {
    color = terminal_background();
    opacity = desk.terminal_opacity * 255 / 100;
  }
  if(role!=6) {
    if(desk.glass_style==0)opacity=MAX(32,(int)opacity-40);
    if(desk.glass_style==2)opacity=MIN(230,opacity+45);
    if(desk.glass_style==3)opacity=255;
  }
  if (!c->pixels) {
    desk.glass_fallbacks++;
    c->pending = false;
    return;
  }
  Rect s = c->sample;
  int width = s.w / 4, height = s.h / 4;
  size_t needed = (size_t)width * height * 4;
  bool solid=desk.glass_style==3 && role!=6;
  if (!desk.glass_disabled && !solid && needed > scratch_bytes) {
    deallocate(scratch_a, scratch_bytes);
    deallocate(scratch_b, scratch_bytes);
    scratch_a = allocate(needed);
    scratch_b = allocate(needed);
    scratch_bytes = needed;
  }
  bool filter = !desk.glass_disabled && !solid && scratch_a && scratch_b;
  if (!desk.glass_disabled && !solid && !filter)
    desk.glass_fallbacks++;
  if (filter) {
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++) {
        unsigned red = 0, green = 0, blue = 0;
        for (int dy = 0; dy < 4; dy++)
          for (int dx = 0; dx < 4; dx++) {
            int sx = s.x + x * 4 + dx, sy = s.y + y * 4 + dy;
            sx = MAX(0, MIN(sx, desk.width - 1));
            sy = MAX(0, MIN(sy, desk.height - 1));
            uint32_t p = desk.scene[(size_t)sy * desk.width + sx];
            red += (p >> 16) & 255;
            green += (p >> 8) & 255;
            blue += p & 255;
          }
        scratch_a[y * width + x] =
            ((red + 8) / 16 << 16) | ((green + 8) / 16 << 8) | (blue + 8) / 16;
      }
    int radius = blur_radius();
    for (int pass = 0; pass < 2; pass++) {
      glass_box_pass(scratch_a, scratch_b, width, height, radius, 0);
      glass_box_pass(scratch_b, scratch_a, width, height, radius, 1);
    }
    for (int i = 0; i < width * height; i++)
      scratch_a[i] = tint(scratch_a[i], color, opacity);
    for (int y = 0; y < c->rect.h; y++) {
      int fy = c->rect.y + y - s.y - 2, iy = fy >> 2;
      unsigned vy = fy & 3;
      for (int x = 0; x < c->rect.w; x++) {
        int fx = c->rect.x + x - s.x - 2, ix = fx >> 2;
        unsigned vx = fx & 3;
        uint32_t top = glass_lerp4(scratch_a[iy * width + ix],
                                   scratch_a[iy * width + ix + 1], vx);
        uint32_t bottom = glass_lerp4(scratch_a[(iy + 1) * width + ix],
                                      scratch_a[(iy + 1) * width + ix + 1], vx);
        c->pixels[(size_t)y * c->rect.w + x] = glass_lerp4(top, bottom, vy);
      }
    }
  } else
    for (size_t i = 0; i < bytes / 4; i++)
      c->pixels[i] = color;
  c->pending = false;
  desk.glass_updates++;
  desk.glass_pixels += (uint64_t)c->rect.w * c->rect.h;
  desk.glass_time_us += clock_us() - begin;
}
uint32_t glass_over(uint32_t foreground, uint32_t background) {
  unsigned alpha = foreground >> 24;
  if (alpha == 255)
    return foreground;
  if (!alpha)
    return background;
  return (foreground & 0xffffff) + coverage_over(0, background, alpha);
}

void glass_draw(GlassCache *c, Rect r, int radius) {
  Rect visible = intersect(r, desk.clip);
  for (int y = visible.y; y < visible.y + visible.h; y++) {
    int dy = MIN(y - r.y, r.y + r.h - 1 - y);
    if (c->pixels && (radius <= 0 || dy >= radius)) {
      memcpy(paint_pixel(visible.x, y),
             c->pixels + (size_t)(y - c->rect.y) * c->rect.w + visible.x -
                 c->rect.x,
             visible.w * 4);
      continue;
    }
    for (int x = visible.x; x < visible.x + visible.w; x++) {
      unsigned a = rect_coverage(r, radius, x, y);
      if (!a)
        continue;
      uint32_t color =
          c->pixels
              ? c->pixels[(size_t)(y - c->rect.y) * c->rect.w + x - c->rect.x]
              : THEME_TOOLBAR;
      uint32_t *target = paint_pixel(x, y);
      *target = coverage_over(color, *target, a);
    }
  }
}
Rect window_glass_rect(Window *w, int index) {
  if (w->fullscreen) return (Rect){0};
  if (!index)
    return (Rect){w->box.x, w->box.y, w->box.w, S(WINDOW_HEADER)};
  if (w->app == APP_TERMINAL && !terminal_is_preferences(w) && desk.terminal_opacity < 100)
    return window_body(w);
  if (w->app == APP_FILES)
    return files_sidebar_rect(w);
  if (w->app == APP_DISKS)
    return disks_sidebar_rect(w);
  return (Rect){0};
}
void window_material(Window *w, Rect rect, int index) {
  if (desk.paint_alpha) {
    Rect r = intersect(rect, desk.clip);
    for (int y = r.y; y < r.y + r.h; y++)
      memset(paint_pixel(r.x, y), 0, (size_t)r.w * 4);
  } else
    glass_draw(&w->glass[index], rect, 0);
}
static uint64_t hash_word(uint64_t hash, uint64_t value) {
  return (hash ^ value) * 1099511628211ULL;
}
uint64_t glass_layer_key(Rect rect, const int *order, int count) {
  Rect source = sample_rect(rect);
  uint64_t key = hash_word(1469598103934665603ULL, desk.material_epoch);
  key = hash_word(key, (unsigned)desk.width |
                           ((uint64_t)(unsigned)desk.height << 32));
  key = hash_word(key,desk.glass_style);
  key = hash_word(key, desk.scale | ((uint64_t)desk.dark_mode << 32) |
                           ((uint64_t)desk.glass_disabled << 40));
  for (int i = 0; i < count; i++) {
    Window *w = &desk.windows[order[i]];
    Rect b = w->box;
    Rect bounds = {b.x - S(34), b.y - S(14), b.w + S(68), b.h + S(62)};
    if (empty(intersect(source, bounds)))
      continue;
    key = hash_word(key, w->id);
    Rect content=intersect(source,b);
    if (!empty(content)) {
      int tile_w=(b.w+SURFACE_GRID-1)/SURFACE_GRID;
      int tile_h=(b.h+SURFACE_GRID-1)/SURFACE_GRID;
      for (int y=(content.y-b.y)/tile_h; y<=(content.y+content.h-1-b.y)/tile_h; y++)
        for (int x=(content.x-b.x)/tile_w; x<=(content.x+content.w-1-b.x)/tile_w; x++)
          key=hash_word(key,w->surface_tiles[y*SURFACE_GRID+x]);
    }
    key = hash_word(key, (unsigned)b.x | ((uint64_t)(unsigned)b.y << 32));
    key = hash_word(key, (unsigned)b.w | ((uint64_t)(unsigned)b.h << 32));
    key = hash_word(key, w->glass[0].key);
    key = hash_word(key, w->glass[1].key);
  }
  return key;
}
void glass_system_plan(const int *order, int count) {
  Rect bounds[5] = {{0, 0, desk.width, S(DESKTOP_TOP)},
                    taskbar_dock_cache_rect(),
                    desk.launcher_box,
                    desk.menu.box,
                    desk.dialog.box};
  system_active[0] = system_active[1] = true;
  system_active[2] = desk.launcher;
  system_active[3] = desk.menu.open;
  system_active[4] = desk.dialog.open;
  Rect hover = taskbar_hover(desk.mouse_x, desk.mouse_y);
  for (int i = 0; i < 5; i++)
    if (system_active[i]) {
      uint64_t key = glass_layer_key(bounds[i], order, count);
      if (i >= 2) {
        Rect dock = taskbar_dock_bounds();
        if (!empty(intersect(sample_rect(bounds[i]), dock)))
          key = hash_word(key, desk.dock_revision);
        key = hash_word(key, (unsigned)hover.x |
                                 ((uint64_t)(unsigned)hover.y << 32));
        key = hash_word(key, desk.menu.origin_group);
        for (const char *p = desk.clock; *p; p++)
          key = hash_word(key, (unsigned char)*p);
      }
      glass_plan(&system_glass[i], bounds[i],
                 key + (uint64_t)i * 0x9e3779b97f4a7c15ULL);
    }
}
void glass_system_damage(void) {
  for (int i = 0; i < 5; i++)
    if (system_active[i] && system_glass[i].pending)
      compositor_damage(desk.glass_disabled ? system_glass[i].rect
                                            : system_glass[i].sample);
}
void glass_system_prepare(int index) {
  glass_build(&system_glass[index], index == 0 ? 2 : index == 1 ? 3 : index == 2 ? 7 : 4);
}
void glass_system_draw(int index, Rect rect, int radius) {
  glass_draw(&system_glass[index], rect, radius);
}
void glass_reset(void) { desk.material_epoch++; }
void glass_cleanup(void) {
  for (int i = 0; i < 5; i++)
    glass_release(&system_glass[i]);
  deallocate(scratch_a, scratch_bytes);
  deallocate(scratch_b, scratch_bytes);
  scratch_a = scratch_b = NULL;
  scratch_bytes = 0;
}
