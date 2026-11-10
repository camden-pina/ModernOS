#include "desktop.h"
static uint32_t tile_mix(uint32_t a, uint32_t b, unsigned t) {
  return ((((a & 0xff00ff) * (255 - t) + (b & 0xff00ff) * t + 0x800080) >> 8) &
          0xff00ff) |
         ((((a & 0x00ff00) * (255 - t) + (b & 0x00ff00) * t + 0x008000) >> 8) &
          0x00ff00);
}
static void dock_art_uncached(Rect r, int item) {
  if (empty(intersect(r, desk.clip)))
    return;
  static const uint32_t upper[] = {0xF9FBFF, 0x79C8FF, 0xFFFFFF, 0x465568,
                                   0xF1F3F6, 0xE4E9F0, 0xFFFFFF, 0x7399D3,
                                   0xB5C8D8, 0xDA8C73, 0x648EC7};
  static const uint32_t lower[] = {0xE3E7F1, 0x238EEB, 0xDFE9F8, 0x182335,
                                   0x9FAAB9, 0xA2AFBE, 0xE8ECF3, 0x385D95,
                                   0x617F99, 0x8F4350, 0x365C99};
  int radius = MAX(2, r.w / 4);
  Rect visible = intersect(r, desk.clip);
  for (int y = visible.y; y < visible.y + visible.h; y++) {
    uint32_t color =
        tile_mix(upper[item], lower[item], (y - r.y) * 255 / MAX(1, r.h - 1));
    for (int x = visible.x; x < visible.x + visible.w; x++)
      blend((Rect){x, y, 1, 1}, color, rect_coverage(r, radius, x, y));
  }
  border(r, radius, tile_mix(upper[item], 0xFFFFFF, 100));
  int glyph = MIN(ICON_MAX, r.w * 2 / 3), x = r.x + (r.w - glyph) / 2,
      y = r.y + (r.h - glyph) / 2;
  if (item == 0) {
    const uint32_t color[] = {0xEF667D, 0xF4BC4F, 0x6CB996, 0x669CE2, 0xAB82C7,
                              0x59BBD2, 0xEC9066, 0x8AA2C5, 0x88C677};
    int step = r.w / 5, size = MAX(2, step - 2),
        start = r.x + (r.w - (step * 2 + size)) / 2,
        top = r.y + (r.h - (step * 2 + size)) / 2;
    for (int i = 0; i < 9; i++)
      rounded((Rect){start + (i % 3) * step, top + (i / 3) * step, size, size},
              MAX(1, size / 4), color[i]);
  } else if (item == 1) {
    rounded((Rect){r.x + r.w / 7, r.y + r.h / 4, r.w * 2 / 5, r.h / 4},
            MAX(1, r.w / 15), 0xBCE7FF);
    rounded((Rect){r.x + r.w / 7, r.y + r.h / 3, r.w * 5 / 7, r.h / 2},
            MAX(1, r.w / 12), 0xD0EEFF);
    rounded((Rect){r.x + r.w / 7, r.y + r.h * 2 / 5, r.w * 5 / 7, r.h * 2 / 5},
            MAX(1, r.w / 12), 0x53B3F4);
    fill((Rect){r.x + r.w / 5, r.y + r.h * 2 / 5, r.w * 3 / 5, 1}, 0x97D5FF);
  } else if (item == 2)
    icon(x, y, I_GLOBE, glyph, 0x2F8DDD);
  else if (item == 3)
    icon(x, y, I_TERMINAL, glyph, 0xF5FAFF);
  else if (item == 4) {
    icon(x, y, I_DRIVE, glyph, 0x5A6674);
    rounded((Rect){r.x + r.w * 3 / 4, r.y + r.h * 3 / 4, MAX(2, r.w / 12),
                   MAX(2, r.h / 12)},
            1, 0x4EABDA);
  } else if (item == 5) {
    icon(x + 1, y + 1, I_GEAR, glyph, 0xFFFFFF);
    icon(x, y, I_GEAR, glyph, 0x637184);
  } else if (item == 6) {
    for (int i = 0; i < 4; i++)
      fill((Rect){r.x + r.w / 5, r.y + r.h / 4 + i * r.h / 7, r.w * 3 / 5, 1},
           0xBCCADA);
    line(r.x + r.w / 3, r.y + r.h * 3 / 4, r.x + r.w * 3 / 4, r.y + r.h / 4,
         0xE1AA3A);
    line(r.x + r.w / 3 + 1, r.y + r.h * 3 / 4, r.x + r.w * 3 / 4 + 1,
         r.y + r.h / 4, 0xF1C665);
  } else
    icon(x, y,
         item == 10  ? I_CODE
         : item == 8   ? I_INFO
         : item == 9 ? I_GRID
                     : I_WORKSPACE,
         glyph, 0xF1F7FF);
}

/* Stable roles keep animation and sprite identity independent of pin order. */
static const App app_order[] = {APP_FILES, APP_CHROMIUM, APP_TERMINAL,
                                APP_DISKS, APP_SETTINGS, APP_EDITOR,
                                APP_POWER, APP_GAME, APP_WORKBENCH, APP_CODE, APP_QT};
static int magnify[APP_COUNT + 2];
static uint64_t tick_at, trace_at, animation_frames, sprite_builds;
static bool animating;
typedef struct {
  uint32_t *pixels;
  size_t bytes;
  int art, size, scale;
} DockSprite;
static DockSprite sprites[128];
static size_t sprite_bytes, sprite_peak;
static unsigned replacement, prior_visible = ~0u, prior_pins = ~0u;
static unsigned running_mask(void) {
  unsigned mask = 0;
  for (int i = 0; i < MAX_WINDOWS; i++)
    if (desk.windows[i].alive)
      mask |= 1u << desk.windows[i].app;
  return mask;
}
static int roles(int *out) {
  unsigned running = running_mask();
  int count = 0;
  out[count++] = APP_COUNT;
  for (int pass = 0; pass < 2; pass++)
    for (unsigned i = 0; i < sizeof(app_order) / sizeof(app_order[0]); i++) {
      int app = app_order[i];
      if (!app_available((App)app))
        continue;
      bool pinned = (desk.dock_pins & (1u << app)) != 0;
      if ((pass == 0 && pinned) ||
          (pass == 1 && !pinned && (running & (1u << app))))
        out[count++] = app;
    }
  out[count++] = APP_COUNT + 1;
  return count;
}
static int dock_step(void) {
  int list[APP_COUNT + 2], n = roles(list);
  return MAX(S(20), MIN(S(desk.dock_size + 10), (desk.width - S(28)) / n));
}
static int base_size(void) {
  return MAX(S(12), MIN(S(desk.dock_size), dock_step() - S(8)));
}
static void layout(Rect *body, Rect *cells, Rect *tiles) {
  int list[APP_COUNT + 2], n = roles(list), sizes[APP_COUNT + 2];
  int total = 0, gap = S(10), available = MAX(S(20), desk.width - S(28));
  for (int i = 0; i < n; i++) {
    sizes[i] = (base_size() * MAX(1024, magnify[list[i]]) + 512) / 1024;
    total += sizes[i];
  }
  int ratio = MIN(1024, MAX(n, available - n * gap) * 1024 / MAX(1, total));
  int width = 0;
  for (int i = 0; i < n; i++) {
    sizes[i] = MAX(1, sizes[i] * ratio / 1024);
    width += sizes[i] + gap;
  }
  int height = base_size() + S(14), x = (desk.width - width) / 2;
  *body = (Rect){x - S(6), desk.height - height - S(8), width + S(12), height};
  for (int i = 0; i < n; i++) {
    cells[i] = (Rect){x, body->y, sizes[i] + gap, height};
    tiles[i] = (Rect){x + gap / 2, body->y + height - S(9) - sizes[i], sizes[i],
                      sizes[i]};
    x += sizes[i] + gap;
  }
}
Rect taskbar_dock_rect(void) {
  Rect b, c[APP_COUNT + 2], t[APP_COUNT + 2];
  layout(&b, c, t);
  return b;
}
int desktop_bottom(void) {
  return MAX(S(DESKTOP_BOTTOM), taskbar_dock_rect().h + S(8));
}
static Rect cell(int at) {
  Rect b, c[APP_COUNT + 2], t[APP_COUNT + 2];
  layout(&b, c, t);
  return c[at];
}
static Rect tile(int at, int role) {
  (void)role;
  Rect b, c[APP_COUNT + 2], t[APP_COUNT + 2];
  layout(&b, c, t);
  return t[at];
}
static Rect dock_bounds(void) {
  int list[APP_COUNT + 2], n = roles(list);
  Rect b = taskbar_dock_rect();
  int top = b.y;
  for (int i = 0; i < n; i++)
    top = MIN(top, tile(i, list[i]).y);
  return (Rect){b.x - S(5), top - S(4), b.w + S(10), desk.height - top + S(4)};
}
Rect taskbar_dock_bounds(void) { return dock_bounds(); }
/* Animate the visible backplate inside a stable backdrop cache. Sampling the
   same screen region avoids rebuilding a large HiDPI blur on each icon step.
   Window/wallpaper revisions still invalidate the complete sampled region. */
Rect taskbar_dock_cache_rect(void) {
  int list[APP_COUNT + 2], n = roles(list), base = base_size();
  int width =
      MIN(desk.width, n * (base + S(10)) + S(12) +
                          (base * desk.dock_magnification * 4 + 99) / 100);
  Rect b = taskbar_dock_rect();
  return (Rect){(desk.width - width) / 2, b.y, width, b.h};
}
static Rect previous_bounds;
static int dock_hit(int x, int y) {
  int list[APP_COUNT + 2], n = roles(list), best = -1, distance = 0x7fffffff;
  for (int i = 0; i < n; i++) {
    Rect r = tile(i, list[i]), c = cell(i);
    r = (Rect){r.x - S(3), r.y - S(3), r.w + S(6), r.h + S(6)};
    if (!contains(r, x, y) && !contains(c, x, y))
      continue;
    int dx = x - (c.x + c.w / 2);
    dx = dx < 0 ? -dx : dx;
    if (dx < distance) {
      best = i;
      distance = dx;
    }
  }
  return best;
}
Rect dock_hover(int x, int y) {
  int at = dock_hit(x, y);
  return at < 0 ? (Rect){0} : cell(at);
}
static int artwork(int role) {
  if (role == APP_COUNT)
    return 0;
  if (role == APP_COUNT + 1)
    return 7;
  return role == APP_FILES      ? 1
         : role == APP_CHROMIUM ? 2
         : role == APP_TERMINAL ? 3
         : role == APP_DISKS    ? 4
         : role == APP_SETTINGS ? 5
         : role == APP_EDITOR   ? 6
         : role == APP_CODE || role == APP_WORKBENCH ? 10
         : role == APP_POWER    ? 8
                                : 9;
}
static void sprite_draw(Rect r, int role) {
  int art = artwork(role);
  DockSprite *s = NULL;
  for (unsigned i = 0; i < 128; i++)
    if (sprites[i].pixels && sprites[i].art == art && sprites[i].size == r.w &&
        sprites[i].scale == desk.scale) {
      s = &sprites[i];
      break;
    }
  if (!s) {
    s = &sprites[replacement++ % 128];
    sprite_bytes -= s->bytes;
    deallocate(s->pixels, s->bytes);
    size_t needed = (size_t)r.w * r.h * 4;
    for (unsigned i = 0; i < 128 && sprite_bytes + needed > 16 * 1024 * 1024;
         i++)
      if (&sprites[i] != s && sprites[i].pixels) {
        sprite_bytes -= sprites[i].bytes;
        deallocate(sprites[i].pixels, sprites[i].bytes);
        memset(&sprites[i], 0, sizeof(sprites[i]));
      }
    s->bytes = (size_t)r.w * r.h * 4;
    s->pixels = allocate(s->bytes);
    s->art = art;
    s->size = r.w;
    s->scale = desk.scale;
    if (!s->pixels) {
      s->bytes = 0;
      dock_art_uncached(r, art);
      return;
    }
    sprite_bytes += s->bytes;
    sprite_peak = MAX(sprite_peak, sprite_bytes);
    memset(s->pixels, 0, s->bytes);
    uint32_t *paint = desk.paint;
    int stride = desk.paint_stride, x = desk.paint_x, y = desk.paint_y;
    Rect clip = desk.clip;
    bool alpha = desk.paint_alpha;
    desk.paint = s->pixels;
    desk.paint_stride = r.w;
    desk.paint_x = desk.paint_y = 0;
    desk.paint_alpha = true;
    desk.clip = (Rect){0, 0, r.w, r.h};
    dock_art_uncached((Rect){0, 0, r.w, r.h}, art);
    desk.paint = paint;
    desk.paint_stride = stride;
    desk.paint_x = x;
    desk.paint_y = y;
    desk.paint_alpha = alpha;
    desk.clip = clip;
    sprite_builds++;
  }
  Rect v = intersect(r, desk.clip);
  for (int y = v.y; y < v.y + v.h; y++)
    for (int x = v.x; x < v.x + v.w; x++) {
      uint32_t fg = s->pixels[(size_t)(y - r.y) * r.w + x - r.x];
      unsigned a = fg >> 24;
      if (a) {
        uint32_t *target = paint_pixel(x, y);
        *target = a == 255 ? fg : glass_over(fg, *target);
      }
    }
}
void dock_draw(void) {
  Rect b = taskbar_dock_rect();
  int list[APP_COUNT + 2], n = roles(list);
  unsigned running = running_mask();
  glass_system_prepare(1);
  blend((Rect){b.x - S(1), b.y + S(4), b.w + S(2), b.h}, 0x091222, 35);
  glass_system_draw(1, b, S(10));
  border(b, S(10), desk.dark_mode ? 0x667078 : 0xCAD3DF);
  for (int i = 0; i < n; i++) {
    Rect c = cell(i);
    sprite_draw(tile(i, list[i]), list[i]);
    if (list[i] < APP_COUNT && (running & (1u << list[i])))
      rounded((Rect){c.x + c.w / 2 - S(2), b.y + b.h - S(5), S(4), S(3)}, S(2),
              desk.dark_mode ? 0xE9ECF4 : 0x26324A);
  }
}
void taskbar_tick(void) {
  unsigned visible = desk.dock_pins | running_mask();
  if (visible != prior_visible || desk.dock_pins != prior_pins)
    taskbar_preferences_changed();
  uint64_t now = now_ms();
  int dt = tick_at ? MIN(1000, now - tick_at) : 8;
  tick_at = now;
  if (dt < 1)
    return;
  Rect b = taskbar_dock_rect(), old_bounds = dock_bounds();
  int list[APP_COUNT + 2], n = roles(list), step = dock_step();
  bool changed = false, active = false;
  bool hover = !desk.dialog.open && !desk.menu.open && !desk.launcher &&
               contains((Rect){0, b.y - base_size(), desk.width,
                               b.h + base_size() + S(22)},
                        desk.mouse_x, desk.mouse_y);
  for (int i = 0; i < n; i++) {
    int role = list[i], old = MAX(1024, magnify[role]),
        distance =
            desk.mouse_x - ((desk.width - step * n) / 2 + i * step + step / 2);
    distance = distance < 0 ? -distance : distance;
    int influence =
        hover ? MAX(0, 1024 - distance * 1024 / MAX(1, step * 3 / 2)) : 0;
    int target = 1024 + (desk.dock_magnification * 1024 / 100) * influence /
                            1024 * influence / 1024;
    int delta = target - old, next = old + (int64_t)delta * dt / (75 + dt);
    if (delta && next == old)
      next += delta > 0 ? 1 : -1;
    if ((base_size() * old + 512) / 1024 != (base_size() * next + 512) / 1024)
      changed = true;
    magnify[role] = next;
    active |= next != target;
  }
  if (animating && !active && desk.trace)
    printf("DESKTOP DOCK_SETTLED hover=%u time=%lu\n", hover, now);
  animating = active;
  if (changed) {
    desk.dock_revision++;
    animation_frames++;
    compositor_damage(old_bounds);
    compositor_damage(dock_bounds());
    previous_bounds = dock_bounds();
    if (desk.trace && now - trace_at >= 45) {
      int at = dock_hit(desk.mouse_x, desk.mouse_y);
      if (at >= 0)
        printf("DESKTOP DOCK_ANIMATE role=%d size=%d base=%d time=%lu\n",
               list[at], tile(at, list[at]).w, base_size(), now);
      trace_at = now;
    }
  }
}
void taskbar_preferences_changed(void) {
  prior_visible = desk.dock_pins | running_mask();
  prior_pins = desk.dock_pins;
  for (int i = 0; i < APP_COUNT + 2; i++)
    magnify[i] = 1024;
  desk.dock_revision++;
  invalidate(previous_bounds);
  previous_bounds = dock_bounds();
  invalidate(previous_bounds);
  wm_workarea_changed();
  if (desk.trace) {
    Rect b = taskbar_dock_rect();
    int list[APP_COUNT + 2];
    printf("DESKTOP DOCK_LAYOUT x=%d y=%d width=%d height=%d count=%d size=%d "
           "magnification=%d pins=%u bottom=%d\n",
           b.x, b.y, b.w, b.h, roles(list), desk.dock_size,
           desk.dock_magnification, desk.dock_pins, desktop_bottom());
    int n = roles(list);
    for (int i = 0; i < n; i++) {
      Rect c = cell(i), t = tile(i, list[i]);
      printf("DESKTOP DOCK_ITEM role=%d x=%d y=%d width=%d height=%d tile_x=%d "
             "tile_y=%d tile_size=%d pinned=%d running=%d\n",
             list[i], c.x, c.y, c.w, c.h, t.x, t.y, t.w,
             list[i] < APP_COUNT && (desk.dock_pins & (1u << list[i])) != 0,
             list[i] < APP_COUNT && (running_mask() & (1u << list[i])) != 0);
    }
  }
}
static char window_labels[MAX_WINDOWS][112];
static void dock_command(Window *unused, int action) {
  (void)unused;
  if (action == 400) {
    settings_open_dock();
    return;
  }
  if (action >= 200 && action < 200 + APP_COUNT) {
    int app = action - 200;
    desk.dock_pins ^= 1u << app;
    taskbar_preferences_changed();
    preferences_save(NULL);
    trace((desk.dock_pins & (1u << app)) ? "DOCK_PIN" : "DOCK_UNPIN", NULL,
          apps[app].name);
    return;
  }
  if (action >= 100 && action < 100 + APP_COUNT) {
    wm_open(action - 100, NULL);
    return;
  }
  if (action >= 0 && action < MAX_WINDOWS)
    wm_focus(&desk.windows[action]);
}
static void app_activate(int app, bool context, int x, int y) {
  Window *recent = NULL;
  MenuItem items[MAX_WINDOWS + 3];
  int n = 0;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *w = &desk.windows[i];
    if (!w->alive || w->app != (App)app)
      continue;
    if (!recent ||
        (w->workspace == desk.workspace &&
         recent->workspace != desk.workspace) ||
        (w->workspace == recent->workspace && w->z > recent->z))
      recent = w;
    copy_text(window_labels[n], sizeof(window_labels[n]), w->title);
    items[n] = (MenuItem){window_labels[n], apps[app].icon, i, true};
    n++;
  }
  if (context) {
    items[n++] = (MenuItem){app == APP_WORKBENCH ? "Open Code OSS" : "New window",
                            apps[app].icon, 100 + app, true};
    items[n++] = (MenuItem){(desk.dock_pins & (1u << app)) ? "Remove from Dock"
                                                           : "Keep in Dock",
                            I_CHECK, 200 + app, true};
    items[n++] = (MenuItem){"Dock preferences...", I_GEAR, 400, true};
    menu_open(NULL, x, y - n * S(24) - S(12), items, n, dock_command);
    trace("DOCK_MENU", NULL, apps[app].name);
  } else if (recent)
    wm_focus(recent);
  else
    wm_open(app, NULL);
}
bool dock_mouse(int x, int y, unsigned pressed, int wheel) {
  (void)wheel;
  int list[APP_COUNT + 2], n = roles(list), at = dock_hit(x, y);
  Rect b = taskbar_dock_rect();
  if (at < 0 && !contains(b, x, y))
    return false;
  if (!pressed)
    return true;
  if (at < 0 || list[at] >= APP_COUNT) {
    if (pressed & 2) {
      MenuItem m[] = {{"Dock preferences...", I_GEAR, 400, true}};
      menu_open(NULL, x, b.y - S(36), m, 1, dock_command);
    } else if (at >= 0 && list[at] == APP_COUNT)
      launcher_toggle();
    else if (at == n - 1)
      desktop_windows_menu(x, b.y - S(4));
  } else
    app_activate(list[at], (pressed & 2) != 0, x, b.y);
  return true;
}
void taskbar_cleanup(void) {
  sprite_bytes = 0;
  for (unsigned i = 0; i < 128; i++) {
    deallocate(sprites[i].pixels, sprites[i].bytes);
    memset(&sprites[i], 0, sizeof(sprites[i]));
  }
  if (desk.trace)
    printf("DOCK_STATS animation_frames=%lu sprite_builds=%lu "
           "cache_peak_bytes=%lu limit_bytes=%u\n",
           animation_frames, sprite_builds, sprite_peak, 16 * 1024 * 1024);
}
