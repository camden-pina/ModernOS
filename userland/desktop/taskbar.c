#include "desktop.h"

static const App pinned[] = {APP_FILES, APP_TERMINAL, APP_BROWSER, APP_SETTINGS, APP_EDITOR};

static bool visible_item(int item) {
    if (item < 4) {
        return true;
    }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (desk.windows[i].alive && desk.windows[i].app == pinned[item]) {
            return true;
        }
    }
    return false;
}

static Rect app_box(int n) {
    return (Rect){S(65 + n * 39), desk.height - S(36), S(34), S(31)};
}

static int workspace_start(void) {
    int total = S(desk.width < S(820) ? 255 : 353);
    return MAX(S(231), desk.width - total);
}

static Rect workspace_box(int n) {
    int start = workspace_start();

    return (Rect){start + (desk.width < S(820) ? 0 : S(89)) + n * S(23), desk.height - S(33), S(23),
                  S(24)};
}

Rect taskbar_hover(int x, int y) {
    Rect launcher = {S(16), desk.height - S(36), S(34), S(31)};
    if (contains(launcher, x, y)) {
        return launcher;
    }
    for (int i = 0; i < 5; i++) {
        Rect r = app_box(i);
        if (visible_item(i) && contains(r, x, y)) {
            return r;
        }
    }

    for (int i = 0; i < desk.workspace_count; i++) {
        Rect r = workspace_box(i);
        if (contains(r, x, y)) {
            return r;
        }
    }

    return (Rect){0};
}

void taskbar_draw(void) {
    int y = desk.height - S(42);
    Rect b = {0, y, desk.width, S(42)};
    blend(b, THEME_TASKBAR, 220);
    blend((Rect){0, y, desk.width, 1}, 0xC9DDE0, 40);
    Rect launcher = {S(16), y + S(6), S(34), S(31)};
    if (desk.launcher || contains(launcher, desk.mouse_x, desk.mouse_y)) {
        rounded(launcher, S(4), 0x3B505B);
    }
    icon(S(25), y + S(13), I_GRID, S(17), 0xD5E4E6);
    blend((Rect){S(57), y + S(11), 1, S(20)}, 0x94ADB5, 50);
    for (int i = 0; i < 5; i++) {
        if (!visible_item(i)) {
            continue;
        }
        Rect r = app_box(i);
        bool running = false, local = false, focused = false;
        for (int j = 0; j < MAX_WINDOWS; j++) {
            Window *w = &desk.windows[j];
            if (w->alive && w->app == pinned[i]) {
                running = true;
                if (w->workspace == desk.workspace) {
                    local = true;
                }
                if (w == wm_focused()) {
                    focused = true;
                }
            }
        }

        if (focused) {
            blend(r, 0x7B999D, 69);
        } else if (contains(r, desk.mouse_x, desk.mouse_y)) {
            blend(r, 0x7B999D, 37);
        }
        icon(r.x + S(8), r.y + S(6), apps[pinned[i]].icon, S(17), 0xD5E4E6);
        if (running) {
            rounded((Rect){r.x + S(local ? 11 : 15), r.y + S(28), S(local ? 13 : 4), S(2)}, S(1),
                    focused ? THEME_ACTIVE
                    : local ? 0x9AAFAD
                            : 0x708890);
        }
    }

    int start = workspace_start();
    if (desk.width >= S(820)) {
        label(start, y + S(15), "WORKSPACES", FONT_SMALL, 0x9DAEB3, S(83));
    }
    for (int i = 0; i < desk.workspace_count; i++) {
        Rect r = workspace_box(i);
        if (desk.workspace == i) {
            rounded(r, S(4), THEME_SELECTION);
        } else if (contains(r, desk.mouse_x, desk.mouse_y)) {
            blend(r, 0x78949C, 45);
        }
        char n[2] = {(char)('1' + i), 0};
        label(r.x + S(8), r.y + S(5), n, desk.workspace == i ? FONT_MEDIUM : FONT_NORMAL,
              desk.workspace == i ? 0x285C4F : 0xACBDC1, S(14));
    }

    int tray = desk.width - S(163);
    blend((Rect){tray, y + S(11), 1, S(20)}, 0x94ADB5, 50);
    icon(tray + S(17), y + S(13), I_WIFI, S(15), 0xA4B5BA);
    icon(tray + S(49), y + S(13), I_SOUND, S(15), 0xA4B5BA);
    int clock_x = desk.width - S(82), clock_w = S(69);
    int tw = text_width(desk.clock, FONT_MEDIUM), dw = text_width(desk.date, FONT_SMALL);
    label(clock_x + (clock_w - tw) / 2, y + S(6), desk.clock, FONT_MEDIUM, 0xDDE9EA, clock_w);
    label(clock_x + (clock_w - dw) / 2, y + S(23), desk.date, FONT_SMALL, 0xAABDC0, clock_w);
}

static char chooser_labels[MAX_WINDOWS][112];

static void choose_window(Window *unused, int action) {
    (void)unused;
    if (action >= 100 && action < 100 + APP_COUNT) {
        wm_open(action - 100, NULL);
        return;
    }
    if (action >= 0 && action < MAX_WINDOWS) {
        wm_focus(&desk.windows[action]);
    }
}

static void app_activate(App app, bool chooser, int x, int y) {
    Window *recent = NULL;
    MenuItem items[MAX_WINDOWS + 1];
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &desk.windows[i];
        if (!w->alive || w->app != app) {
            continue;
        }
        if (!recent || (w->workspace == desk.workspace && recent->workspace != desk.workspace) ||
            (w->workspace == recent->workspace && w->z > recent->z)) {
            recent = w;
        }
        copy_text(chooser_labels[n], sizeof(chooser_labels[n]), w->title);
        append_text(chooser_labels[n], sizeof(chooser_labels[n]), "  [workspace ");
        char num[24];
        format_uint(num, w->workspace + 1);
        append_text(chooser_labels[n], sizeof(chooser_labels[n]), num);
        append_text(chooser_labels[n], sizeof(chooser_labels[n]), "]");
        items[n] = (MenuItem){chooser_labels[n], apps[app].icon, i, true};
        n++;
    }

    if (chooser) {
        items[n++] = (MenuItem){"New window", apps[app].icon, 100 + app, true};
        menu_open(NULL, x, y - n * S(29) - S(12), items, n, choose_window);
    } else if (recent) {
        wm_focus(recent);
    } else {
        wm_open(app, NULL);
    }
}

bool taskbar_mouse(int x, int y, unsigned pressed) {
    if (desk.launcher && contains(desk.launcher_box, x, y)) {
        if (pressed & 1) {
            int row = (y - desk.launcher_box.y - S(62)) / S(39);
            /* Visible rows follow the exact same filtered registration list. */
            int match = 0;
            for (int i = 0; i < APP_COUNT + 1; i++) {
                const char *name = i < APP_COUNT ? apps[i].name : "Return to console";
                bool found = !desk.launcher_query[0];
                for (int a = 0; name[a] && !found; a++) {
                    int b = 0;
                    while (desk.launcher_query[b] && name[a + b]) {
                        char c = name[a + b], d = desk.launcher_query[b];
                        if (c >= 'A' && c <= 'Z') {
                            c += 32;
                        }
                        if (d >= 'A' && d <= 'Z') {
                            d += 32;
                        }
                        if (c != d) {
                            break;
                        }
                        b++;
                    }

                    found = !desk.launcher_query[b];
                }

                if (found && match++ == row) {
                    launcher_close();
                    if (i == APP_COUNT) {
                        desktop_quit();
                    } else {
                        wm_open(i, NULL);
                    }
                    break;
                }
            }
        }

        return true;
    }

    if (y < desk.height - S(42)) {
        if (desk.launcher && pressed) {
            launcher_close();
        }
        return false;
    }

    if (!pressed) {
        return true;
    }
    if (contains((Rect){S(16), desk.height - S(36), S(34), S(31)}, x, y)) {
        launcher_toggle();
        return true;
    }
    for (int i = 0; i < 5; i++) {
        if (visible_item(i) && contains(app_box(i), x, y)) {
            app_activate(pinned[i], pressed & 2, x, y);
            return true;
        }
    }

    for (int i = 0; i < desk.workspace_count; i++) {
        if (contains(workspace_box(i), x, y)) {
            workspace_switch(i);
            return true;
        }
    }

    int tray = desk.width - S(163);
    if (x >= tray + S(9) && x < tray + S(42)) {
        dialog_notice(NULL, "Wi-Fi",
                      "Wi-Fi status and configuration are not implemented. No connection status is "
                      "available.");
    } else if (x >= tray + S(42) && x < tray + S(79)) {
        dialog_notice(
            NULL, "Sound",
            "The sound service is not implemented. No volume or output status is available.");
    } else if (x >= desk.width - S(84)) {
        dialog_notice(
            NULL, "System clock",
            "This clock uses Limine's firmware boot time plus the OS monotonic timer. Displayed "
            "times are UTC. RTC synchronization and a calendar are not implemented.");
    }

    return true;
}

void launcher_close(void) {
    if (!desk.launcher) {
        return;
    }
    Rect b = desk.launcher_box;
    desk.launcher = false;
    /* Shadows extend beyond the popup. Clear their complete bounds as well. */
    invalidate((Rect){b.x - S(8), b.y - S(8), b.w + S(16), b.h + S(16)});
    invalidate((Rect){S(16), desk.height - S(42), S(36), S(42)});
}

void launcher_toggle(void) {
    menu_close();
    if (desk.dialog.open) {
        return;
    }
    if (desk.launcher) {
        launcher_close();
        return;
    }
    desk.launcher = true;
    int width = MIN(S(326), desk.width - S(30)), height = S(317);
    desk.launcher_box = (Rect){S(16), desk.height - S(50) - height, width, height};
    desk.launcher_query[0] = 0;
    desk.launcher_focus = 0;
    invalidate((Rect){desk.launcher_box.x - S(8), desk.launcher_box.y - S(8), width + S(16),
                      height + S(16)});
    invalidate((Rect){S(16), desk.height - S(42), S(36), S(42)});
}

static bool matches(const char *name) {
    if (!desk.launcher_query[0]) {
        return true;
    }
    for (int a = 0; name[a]; a++) {
        int b = 0;
        while (desk.launcher_query[b] && name[a + b]) {
            char c = name[a + b], d = desk.launcher_query[b];
            if (c >= 'A' && c <= 'Z') {
                c += 32;
            }
            if (d >= 'A' && d <= 'Z') {
                d += 32;
            }
            if (c != d) {
                break;
            }
            b++;
        }

        if (!desk.launcher_query[b]) {
            return true;
        }
    }

    return false;
}

void launcher_draw(void) {
    Rect b = desk.launcher_box;
    blend((Rect){b.x + S(3), b.y + S(5), b.w, b.h}, THEME_SHADOW, 40);
    rounded(b, S(9), THEME_WINDOW);
    border(b, S(9), THEME_WINDOW_BORDER);
    Rect input = {b.x + S(12), b.y + S(12), b.w - S(24), S(30)};
    input_draw(input, desk.launcher_query, "Search applications", strlen(desk.launcher_query),
               true);
    int row = 0;
    for (int i = 0; i < APP_COUNT + 1; i++) {
        const char *name = i < APP_COUNT ? apps[i].name : "Return to console";
        if (!matches(name)) {
            continue;
        }
        Rect r = {b.x + S(8), b.y + S(62) + row * S(39), b.w - S(16), S(35)};
        if (row == desk.launcher_focus || contains(r, desk.mouse_x, desk.mouse_y)) {
            rounded(r, S(5), row == desk.launcher_focus ? desk.selection : theme_color(0xF0F4F1));
        }
        icon(r.x + S(10), r.y + S(9), i < APP_COUNT ? apps[i].icon : I_CLOSE, S(17),
             theme_color(0x617176));
        label(r.x + S(39), r.y + S(9), name, FONT_BODY, theme_color(0x41525A), r.w - S(52));
        row++;
    }

    if (!row) {
        label(b.x + S(20), b.y + S(67), "No matching application", FONT_BODY, THEME_SECONDARY_TEXT,
              b.w - S(40));
    }
}

bool taskbar_key(Key k) {
    if ((k.mods & MOD_SUPER) && k.down && k.code == KEY_SPACE) {
        desk.super_used = true;
        launcher_toggle();
        return true;
    }
    if (!desk.launcher || !k.down) {
        return false;
    }
    int count = 0, indices[APP_COUNT + 1];
    for (int i = 0; i < APP_COUNT + 1; i++) {
        if (matches(i < APP_COUNT ? apps[i].name : "Return to console")) {
            indices[count++] = i;
        }
    }
    if (k.code == KEY_ESCAPE) {
        launcher_toggle();
    } else if (k.code == KEY_UP || k.code == KEY_DOWN || k.code == KEY_TAB) {
        if (count) {
            desk.launcher_focus =
                (desk.launcher_focus + (k.code == KEY_UP ? -1 : 1) + count) % count;
        }
        invalidate(desk.launcher_box);
    } else if (k.code == KEY_ENTER && count) {
        int app = indices[clampi(desk.launcher_focus, 0, count - 1)];
        launcher_close();
        if (app == APP_COUNT) {
            desktop_quit();
        } else {
            wm_open(app, NULL);
        }
    } else {
        int cursor = strlen(desk.launcher_query);
        if (input_key(desk.launcher_query, sizeof(desk.launcher_query), &cursor, k)) {
            desk.launcher_focus = 0;
            invalidate(desk.launcher_box);
        }
    }

    return true;
}
