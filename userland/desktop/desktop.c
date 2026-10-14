#include "desktop.h"
#include "pixel_blend.h"

Desktop desk;
const AppOps apps[APP_COUNT] = {
    [APP_FILES] = {"Files", I_FOLDER, files_open, files_draw, files_mouse, files_key, files_tick,
                   files_close, NULL, files_toolbar, files_toolbar_mouse, files_toolbar_hover},
    [APP_TERMINAL] = {"Terminal", I_TERMINAL, terminal_open, terminal_draw, terminal_mouse,
                      terminal_key, terminal_tick, terminal_close, terminal_can_close, NULL, NULL,
                      NULL},
    [APP_BROWSER] = {"Browser", I_GLOBE, browser_open, browser_draw, browser_mouse, browser_key,
                     browser_tick, browser_close, NULL, browser_toolbar, browser_toolbar_mouse,
                     browser_toolbar_hover},
    [APP_SETTINGS] = {"Settings", I_GEAR, settings_open, settings_draw, settings_mouse,
                      settings_key, settings_tick, settings_close, NULL, NULL, NULL, NULL},
    [APP_EDITOR] = {"Text Editor", I_DOC, editor_open, editor_draw, editor_mouse, editor_key,
                    editor_tick, editor_close, editor_confirm_close, NULL, NULL, NULL}};

void recent_add(const char *path) {
    int existing = -1;
    for (int i = 0; i < desk.recent_count; i++) {
        if (!strcmp(desk.recent[i], path)) {
            existing = i;
        }
    }
    if (existing >= 0) {
        memmove(desk.recent + 1, desk.recent, existing * OS_PATH_MAX);
    } else {
        int n = MIN(desk.recent_count, MAX_RECENT - 1);
        memmove(desk.recent + 1, desk.recent, n * OS_PATH_MAX);
        if (desk.recent_count < MAX_RECENT) {
            desk.recent_count++;
        }
    }

    copy_text(desk.recent[0], OS_PATH_MAX, path);
}

void desktop_quit(void) {
    desk.quit_pending = true;
    menu_close();
    launcher_close();
    /* Ask before closing any unrelated window or terminating its processes. */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &desk.windows[i];
        if (w->alive && apps[w->app].can_close && !apps[w->app].can_close(w)) {
            return;
        }
    }

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (desk.windows[i].alive) {
            wm_close(&desk.windows[i]);
            if (desk.windows[i].alive) {
                return;
            }
        }
    }

    desk.running = false;
}

static void clock_update(void) {
    char time_text[16], date_text[32];
    struct os_timespec ts;
    long result = call2(SYS_clock_gettime, 0, &ts);
    if (result || ts.sec <= 0) {
        copy_text(time_text, sizeof(time_text), "--:--");
        copy_text(date_text, sizeof(date_text), "No time");
    } else {
        uint64_t minutes = (uint64_t)ts.sec / 60;
        unsigned hour = (minutes / 60) % 24, minute = minutes % 60;
        time_text[0] = '0' + hour / 10;
        time_text[1] = '0' + hour % 10;
        time_text[2] = ':';
        time_text[3] = '0' + minute / 10;
        time_text[4] = '0' + minute % 10;
        time_text[5] = 0;
        int64_t z = ts.sec / 86400 + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
        unsigned doe = z - era * 146097,
                 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365,
                 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        unsigned day = doy - (153 * mp + 2) / 5 + 1, month = mp + (mp < 10 ? 3 : -9);
        static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        copy_text(date_text, sizeof(date_text), months[month - 1]);
        append_text(date_text, sizeof(date_text), " ");
        char n[32];
        format_uint(n, day);
        append_text(date_text, sizeof(date_text), n);
    }

    if (strcmp(time_text, desk.clock) || strcmp(date_text, desk.date)) {
        copy_text(desk.clock, sizeof(desk.clock), time_text);
        copy_text(desk.date, sizeof(desk.date), date_text);
        invalidate((Rect){desk.width - S(84), desk.height - S(42), S(84), S(42)});
    }
}

static Rect hover(int x, int y) {
    if (desk.dialog.open) {
        return dialog_hover(x, y);
    }
    if (desk.menu.open && contains(desk.menu.box, x, y)) {
        Rect b = desk.menu.box;
        int row = clampi((y - b.y - S(6)) / S(29), 0, desk.menu.visible - 1);

        return (Rect){b.x + S(5), b.y + S(6) + row * S(29), b.w - S(10), S(29)};
    }
    if (desk.launcher && contains(desk.launcher_box, x, y)) {
        Rect b = desk.launcher_box;

        return (Rect){b.x, b.y + S(52), b.w, b.h - S(52)};
    }
    if (y >= desk.height - S(42)) {
        return taskbar_hover(x, y);
    }
    Window *hit = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &desk.windows[i];
        if (w->alive && !w->minimized && w->workspace == desk.workspace && contains(w->box, x, y) &&
            (!hit || w->z > hit->z)) {
            hit = w;
        }
    }

    if (!hit) {
        return (Rect){0};
    }
    Rect b = hit->box;
    if (y < b.y + S(40)) {
        if (x >= b.x + b.w - S(79)) {
            return (Rect){b.x + b.w - S(79), b.y + S(7), S(74), S(27)};
        }
        if (apps[hit->app].toolbar_hover) {
            return apps[hit->app].toolbar_hover(hit, x, y);
        }

        return (Rect){0};
    }

    if (hit->app == APP_FILES) {
        return files_hover(hit, x, y);
    }
    if (hit->app == APP_SETTINGS) {
        return settings_hover(hit, x, y);
    }
    if (hit->app == APP_EDITOR && y < b.y + S(82) && x < b.x + S(208)) {
        return (Rect){b.x + S(15), b.y + S(50), S(185), S(33)};
    }

    return (Rect){0};
}

static void dispatch_key(Key k) {
    desk.modifiers = k.mods;
    if (k.code == 0xe3 || k.code == 0xe7) {
        if (k.down) {
            desk.super_used = false;
        } else if (!desk.super_used) {
            launcher_toggle();
        }
        return;
    }

    if (k.down && (k.mods & MOD_SUPER)) {
        desk.super_used = true;
    }
    if (k.down && (k.mods & MOD_CTRL) && (k.mods & MOD_ALT) && k.code == KEY_ESCAPE) {
        desktop_quit();
        return;
    }
    if (wm_key(k)) {
        return;
    }
    if (desk.dialog.open) {
        dialog_key(k);
        return;
    }
    if (menu_key(k) || taskbar_key(k)) {
        return;
    }
    Window *w = wm_focused();
    if (w && apps[w->app].key) {
        apps[w->app].key(w, k);
    }
}

static void legacy_key(unsigned c) {
    static char sequence[16];
    static int used;
    if (used || c == 27) {
        if (!used) {
            sequence[used++] = 27;
            return;
        }
        if (used < 15) {
            sequence[used++] = c;
        }
        sequence[used] = 0;
        if (used == 2 && c != '[' && c != 'O') {
            used = 0;
            dispatch_key((Key){.code = KEY_ESCAPE, .down = true});
            return;
        }
        unsigned key = 0;
        if (used >= 3 && c >= 'A' && c <= 'Z') {
            switch (c) {
            case 'A':
                key = KEY_UP;
                break;
            case 'B':
                key = KEY_DOWN;
                break;
            case 'C':
                key = KEY_RIGHT;
                break;
            case 'D':
                key = KEY_LEFT;
                break;
            case 'H':
                key = KEY_HOME;
                break;
            case 'F':
                key = KEY_END;
                break;
            }
            used = 0;
        } else if (c == '~') {
            if (!strcmp(sequence, "\033[3~")) {
                key = KEY_DELETE;
            } else if (!strcmp(sequence, "\033[5~")) {
                key = KEY_PAGEUP;
            } else if (!strcmp(sequence, "\033[6~")) {
                key = KEY_PAGEDOWN;
            } else if (!strcmp(sequence, "\033[12~")) {
                key = KEY_F2;
            } else if (!strcmp(sequence, "\033[15~")) {
                key = KEY_F5;
            }
            used = 0;
        }

        if (key) {
            dispatch_key((Key){.code = key, .down = true});
        }
        return;
    }

    Key k = {.down = true, .ascii = c};
    if (c == 10 || c == 13) {
        k.code = KEY_ENTER;
    } else if (c == 8 || c == 127) {
        k.code = KEY_BACKSPACE;
    } else if (c == 9) {
        k.code = KEY_TAB;
    } else if (c > 0 && c < 27) {
        k.ascii = c + 'a' - 1;
        k.mods = 1;
    } else if (c == ' ') {
        k.code = KEY_SPACE;
    }
    dispatch_key(k);
}

static void pointer(struct os_input_event *e) {
    Rect previous = desk.drag ? (Rect){0} : hover(desk.mouse_x, desk.mouse_y);
    unsigned old = desk.buttons;
    desk.mouse_x = clampi((int64_t)desk.mouse_x + e->dx, 0, desk.width - 1);
    desk.mouse_y = clampi((int64_t)desk.mouse_y + e->dy, 0, desk.height - 1);
    desk.buttons = e->buttons;
    Rect current = desk.drag ? (Rect){0} : hover(desk.mouse_x, desk.mouse_y);
    if (memcmp(&previous, &current, sizeof(Rect))) {
        invalidate(previous);
        invalidate(current);
    }
    if (old != desk.buttons) {
        invalidate(current);
    }

    unsigned pressed = desk.buttons & ~old, released = old & ~desk.buttons;
    if (desk.trace && (pressed || released)) {
        printf("DESKTOP POINTER position=(%d,%d) buttons=%u pressed=%u released=%u sequence=%lu\n",
               desk.mouse_x, desk.mouse_y, desk.buttons, pressed, released, e->sequence);
    }

    if (desk.dialog.open) {
        dialog_mouse(desk.mouse_x, desk.mouse_y, pressed);
        return;
    }
    if (desk.menu.open && menu_mouse(desk.mouse_x, desk.mouse_y, pressed, e->wheel)) {
        return;
    }
    if (!desk.drag && taskbar_mouse(desk.mouse_x, desk.mouse_y, pressed)) {
        return;
    }

    wm_mouse(desk.mouse_x, desk.mouse_y, pressed, released, e->wheel);
}

int desktop_main(int argc, char **argv) {
    bool info_only = false, statistics = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            puts(
                "Usage: gui [--info] [--stats] [--trace] [--browser-network-trace] [--browser-exception-trace]\nModernOS desktop: Files, Terminal, Text "
                "Editor, Settings, and three workspaces.\nSuper: launcher; Super+1/2/3: "
                "workspaces; Alt+Tab: windows; Alt+F4: close.\nCtrl+Alt+Escape or launcher "
                "'Return to console': leave the desktop safely.\n--info displays capabilities; "
                "--stats reports actual damaged pixel traffic on exit.\n--trace logs desktop "
                "actions to serial for integration testing.\n--browser-network-trace logs "
                "HTTP methods and cookie names to serial, without cookie values.\n"
                "--browser-exception-trace observes genuine JavaScript exceptions and "
                "immediately resumes execution; debugger timing differs from normal browsing.");
            return 0;
        }

        if (!strcmp(argv[i], "--info")) {
            info_only = true;
        } else if (!strcmp(argv[i], "--trace")) {
            desk.trace = true;
        } else if (!strcmp(argv[i], "--stats")) {
            statistics = true;
        } else if (!strcmp(argv[i], "--browser-network-trace")) {
            desk.browser_network_trace = true;
        } else if (!strcmp(argv[i], "--browser-exception-trace")) {
            desk.browser_exception_trace = true;
        } else {
            puts(
                "Usage: gui [--info] [--stats] [--trace] [--browser-network-trace] [--browser-exception-trace]");
            return 2;
        }
    }

    struct os_display_info info;
    long error = call3(SYS_display, DISPLAY_INFO, &info, sizeof(info));
    if (error) {
        printf("gui: display unavailable: %s\n", error_text(error));
        return 1;
    }

    struct os_display_caps caps = {0};
    if (info.version >= 3) {
        call3(SYS_display, DISPLAY_CAPS, &caps, sizeof(caps));
    }
    desk.buffered_display = (caps.flags & DISPLAY_CAP_PAGE_FLIP) != 0;
    if (info_only) {
        printf("display ABI=%u %ux%u XRGB8888 max_rectangle_pixels=%u\nBatched damage "
               "presentation; cached wallpaper and glyphs; native applications.\n",
               info.version, info.width, info.height, info.max_pixels);
        printf("Display backend=%u buffers=%u page_flip=%u write_combining=%u large_pages=%u; "
               "120 Hz composition target.\n",
               caps.backend, caps.buffers, desk.buffered_display,
               !!(caps.flags & DISPLAY_CAP_WRITE_COMBINING),
               !!(caps.flags & DISPLAY_CAP_LARGE_PAGES));
        printf(
            "Shadow blending=%s; hardware refresh synchronization requires a supporting display driver.\n",
            pixels_blend_vectorized() ? "SSE2" : "integer (QEMU TCG)");
        return 0;
    }

    if (info.version < 2 || info.width < 640 || info.width > 8192 || info.height < 480 ||
        (uint64_t)info.width * info.height * 4 > 64 * 1024 * 1024) {
        puts("gui: display ABI 2 and a 640x480 or larger 32-bit framebuffer are required "
             "(8192 pixels maximum width, 64 MiB per surface maximum).");
        return 1;
    }

    desk.width = info.width;
    desk.height = info.height;
    size_t bytes = (size_t)desk.width * desk.height * 4;
    desk.scene = allocate(bytes);
    desk.canvas_present = info.version >= 3;
    /* Presentation is synchronous. Temporarily stamp and restore the cursor
       in the composed canvas instead of copying an entire second canvas. */
    desk.output = desk.scene;
    desk.wallpaper = allocate(bytes);
    if (!desk.scene || !desk.wallpaper) {
        error = -E_NOMEM;
        goto cleanup_memory;
    }

    desk.focused = -1;
    desk.mouse_x = desk.old_mouse_x = desk.width / 2;
    desk.mouse_y = desk.old_mouse_y = desk.height / 2;
    clip_set((Rect){0, 0, desk.width, desk.height});
    config_load();
    clock_update();

    struct os_termios saved = {0}, raw;
    error = ioctl(0, TCGETS, &saved);
    if (!error) {
        raw = saved;
        raw.lflag = 0;
        raw.iflag = 0;
        raw.oflag = 0;
        raw.cc[VMIN] = 0;
        raw.cc[VTIME] = 0;
        error = ioctl(0, TCSETSF, &raw);
    }

    if (!error) {
        error = call3(SYS_display, DISPLAY_ACQUIRE, 0, 0);
    }
    if (error) {
        ioctl(0, TCSETSF, &saved);
        goto cleanup_memory;
    }

    desk.running = true;
    wm_open(APP_FILES, NULL);
    invalidate_all();
    error = desktop_present();
    if (!error) {
        puts("GUI_READY: ModernOS desktop. Super opens launcher; Ctrl+Alt+Escape returns to "
             "console.");
    }

    while (desk.running && !error) {
        struct os_input_event events[64];
        long n = call2(SYS_input, events, 64);
        if (n < 0 && n != -E_AGAIN) {
            error = n;
            break;
        }
        for (long i = 0; i < n; i++) {
            struct os_input_event *e = &events[i];
            if (e->type == INPUT_POINTER) {
                /* Collapse motion bursts without crossing key, wheel or button
                   boundaries. Clamp each sample before combining, preserving
                   pointer behavior at screen edges and the final release position. */
                struct os_input_event motion = *e;
                int x = clampi((int64_t)desk.mouse_x + motion.dx, 0, desk.width - 1);
                int y = clampi((int64_t)desk.mouse_y + motion.dy, 0, desk.height - 1);
                while (!motion.code && !motion.wheel && motion.buttons == desk.buttons &&
                       i + 1 < n && events[i + 1].type == INPUT_POINTER && !events[i + 1].code &&
                       events[i + 1].buttons == motion.buttons && !events[i + 1].wheel) {
                    i++;
                    x = clampi((int64_t)x + events[i].dx, 0, desk.width - 1);
                    y = clampi((int64_t)y + events[i].dy, 0, desk.height - 1);
                }

                motion.dx = x - desk.mouse_x;
                motion.dy = y - desk.mouse_y;
                pointer(&motion);
            } else if (e->type == INPUT_KEY) {
                legacy_key(e->code);
            } else if (e->type == INPUT_HID_KEY) {
                Key k = {.code = e->code, .ascii = e->dx, .mods = e->dy, .down = e->buttons != 0};
                if (k.down && k.code < 0xe0) {
                    desk.held = k;
                    desk.repeat_at = now_ms() + 400;
                } else if (!k.down && k.code == desk.held.code) {
                    desk.held.down = false;
                }
                dispatch_key(k);
            } else if (e->type == INPUT_OVERFLOW) {
                desk.drag = NULL;
                desk.buttons = desk.modifiers = 0;
                desk.held.down = false;
                dialog_notice(NULL, "Input queue overflow",
                              "Release all keys and mouse buttons, then try again. No filesystem "
                              "action was reported as successful.");
            }
        }

        uint64_t now = now_ms();
        if (desk.held.down && now >= desk.repeat_at &&
            !(desk.held.mods & (MOD_ALT | MOD_SUPER | MOD_CTRL)) && desk.held.code != KEY_ENTER &&
            desk.held.code != KEY_DELETE && desk.held.code != KEY_F2) {
            Key repeated = desk.held;
            repeated.repeat = true;
            dispatch_key(repeated);
            desk.repeat_at = now + 35;
        }

        for (int i = 0; i < MAX_WINDOWS; i++) {
            Window *w = &desk.windows[i];
            if (w->alive && apps[w->app].tick) {
                apps[w->app].tick(w);
            }
        }

        terminal_reap();
        menu_tick();
        if (now >= desk.clock_at) {
            desk.clock_at = now + 1000;
            clock_update();
        }
        if (desk.quit_pending && !desk.dialog.open) {
            desktop_quit();
        }
        if (desk.running) {
            error = desktop_present();
        }
        if (n == -E_AGAIN || n == 0) {
            struct os_timespec delay = {0, 1000000};
            call2(SYS_nanosleep, &delay, 0);
        }
    }

    if (error) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            Window *w = &desk.windows[i];
            if (w->alive && w->app != APP_EDITOR) {
                wm_close(w);
            }
        }
    }

    call3(SYS_display, DISPLAY_RELEASE, 0, 0);
    ioctl(0, TCSETSF, &saved);

    if (statistics) {
        printf("GUI_STATS frames=%lu presents=%lu pixels=%lu full_frames=%lu screen_pixels=%lu\n",
               desk.frames, desk.present_calls, desk.presented_pixels, desk.full_frames,
               (uint64_t)desk.width * desk.height);
        printf("GUI_TIMING compose_us=%lu upload_us=%lu drag_frames=%lu drag_us=%lu "
               "drag_max_us=%lu surface_updates=%lu\n",
               desk.compose_us, desk.upload_us, desk.drag_frames, desk.drag_us, desk.drag_max_us,
               desk.surface_updates);
        unsigned percentile = 0;
        uint64_t accumulated = 0, target = (desk.drag_intervals * 95 + 99) / 100;
        for (; percentile < 127; percentile++) {
            accumulated += desk.drag_interval_histogram[percentile];
            if (accumulated >= target) {
                break;
            }
        }

        printf("GUI_PACING drag_intervals=%lu mean_us=%lu p95_us=%u max_us=%lu "
               "buffered=%u target_us=8333\n",
               desk.drag_intervals,
               desk.drag_intervals ? desk.drag_interval_us / desk.drag_intervals : 0,
               (percentile + 1) * 1000, desk.drag_interval_max, desk.buffered_display);
    }

cleanup_memory:
    wallpaper_cleanup();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_free_surface(&desk.windows[i]);
    }
    deallocate(desk.scene, bytes);
    deallocate(desk.wallpaper, bytes);
    if (error) {
        printf("gui: %s\n", error_text(error));
        return 1;
    }

    puts("gui: returned to terminal.");
    return 0;
}
