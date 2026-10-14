#include "desktop.h"

Rect window_body(Window *w) {
    return (Rect){w->box.x + 1, w->box.y + S(40), w->box.w - 2, w->box.h - S(40) - 1};
}

Window *wm_focused(void) {
    if (desk.focused < 0 || desk.focused >= MAX_WINDOWS) {
        return NULL;
    }
    Window *w = &desk.windows[desk.focused];
    return w->alive && !w->minimized && w->workspace == desk.workspace ? w : NULL;
}

static void taskbar_dirty(void) {
    invalidate((Rect){0, desk.height - S(42), desk.width, S(42)});
}

static void clamp_window(Window *w) {
    int work = desk.height - S(42), minw = MIN(S(530), desk.width), minh = MIN(S(340), work);
    w->box.w = clampi(w->box.w, minw, desk.width);
    w->box.h = clampi(w->box.h, minh, work);
    w->box.x = clampi(w->box.x, 0, desk.width - w->box.w);
    w->box.y = clampi(w->box.y, 0, work - w->box.h);
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
    w->minimized = false;
    w->z = ++desk.z;
    desk.focused = w - desk.windows;
    invalidate_window(w);
    taskbar_dirty();
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
    Window *w = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!desk.windows[i].alive) {
            w = &desk.windows[i];
            break;
        }
    }

    if (!w) {
        dialog_notice(
            NULL, "Window limit",
            "Close a window before opening another. The desktop supports 16 simultaneous windows.");
        return NULL;
    }

    memset(w, 0, sizeof(*w));
    w->app = app;
    w->workspace = desk.workspace;
    w->id = ++desk.next_id;
    w->alive = true;
    copy_text(w->title, sizeof(w->title), apps[app].name);
    int width = desk.width * 78 / 100, height = desk.height * 788 / 1000;
    w->box = (Rect){(desk.width - width) / 2, desk.height * 8 / 100, width, height};
    if (app != APP_FILES) {
        w->box.w = MIN(S(app == APP_SETTINGS ? 680 : 840), desk.width - S(50));
        w->box.h = MIN(S(app == APP_SETTINGS ? 550 : 560), desk.height - S(92));
        w->box.x = (desk.width - w->box.w) / 2;
        w->box.y = (desk.height - S(42) - w->box.h) / 2;
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
    return w;
}

void wm_minimize(Window *w) {
    if (!w) {
        return;
    }
    invalidate_window(w);
    w->minimized = true;
    focus_top();
    taskbar_dirty();
    trace("MINIMIZE", w, "");
}

void wm_maximize(Window *w) {
    if (!w) {
        return;
    }
    invalidate_window(w);
    if (w->maximized) {
        w->box = w->restore;
        w->maximized = false;
        clamp_window(w);
    } else {
        w->restore = w->box;
        w->box = (Rect){0, 0, desk.width, desk.height - S(42)};
        w->maximized = true;
    }
    invalidate_window(w);
    trace(w->maximized ? "MAXIMIZE" : "RESTORE", w, "");
}

void wm_close(Window *w) {
    if (!w || !w->alive) {
        return;
    }
    if (apps[w->app].can_close && !apps[w->app].can_close(w)) {
        return;
    }
    if (apps[w->app].close && !apps[w->app].close(w)) {
        return;
    }
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
}

void wm_snap(Window *w, bool right) {
    if (!w) {
        return;
    }
    invalidate_window(w);
    if (!w->maximized) {
        w->restore = w->box;
    }
    w->maximized = false;
    w->box = (Rect){right ? desk.width / 2 : 0, 0, desk.width / 2, desk.height - S(42)};
    invalidate_window(w);
    trace("SNAP", w, right ? "right" : "left");
}

void workspace_switch(int ws) {
    if (ws < 0 || ws >= desk.workspace_count || ws == desk.workspace) {
        return;
    }
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
    focus_top();
    taskbar_dirty();
    trace("MOVE_WORKSPACE", w, "");
}

void wm_paint(Window *w) {
    Rect saved = desk.clip, b = w->box;
    int radius = w->maximized ? 0 : S(9);
    uint32_t corner_pixels[4][18 * 18];
    for (int corner = 0; corner < 4; corner++) {
        for (int yy = 0; yy < radius; yy++) {
            for (int xx = 0; xx < radius; xx++) {
                int x = (corner & 1) ? b.x + b.w - 1 - xx : b.x + xx;
                int y = (corner & 2) ? b.y + b.h - 1 - yy : b.y + yy;
                if (contains(saved, x, y)) {
                    corner_pixels[corner][yy * 18 + xx] = *paint_pixel(x, y);
                }
            }
        }
    }

    clip_set(intersect(saved, b));
    rounded(b, radius, THEME_WINDOW);
    Rect toolbar = {b.x, b.y, b.w, S(40)};
    rounded(toolbar, w->maximized ? 0 : S(9), THEME_TOOLBAR);
    fill((Rect){b.x, b.y + S(10), b.w, S(30)}, THEME_TOOLBAR);
    fill((Rect){b.x, b.y + S(39), b.w, 1}, theme_color(0xE2E8E7));
    rounded((Rect){b.x + S(13), b.y + S(8), S(23), S(23)}, S(5),
            w->app == APP_FILES ? theme_color(0xE0EDE7) : theme_color(0xEAF0EF));
    icon(b.x + S(17), b.y + S(12), apps[w->app].icon, S(15), desk.accent);
    const AppOps *app = &apps[w->app];
    label(b.x + S(44), b.y + S(12), app->toolbar ? app->name : w->title, FONT_BOLD,
          theme_color(0x30434A), app->toolbar ? text_width(app->name, FONT_BOLD) : b.w - S(145));
    if (app->toolbar) {
        app->toolbar(w);
    }
    for (int i = 0; i < 3; i++) {
        Rect r = {b.x + b.w - S(79) + i * S(24), b.y + S(8), S(23), S(25)};
        bool hover = contains(r, desk.mouse_x, desk.mouse_y);
        if (hover) {
            rounded(r, S(4), i == 2 ? theme_color(0xF8E7E5) : theme_color(0xEAF0ED));
        }
        Icon kind = i == 0 ? I_MINUS : i == 1 ? (w->maximized ? I_RESTORE : I_MAX) : I_CLOSE;
        icon(r.x + S(5), r.y + S(6), kind, S(13),
             hover && i == 2 ? theme_color(0xB86B65) : theme_color(0x7E8C90));
    }

    clip_set(intersect(saved, window_body(w)));
    if (apps[w->app].draw) {
        apps[w->app].draw(w);
    }
    clip_set(intersect(saved, b));
    border(b, radius, wm_focused() == w ? THEME_WINDOW_BORDER : theme_color(0xE4E9E7));
    for (int corner = 0; corner < 4; corner++) {
        for (int yy = 0; yy < radius; yy++) {
            for (int xx = 0; xx < radius; xx++) {
                int dx = 2 * (radius - xx) - 1, dy = 2 * (radius - yy) - 1;
                if (dx * dx + dy * dy <= 4 * radius * radius) {
                    continue;
                }
                int x = (corner & 1) ? b.x + b.w - 1 - xx : b.x + xx;
                int y = (corner & 2) ? b.y + b.h - 1 - yy : b.y + yy;
                if (contains(saved, x, y)) {
                    *paint_pixel(x, y) = corner_pixels[corner][yy * 18 + xx];
                }
            }
        }
    }

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
    } else if (action >= 10) {
        window_move_workspace(w, action - 10);
    }
}

void wm_actions(Window *w, int x, int y) {
    MenuItem items[8] = {{"Minimize", I_MINUS, 0, true},
                         {w->maximized ? "Restore" : "Maximize", I_MAX, 1, true},
                         {"Snap left", I_BACK, 3, true},
                         {"Snap right", I_FORWARD, 4, true}};
    static const char *names[] = {"Move to workspace 1", "Move to workspace 2",
                                  "Move to workspace 3"};
    int n = 4;
    for (int i = 0; i < desk.workspace_count; i++) {
        items[n++] = (MenuItem){names[i], I_WORKSPACE, 10 + i, w->workspace != i};
    }
    items[n++] = (MenuItem){"Close", I_CLOSE, 2, true};
    menu_open(w, x, y, items, n, window_action);
}

void wm_mouse(int x, int y, unsigned pressed, unsigned released, int wheel) {
    if (desk.drag) {
        Window *w = desk.drag;
        Rect before = w->box;
        bool finished = !(desk.buttons & 1) || (released & 1);
        damage_window(w);
        int dx = x - desk.drag_start_x, dy = y - desk.drag_start_y;
        if (desk.drag_mode == 16) {
            if (dx >= S(3) || dx <= -S(3) || dy >= S(3) || dy <= -S(3)) {
                desk.last_click = 0; /* A completed drag is not a title-bar click. */
            }
            w->box.x = desk.drag_start.x + dx;
            w->box.y = desk.drag_start.y + dy;
            clamp_window(w);
        } else {
            Rect b = desk.drag_start;
            int minw = MIN(S(530), desk.width), minh = MIN(S(340), desk.height - S(42));
            if (desk.drag_mode & 1) {
                int right = b.x + b.w;
                b.x = clampi(b.x + dx, 0, right - minw);
                b.w = right - b.x;
            }
            if (desk.drag_mode & 2) {
                b.w = clampi(b.w + dx, minw, desk.width - b.x);
            }
            if (desk.drag_mode & 4) {
                int bottom = b.y + b.h;
                b.y = clampi(b.y + dy, 0, bottom - minh);
                b.h = bottom - b.y;
            }
            if (desk.drag_mode & 8) {
                b.h = clampi(b.h + dy, minh, desk.height - S(42) - b.y);
            }
            w->box = b;
        }

        if (memcmp(&before, &w->box, sizeof(before))) {
            uint64_t time = now_ms();
            if (time - desk.drag_motion_at > 50) {
                desk.drag_frame_at = 0;
            }
            desk.drag_motion_at = time;
            desk.drag_moved = true;
        }

        if (desk.drag_mode == 16) {
            damage_window(w);
        } else {
            invalidate_window(w);
        }
        if (finished) {
            if (desk.trace) {
                printf("DESKTOP GEOMETRY window=%d x=%d y=%d width=%d height=%d\n", w->id, w->box.x,
                       w->box.y, w->box.w, w->box.h);
            }
            desk.drag = NULL;
        }

        return;
    }

    Window *hit = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &desk.windows[i];
        Rect r = {w->box.x - S(6), w->box.y - S(6), w->box.w + S(12), w->box.h + S(12)};
        if (w->alive && !w->minimized && w->workspace == desk.workspace && contains(r, x, y) &&
            (!hit || w->z > hit->z)) {
            hit = w;
        }
    }

    if (!hit) {
        return;
    }
    Window *w = hit;
    Rect b = w->box;
    if (pressed) {
        wm_focus(w);
    }
    int edge = 0;
    if (!w->maximized) {
        if (x < b.x + S(6)) {
            edge |= 1;
        }
        if (x >= b.x + b.w - S(6)) {
            edge |= 2;
        }
        if (y < b.y + S(6)) {
            edge |= 4;
        }
        if (y >= b.y + b.h - S(6)) {
            edge |= 8;
        }
    }

    if ((pressed & 1) && edge) {
        desk.drag = w;
        desk.drag_mode = edge;
        desk.drag_start = b;
        desk.drag_start_x = x;
        desk.drag_start_y = y;
        return;
    }

    if (y >= b.y && y < b.y + S(40)) {
        if ((pressed & 2)) {
            wm_actions(w, x, y);
            return;
        }
        if (pressed & 1) {
            for (int i = 0; i < 3; i++) {
                if (contains((Rect){b.x + b.w - S(79) + i * S(24), b.y + S(8), S(23), S(25)}, x,
                             y)) {
                    window_action(w, i);
                    return;
                }
            }

            if (apps[w->app].toolbar_mouse && apps[w->app].toolbar_mouse(w, x, y, pressed)) {
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
            if (w->maximized) {
                wm_maximize(w);
                w->box.x = clampi(x - w->box.w / 2, 0, desk.width - w->box.w);
                w->box.y = MAX(0, y - S(18));
            }
            desk.drag = w;
            desk.drag_mode = 16;
            desk.drag_start = w->box;
            desk.drag_start_x = x;
            desk.drag_start_y = y;
        }

        return;
    }

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
    if (k.mods & MOD_SUPER) {
        if ((k.code >= 0x1e && k.code <= 0x20) || (k.ascii >= '1' && k.ascii <= '3')) {
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
            if (w && w->maximized) {
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
                if (desk.windows[i].alive && desk.windows[i].workspace == desk.workspace) {
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
                (cycle_index + ((k.mods & MOD_SHIFT) ? cycle_count - 1 : 1)) % cycle_count;
            Window *next = &desk.windows[cycle[cycle_index]];
            if (next->alive) {
                wm_focus(next);
            }
        }

        return true;
    }

    return false;
}
