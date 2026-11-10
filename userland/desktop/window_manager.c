#include "desktop.h"

Rect window_body(Window *w) {
  if (w->fullscreen) return w->box;
  return (Rect){w->box.x + 1, w->box.y + S(WINDOW_HEADER), w->box.w - 2,
                w->box.h - S(WINDOW_HEADER) - 1};
}

bool wm_fullscreen_active(void) {
  Window *w = wm_focused();
  return w && w->fullscreen;
}

void wm_trace_geometry(Window *w) {
  if (desk.trace)
    printf("DESKTOP GEOMETRY window=%d workspace=%d x=%d y=%d width=%d "
           "height=%d body=%d "
           "scale=%d\n",
           w->id, w->workspace + 1, w->box.x, w->box.y, w->box.w, w->box.h,
           S(WINDOW_HEADER), desk.scale);
}

Window *wm_focused(void) {
  if (desk.focused < 0 || desk.focused >= MAX_WINDOWS) {
    return NULL;
  }
  Window *w = &desk.windows[desk.focused];
  return w->alive && !w->minimized && w->workspace == desk.workspace ? w : NULL;
}

/* A cached underlay must not paint a hover belonging to a higher window or
   popup. Its eligibility is tracked with the cached foreground in render.c. */
bool wm_pointer_active(Window *w) {
  int x = desk.mouse_x, y = desk.mouse_y;
  if (!w->alive || w->minimized || w->workspace != desk.workspace ||
      !contains(w->box, x, y) || desk.dialog.open || desk.snap_assist ||
      (desk.menu.open && contains(desk.menu.box, x, y)) ||
      (desk.snap_open && contains(desk.snap_box, x, y)) ||
      (desk.launcher && contains(desk.launcher_box, x, y)))
    return false;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *above = &desk.windows[i];
    if (above->alive && !above->minimized &&
        above->workspace == desk.workspace && above->z > w->z &&
        contains(above->box, x, y))
      return false;
  }
  return true;
}

static void taskbar_dirty(void) {
  invalidate((Rect){0, 0, desk.width, S(DESKTOP_TOP)});
  invalidate(
      (Rect){0, desk.height - desktop_bottom(), desk.width, desktop_bottom()});
}

Rect window_control(Window *w, int index) {
  return (Rect){w->box.x + S(10 + index * 20),
                w->box.y + (S(WINDOW_HEADER) - S(22)) / 2, S(20), S(22)};
}

static void clamp_window(Window *w) {
  int work = desk.height - (desktop_bottom() + S(DESKTOP_TOP)),
      minw = MIN(S(480), desk.width), minh = MIN(S(340), work);
  w->box.w = clampi(w->box.w, minw, desk.width);
  w->box.h = clampi(w->box.h, minh, work);
  /* Keep a reachable strip of title bar on either side, with the menu bar
     and Dock reserved even when the body is partly off screen. */
  int visible = MIN(S(96), w->box.w);
  w->box.x = clampi(w->box.x, visible - w->box.w, desk.width - visible);
  w->box.y = clampi(w->box.y, S(DESKTOP_TOP), S(DESKTOP_TOP) + work - w->box.h);
}

void wm_workarea_changed(void) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *w = &desk.windows[i];
    if (!w->alive)
      continue;
    damage_window(w);
    if (w->fullscreen)
      w->box = (Rect){0, 0, desk.width, desk.height};
    else if (w->snapped)
      w->box = snap_geometry(w->snap_layout, w->snap_slot);
    else if (w->maximized)
      w->box = (Rect){0, S(DESKTOP_TOP), desk.width,
                      desk.height - S(DESKTOP_TOP) - desktop_bottom()};
    else
      clamp_window(w);
    w->restore.h =
        MIN(w->restore.h, desk.height - S(DESKTOP_TOP) - desktop_bottom());
    w->restore.y = clampi(w->restore.y, S(DESKTOP_TOP),
                          desk.height - desktop_bottom() - w->restore.h);
    invalidate_window(w);
    wm_trace_geometry(w);
  }
}

void wm_focus(Window *w) {
  if (!w || !w->alive) {
    return;
  }
  Window *old = wm_focused();
  if (old == w) {
    return;
  }
  if (old) {
    invalidate_window(old);
  }
  if (w->workspace != desk.workspace) {
    workspace_switch(w->workspace);
  }
  if (w->owner && w->owner->alive && w->owner->id == w->owner_id) {
    w->owner->minimized = false;
    w->owner->z = ++desk.z;
    invalidate_window(w->owner);
  }
  w->minimized = false;
  w->z = ++desk.z;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *child = &desk.windows[i];
    if (child->alive && child->owner == w && child->owner_id == w->id) {
      child->minimized = false;
      child->z = ++desk.z;
      invalidate_window(child);
    }
  }
  desk.focused = w - desk.windows;
  invalidate_window(w);
  taskbar_dirty();
  game_focus_changed();
  trace("FOCUS", w, apps[w->app].name);
}

static void focus_top(void) {
  Window *top = NULL;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *w = &desk.windows[i];
    if (w->alive && !w->minimized && w->workspace == desk.workspace &&
        (!top || w->z > top->z)) {
      top = w;
    }
  }

  int previous = desk.focused;
  desk.focused = top ? top - desk.windows : -1;
  if (top && previous != desk.focused) {
    invalidate_window(top);
  }
}

Window *wm_open(App app, const char *path) {
  if (app < 0 || app >= APP_COUNT || !apps[app].name)
    return NULL;
  /* The current Wayland adapter supports one workbench. Reopening its desktop
     entry also restores a minimized or still-starting window. */
  if ((app == APP_WORKBENCH || app == APP_CHROMIUM) && !path) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
      Window *existing = &desk.windows[i];
      if (existing->alive && existing->app == app) {
        wm_focus(existing);
        trace("ACTIVATE", existing, apps[app].name);
        return existing;
      }
    }
  }
  Window *w = NULL;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (!desk.windows[i].alive) {
      w = &desk.windows[i];
      break;
    }
  }

  if (!w) {
    dialog_notice(NULL, "Window limit",
                  "Close a window before opening another. The desktop supports "
                  "16 simultaneous windows.");
    return NULL;
  }

  memset(w, 0, sizeof(*w));
  w->app = app;
  w->workspace = desk.workspace;
  w->id = ++desk.next_id;
  w->alive = true;
  copy_text(w->title, sizeof(w->title), apps[app].name);
  int width = MIN(desk.width - S(120), S(800)),
      height = MIN(desk.height - S(160), S(500));
  w->box = (Rect){(desk.width - width) / 2, S(DESKTOP_TOP + 40), width, height};
  if (app != APP_FILES) {
    w->box.w = MIN(S(app == APP_SETTINGS ? 760 : 720), desk.width - S(50));
    w->box.h = MIN(S(app == APP_SETTINGS ? 560 : 460), desk.height - S(92));
    w->box.x = (desk.width - w->box.w) / 2;
    w->box.y = (desk.height - desktop_bottom() - w->box.h) / 2;
  }

  int offset = (w->id - 1) % 5 * S(16);
  if (w->id > 1) {
    w->box.x += offset;
    w->box.y += offset;
  }
  clamp_window(w);
  w->restore = w->box;
  int e = apps[app].open ? apps[app].open(w, path) : 0;
  if (e) {
    w->alive = false;
    dialog_error(NULL, "Open application", e);
    return NULL;
  }
  wm_focus(w);
  trace("OPEN", w, path ? path : apps[app].name);
  wm_trace_geometry(w);
  return w;
}

void wm_minimize(Window *w) {
  if (!w) {
    return;
  }
  invalidate_window(w);
  w->minimized = true;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *child = &desk.windows[i];
    if (child->alive && child->owner == w && child->owner_id == w->id) {
      invalidate_window(child);
      child->minimized = true;
    }
  }
  focus_top();
  taskbar_dirty();
  trace("MINIMIZE", w, "");
}

void wm_fullscreen(Window *w, bool enabled) {
  if (!w || !w->alive || w->fullscreen == enabled) return;
  if (enabled) {
    w->fullscreen_restore = w->box;
    w->fullscreen_was_maximized = w->maximized;
    w->fullscreen = true;
    w->box = (Rect){0, 0, desk.width, desk.height};
    wm_focus(w);
  } else {
    w->fullscreen = false;
    w->maximized = w->fullscreen_was_maximized;
    w->box = w->fullscreen_restore;
    if (w->snapped) w->box = snap_geometry(w->snap_layout, w->snap_slot);
    else clamp_window(w);
  }
  invalidate_all();
  trace(enabled ? "FULLSCREEN" : "UNFULLSCREEN", w, "");
  wm_trace_geometry(w);
}

void wm_maximize(Window *w) {
  if (!w) {
    return;
  }
  if (w->fullscreen) wm_fullscreen(w, false);
  transition_begin(snap_geometry(-1,0));
  invalidate_window(w);
  if (w->maximized) {
    w->box = w->restore;
    w->maximized = false;
    clamp_window(w);
  } else {
    if (!w->snapped) w->restore = w->box;
    w->snapped = false;
    w->box = (Rect){0, S(DESKTOP_TOP), desk.width,
                    desk.height - (desktop_bottom() + S(DESKTOP_TOP))};
    w->maximized = true;
  }
  invalidate_window(w);
  trace(w->maximized ? "MAXIMIZE" : "RESTORE", w, "");
  wm_trace_geometry(w);
}

void wm_close(Window *w) {
  if (!w || !w->alive) {
    return;
  }
  if (desk.snap_owner == w) snap_cancel();
  power_review_cancel();
  if (apps[w->app].can_close && !apps[w->app].can_close(w)) {
    return;
  }
  if (apps[w->app].close && !apps[w->app].close(w)) {
    return;
  }
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *child = &desk.windows[i];
    if (child->alive && child->owner == w && child->owner_id == w->id)
      wm_close(child);
  }
  Window *owner = w->owner;
  int owner_id = w->owner_id;
  invalidate_window(w);
  wm_free_surface(w);
  w->alive = false;
  if (desk.drag == w) {
    desk.drag = NULL;
  }
  if (desk.menu.owner == w) {
    menu_close();
  }
  if (desk.dialog.open && desk.dialog.owner == w) {
    Rect d = desk.dialog.box;
    invalidate((Rect){d.x - S(8), d.y - S(8), d.w + S(16), d.h + S(16)});
    desk.dialog.open = false;
  }
  focus_top();
  taskbar_dirty();
  trace("CLOSE", w, "");
  if (owner && owner->alive && owner->id == owner_id)
    wm_focus(owner);
}

void wm_snap(Window *w, bool right) {
  snap_apply(w, 0, right ? 1 : 0);
}

void workspace_switch(int ws) {
  if (ws < 0 || ws >= desk.workspace_count || ws == desk.workspace) {
    return;
  }
  snap_cancel();
  menu_close();
  desk.dialog.open = false;
  desk.quit_pending = false;
  desk.launcher = false;
  desk.drag = NULL;
  desk.workspace = ws;
  focus_top();
  invalidate_all();
  trace("WORKSPACE", NULL, "");
}

void window_move_workspace(Window *w, int ws) {
  if (!w || ws < 0 || ws >= desk.workspace_count) {
    return;
  }
  invalidate_window(w);
  w->workspace = ws;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *child = &desk.windows[i];
    if (child->alive && child->owner == w && child->owner_id == w->id) {
      invalidate_window(child);
      child->workspace = ws;
    }
  }
  focus_top();
  taskbar_dirty();
  trace("MOVE_WORKSPACE", w, "");
}

void wm_paint(Window *w) {
  int pointer_x = desk.mouse_x, pointer_y = desk.mouse_y;
  if (!wm_pointer_active(w))
    desk.mouse_x = desk.mouse_y = -100000;
  Rect saved = desk.clip, b = w->box;
  if (w->fullscreen) {
    clip_set(intersect(saved, b));
    fill(b, THEME_WINDOW);
    if (apps[w->app].draw) apps[w->app].draw(w);
    desk.mouse_x = pointer_x;
    desk.mouse_y = pointer_y;
    desk.clip = saved;
    return;
  }
  int radius = w->maximized ? 0 : S(WINDOW_CORNER);
  clip_set(intersect(saved, b));
  fill(b, THEME_WINDOW);
  Rect toolbar = {b.x, b.y, b.w, S(WINDOW_HEADER)};
  window_material(w, toolbar, 0);
  blend((Rect){b.x, b.y + S(WINDOW_HEADER) - 1, b.w, 1},
        desk.dark_mode ? 0x000000 : 0xFFFFFF, 46);
  const AppOps *app = &apps[w->app];
  if (app->toolbar)
    app->toolbar(w);
  else {
    int title_width = MIN(text_width(w->title, FONT_BOLD), b.w - S(180));
    label(b.x + MAX(S(82), (b.w - title_width) / 2), b.y + S(6), w->title,
          FONT_BOLD, THEME_PRIMARY_TEXT, MAX(0, b.w - S(170)));
  }
  static const uint32_t lights[] = {0xFF6259, 0xFFBD2E, 0x28C840};
  for (int i = 0; i < 3; i++) {
    Rect hit = window_control(w, i);
    Rect dot = {hit.x + S(3), b.y + (S(WINDOW_HEADER) - S(11)) / 2, S(11), S(11)};
    bool hover = contains(hit, desk.mouse_x, desk.mouse_y);
    rounded(dot, S(5),
            wm_focused() == w || hover ? lights[i] : theme_color(0xC3CCCD));
    if (hover)
      icon(dot.x + S(2), dot.y + S(2),
           i == 0   ? I_CLOSE
           : i == 1 ? I_MINUS
                    : I_MAX,
           S(8), 0x634D40);
  }
  clip_set(intersect(saved, window_body(w)));
  if (apps[w->app].draw) {
    apps[w->app].draw(w);
  }
  clip_set(intersect(saved, b));
  border(b, radius,
         wm_focused() == w ? THEME_WINDOW_BORDER : theme_color(0xE4E9E7));
  desk.mouse_x = pointer_x;
  desk.mouse_y = pointer_y;
  desk.clip = saved;
}

static void window_action(Window *w, int action) {
  if (action == 0) {
    wm_minimize(w);
  } else if (action == 1) {
    wm_maximize(w);
  } else if (action == 2) {
    wm_close(w);
  } else if (action == 3) {
    wm_snap(w, false);
  } else if (action == 4) {
    wm_snap(w, true);
  } else if (action == 5) {
    snap_show(w);
  } else if (action >= 10) {
    window_move_workspace(w, action - 10);
  }
}

void wm_actions(Window *w, int x, int y) {
  MenuItem items[9] = {{"Minimize", I_MINUS, 0, true},
                       {w->maximized ? "Restore" : "Maximize", I_MAX, 1, true},
                       {"Snap left", I_BACK, 3, true},
                       {"Snap right", I_FORWARD, 4, true},
                       {"Snap layouts  Super+Z", I_LAYOUT, 5, true}};
  static const char *names[] = {"Move to workspace 1", "Move to workspace 2",
                                "Move to workspace 3"};
  int n = 5;
  for (int i = 0; i < desk.workspace_count; i++) {
    items[n++] = (MenuItem){names[i], I_WORKSPACE, 10 + i, w->workspace != i};
  }
  items[n++] = (MenuItem){"Close", I_CLOSE, 2, true};
  menu_open(w, x, y, items, n, window_action);
}

Window *wm_hit_test(int x, int y) {
  Window *hit = NULL;
  if (!wm_fullscreen_active() &&
      (y < S(DESKTOP_TOP) || y >= desk.height - desktop_bottom()))
    return NULL;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *w = &desk.windows[i];
    Rect r = {w->box.x - S(6), w->box.y - S(6), w->box.w + S(12),
              w->box.h + S(12)};
    if (w->alive && !w->minimized && w->workspace == desk.workspace &&
        contains(r, x, y) && (!hit || w->z > hit->z)) hit = w;
  }
  return hit;
}

int wm_resize_edges(Window *w, int x, int y) {
  if (!w || w->maximized || w->fullscreen) return 0;
  Rect b = w->box;
  int edge = 0;
  if (x < b.x + S(6)) edge |= 1;
  if (x >= b.x + b.w - S(6)) edge |= 2;
  if (y < b.y + S(6)) edge |= 4;
  if (y >= b.y + b.h - S(6)) edge |= 8;
  return edge;
}

void wm_mouse(int x, int y, unsigned pressed, unsigned released, int wheel) {
  if (desk.drag) {
    Window *w = desk.drag;
    Rect before = w->box;
    bool finished = !(desk.buttons & 1) || (released & 1);
    int dx = x - desk.drag_start_x, dy = y - desk.drag_start_y;
    if (desk.drag_mode == 16) {
      if (!desk.drag_started && (dx >= S(3) || dx <= -S(3) || dy >= S(3) || dy <= -S(3))) {
        desk.drag_started = true;
        desk.last_click = 0; /* A completed drag is not a title-bar click. */
      }
      if (!desk.drag_started) {
        if (finished) { desk.drag = NULL; desk.cursor_dirty = true; }
        return;
      }
      if (w->maximized || w->snapped) {
        /* A title-bar click only focuses. Detach after real movement, keeping
           the same proportional grab point under the pointer. */
        int grab=(int64_t)(desk.drag_start_x-before.x)*w->restore.w/MAX(1,before.w);
        w->box=w->restore;w->maximized=w->snapped=false;
        w->box.x=x-clampi(grab,0,w->box.w);
        w->box.y=y-S(WINDOW_HEADER/2);clamp_window(w);
        desk.drag_start=w->box;desk.drag_start_x=x;desk.drag_start_y=y;dx=dy=0;
      }
      w->box.x = desk.drag_start.x + dx;
      w->box.y = desk.drag_start.y + dy;
      clamp_window(w);
    } else {
      Rect b = desk.drag_start;
      int minw = MIN(S(480), desk.width),
          minh = MIN(S(340), desk.height - desktop_bottom() - b.y);
      if (desk.drag_mode & 1) {
        int right = b.x + b.w;
        minw = MAX(minw, right - (desk.width - MIN(S(96), minw)));
        b.x = clampi(b.x + dx, right - desk.width, right - minw);
        b.w = right - b.x;
      }
      if (desk.drag_mode & 2) {
        minw = MAX(minw, MIN(S(96), minw) - b.x);
        b.w = clampi(b.w + dx, minw, desk.width);
      }
      if (desk.drag_mode & 4) {
        int bottom = b.y + b.h;
        b.y = clampi(b.y + dy, S(DESKTOP_TOP), bottom - minh);
        b.h = bottom - b.y;
      }
      if (desk.drag_mode & 8) {
        b.h = clampi(b.h + dy, minh, desk.height - desktop_bottom() - b.y);
      }
      w->box = b;
    }

    if (memcmp(&before, &w->box, sizeof(before))) {
      Rect after = w->box;
      if((w->app==APP_WORKBENCH || w->app==APP_QT || w->app==APP_CHROMIUM) && desk.drag_mode!=16 && before.x==after.x && before.y==after.y && !game_window_loading(w)) {
        damage_native_resize(w,before);wm_trace_geometry(w);
      } else {
        w->box=before;damage_window(w);w->box=after;
        if(desk.drag_mode==16)damage_window(w);
        else {invalidate_window(w);if(native_window_app(w->app))wm_trace_geometry(w);}
      }
      uint64_t time = now_ms();
      if (time - desk.drag_motion_at > 50) {
        desk.drag_frame_at = 0;
      }
      desk.drag_motion_at = time;
      desk.drag_moved = true;
    }

    if (desk.drag_mode == 16) snap_drag(w, x, y, finished);
    if (finished) {
      wm_trace_geometry(w);
      desk.drag = NULL;
      desk.cursor_dirty = true;
    }

    return;
  }

  Window *hit = wm_hit_test(x, y);

  if (!hit) {
    return;
  }
  Window *w = hit;
  Rect b = w->box;
  if (pressed) {
    wm_focus(w);
  }
  int edge = wm_resize_edges(w, x, y);

  if ((pressed & 1) && edge) {
    w->snapped = false;
    snap_cancel();
    desk.drag = w;
    desk.drag_started = false;
    desk.drag_mode = edge;
    desk.drag_start = b;
    desk.drag_start_x = x;
    desk.drag_start_y = y;
    return;
  }

  if (!w->fullscreen && y >= b.y && y < b.y + S(WINDOW_HEADER)) {
    if ((pressed & 2)) {
      wm_actions(w, x, y);
      return;
    }
    if (pressed & 1) {
      for (int i = 0; i < 3; i++) {
        if (contains(window_control(w, i), x, y)) {
          window_action(w, i == 0 ? 2 : i == 1 ? 0 : 1);
          return;
        }
      }

      if (apps[w->app].toolbar_mouse &&
          apps[w->app].toolbar_mouse(w, x, y, pressed)) {
        return;
      }
      uint64_t now = now_ms();
      if (desk.last_click_window == w->id && desk.last_click_hit == -99 &&
          now - desk.last_click < 400) {
        wm_maximize(w);
        desk.last_click = 0;
        return;
      }

      desk.last_click = now;
      desk.last_click_window = w->id;
      desk.last_click_hit = -99;
      snap_cancel();
      desk.drag = w;
      desk.drag_started = false;
      desk.drag_mode = 16;
      desk.drag_start = w->box;
      desk.drag_start_x = x;
      desk.drag_start_y = y;
    }

    return;
  }

  if (menubar_mouse(w, x, y, pressed))
    return;
  if (contains(window_body(w), x, y) && apps[w->app].mouse) {
    apps[w->app].mouse(w, x, y, pressed, wheel);
  }
}

bool wm_key(Key k) {
  static int cycle[MAX_WINDOWS], cycle_count, cycle_index;
  static bool cycling;
  if (!k.down && (k.code == 0xe2 || k.code == 0xe6)) {
    cycling = false;
  }
  if (!k.down) {
    return false;
  }
  Window *w = wm_focused();
  if (snap_key(k)) return true;
  if (k.code == KEY_ESCAPE && (desk.snap_open || desk.snap_preview)) {
    snap_cancel();
    return true;
  }
  if (k.mods & MOD_SUPER) {
    if ((k.ascii | 32) == 'z') { snap_show(w); return true; }
    if ((k.code >= 0x1e && k.code <= 0x20) ||
        (k.ascii >= '1' && k.ascii <= '3')) {
      int ws = k.code >= 0x1e && k.code <= 0x20 ? k.code - 0x1e : k.ascii - '1';
      if ((k.mods & MOD_SHIFT) && w) {
        window_move_workspace(w, ws);
      } else {
        workspace_switch(ws);
      }
      return true;
    }

    if (k.code == KEY_LEFT || k.code == KEY_RIGHT) {
      wm_snap(w, k.code == KEY_RIGHT);
      return true;
    }
    if (k.code == KEY_UP) {
      if (w && !w->maximized) {
        wm_maximize(w);
      }
      return true;
    }

    if (k.code == KEY_DOWN) {
      if (w && w->snapped) {
        damage_window(w); w->box = w->restore; w->snapped = false;
        invalidate_window(w); wm_trace_geometry(w);
      } else if (w && w->maximized) {
        wm_maximize(w);
      } else {
        wm_minimize(w);
      }
      return true;
    }
  }

  if ((k.mods & MOD_ALT) && k.code == KEY_F4) {
    wm_close(w);
    return true;
  }
  if ((k.mods & MOD_ALT) && k.code == KEY_TAB) {
    if (!cycling) {
      cycle_count = 0;
      for (int i = 0; i < MAX_WINDOWS; i++) {
        if (desk.windows[i].alive &&
            desk.windows[i].workspace == desk.workspace) {
          cycle[cycle_count++] = i;
        }
      }
      for (int i = 1; i < cycle_count; i++) {
        int value = cycle[i], j = i;
        while (j && desk.windows[cycle[j - 1]].z < desk.windows[value].z) {
          cycle[j] = cycle[j - 1];
          j--;
        }
        cycle[j] = value;
      }

      cycle_index = 0;
      cycling = true;
    }

    if (cycle_count) {
      cycle_index =
          (cycle_index + ((k.mods & MOD_SHIFT) ? cycle_count - 1 : 1)) %
          cycle_count;
      Window *next = &desk.windows[cycle[cycle_index]];
      if (next->alive) {
        wm_focus(next);
      }
    }

    return true;
  }

  return false;
}

void wm_arrange(bool tile) {
  Window *order[MAX_WINDOWS];
  int count = 0;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *w = &desk.windows[i];
    if (!w->alive || w->minimized || w->workspace != desk.workspace)
      continue;
    int j = count++;
    while (j && order[j - 1]->z < w->z) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = w;
  }
  if (tile) {
    if (count < 2)
      return;
    wm_snap(order[1], false);
    wm_snap(order[0], true);
  } else {
    int width = MIN(S(940), desk.width - S(32)),
        height =
            MIN(S(660), desk.height - S(DESKTOP_TOP + DESKTOP_BOTTOM + 32));
    int step = MIN(
        S(24), MIN(MAX(0, desk.width - width - S(16)),
                   MAX(0, desk.height - desktop_bottom() - height - S(16))) /
                   MAX(1, count - 1));
    for (int i = count - 1; i >= 0; i--) {
      Window *w = order[i];
      invalidate_window(w);
      w->maximized = false;
      w->box = (Rect){S(16) + (count - 1 - i) * step,
                      S(16) + (count - 1 - i) * step, width, height};
      clamp_window(w);
      w->restore = w->box;
      invalidate_window(w);
      wm_trace_geometry(w);
    }
    trace("CASCADE", NULL, "workspace");
  }
}
