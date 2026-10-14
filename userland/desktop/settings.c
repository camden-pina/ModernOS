#include "desktop.h"

typedef struct {
    int scroll, focus;
    SmoothScroll animation;
} Settings;

static const int scales[] = {100, 125, 150, 200};
static const uint32_t accents[] = {THEME_ACCENT, 0x3C709A, 0x80709F},
                      selections[] = {THEME_SELECTION, 0xE0EBF4, 0xEBE5F3},
                      accent_texts[] = {THEME_SELECTION_TEXT, 0x315E83, 0x665684};

static void home_init(void) {
    copy_text(desk.home, sizeof(desk.home), "/home/user");
    const char *directories[] = {"/home",
                                 "/home/user",
                                 "/home/user/Downloads",
                                 "/home/user/Projects",
                                 "/home/user/Pictures",
                                 "/home/user/Pictures/Wallpapers",
                                 "/home/user/.Trash",
                                 "/home/user/.config"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); i++) {
        int e = call2(SYS_mkdir, directories[i], i >= 6 ? 0700 : 0755);
        if (e && e != -E_EXIST) {
            copy_text(desk.home, sizeof(desk.home), "/");
            break;
        }
    }
}

static void config_path(void) {
    join_path(desk.config, desk.home, ".config/desktop.ini");
    /* Prefer an actual writable disk configuration over volatile RAM. Merely
       choosing it does not write disk data; Save creates its directory. */
    char mounts[8192];
    long n = call3(SYS_sysinfo, INFO_MOUNTS, mounts, sizeof(mounts));
    if (n < 0) {
        return;
    }
    mounts[MIN(n, (long)sizeof(mounts) - 1)] = 0;
    char *p = mounts;
    while (*p) {
        char *path = p;
        while (*p && *p != ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        *p++ = 0;
        char *type = p;
        while (*p && *p != ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        *p++ = 0;
        char *mode = p;
        while (*p && *p != ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        *p++ = 0;
        while (*p && *p != '\n') {
            p++;
        }
        if (*p) {
            p++;
        }
        if (!strcmp(type, "fat32") && !strcmp(mode, "rw")) {
            char dir[OS_PATH_MAX];
            if (!join_path(dir, path, ".modernos") && !join_path(desk.config, dir, "desktop.ini")) {
                return;
            }
        }
    }
}

void config_load(void) {
    home_init();
    config_path();
    desk.scale = 100;
    desk.scale_index = 0;
    desk.workspace_count = 3;
    desk.wallpaper_index = 0;
    desk.wallpaper_mode = WALLPAPER_FILL;
    desk.wallpaper_background = 0;
    desk.wallpaper_path[0] = 0;
    int fd = open(desk.config, O_RDONLY, 0);
    if (fd < 0) {
        settings_apply();
        return;
    }
    char buffer[OS_PATH_MAX + 512];
    long n = 0;
    while (n < (long)sizeof(buffer) - 1) {
        long count = read(fd, buffer + n, sizeof(buffer) - 1 - n);
        if (count == -E_INTR)
            continue;
        if (count < 0) {
            n = count;
            break;
        }
        if (!count)
            break;
        n += count;
    }

    close(fd);
    if (n < 0) {
        settings_apply();
        return;
    }
    buffer[n] = 0;
    char *p = buffer;
    while (*p) {
        char *name = p;
        while (*p && *p != '=' && *p != '\n') {
            p++;
        }
        if (*p != '=') {
            while (*p && *p != '\n') {
                p++;
            }
            if (*p) {
                p++;
            }
            continue;
        }

        *p++ = 0;
        char *value = p;
        while (*p && *p != '\n') {
            p++;
        }
        if (*p) {
            *p++ = 0;
        }
        long v = number(value, 10);
        if (!strcmp(name, "accent") && v >= 0 && v < 3) {
            desk.accent_index = v;
        }
        if (!strcmp(name, "wallpaper") && v >= 0 && v < WALLPAPER_COUNT) {
            desk.wallpaper_index = v;
        }
        if (!strcmp(name, "wallpaper_path")) {
            copy_text(desk.wallpaper_path, sizeof(desk.wallpaper_path), value);
        }
        if (!strcmp(name, "wallpaper_mode") && v >= WALLPAPER_FILL && v <= WALLPAPER_CENTER) {
            desk.wallpaper_mode = v;
        }
        if (!strcmp(name, "wallpaper_background") && v >= 0 && v < 3) {
            desk.wallpaper_background = v;
        }
        if (!strcmp(name, "scale") && v >= 0 && v < 4 && desk.width * 100 / scales[v] >= 530 &&
            desk.height * 100 / scales[v] >= 420) {
            desk.scale_index = v;
            desk.scale = scales[v];
        }
        if (!strcmp(name, "workspaces") && v >= 1 && v <= 3) {
            desk.workspace_count = v;
        }
        if (!strcmp(name, "dark") && (v == 0 || v == 1)) {
            desk.dark_mode = v;
        }
    }

    int error = wallpaper_set(desk.wallpaper_index, desk.wallpaper_path);
    if (error) {
        trace("WALLPAPER_UNAVAILABLE", NULL, desk.wallpaper_path);
        wallpaper_set(0, NULL);
        copy_text(desk.wallpaper_status, sizeof(desk.wallpaper_status),
                  "Saved image unavailable. Using Mineral; choose another image.");
    }

    settings_apply();
}

int config_save(void) {
    char parent[OS_PATH_MAX];
    normalize_path(parent, desk.config, "..");
    int result = call2(SYS_mkdir, parent, 0700);
    if (result && result != -E_EXIST) {
        return result;
    }
    char content[OS_PATH_MAX + 256], num[32];
    copy_text(content, sizeof(content), "accent=");
    format_uint(num, desk.accent_index);
    append_text(content, sizeof(content), num);
    append_text(content, sizeof(content), "\nwallpaper=");
    format_uint(num, desk.wallpaper_index);
    append_text(content, sizeof(content), num);
    append_text(content, sizeof(content), "\nscale=");
    format_uint(num, desk.scale_index);
    append_text(content, sizeof(content), num);
    append_text(content, sizeof(content), "\nworkspaces=");
    format_uint(num, desk.workspace_count);
    append_text(content, sizeof(content), num);
    append_text(content, sizeof(content), "\ndark=");
    append_text(content, sizeof(content), desk.dark_mode ? "1" : "0");
    append_text(content, sizeof(content), "\nwallpaper_mode=");
    format_uint(num, desk.wallpaper_mode);
    append_text(content, sizeof(content), num);
    append_text(content, sizeof(content), "\nwallpaper_background=");
    format_uint(num, desk.wallpaper_background);
    append_text(content, sizeof(content), num);
    append_text(content, sizeof(content), "\nwallpaper_path=");
    append_text(content, sizeof(content), desk.wallpaper_path);
    append_text(content, sizeof(content), "\n");
    /* Complete new contents before replacing the existing configuration. */
    char temporary[OS_PATH_MAX], backup[OS_PATH_MAX];
    if (strlen(desk.config) + 5 >= OS_PATH_MAX) {
        return -E_NAMETOOLONG;
    }
    copy_text(temporary, sizeof(temporary), desk.config);
    append_text(temporary, sizeof(temporary), ".new");
    copy_text(backup, sizeof(backup), desk.config);
    append_text(backup, sizeof(backup), ".bak");
    struct os_stat st;
    bool existed = !stat(desk.config, &st);
    if (existed && !stat(backup, &st)) {
        return -E_EXIST;
    }
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return fd;
    }
    result = write_all(fd, content, strlen(content));
    if (!result) {
        result = fsync(fd);
    }
    close(fd);
    if (result) {
        call1(SYS_unlink, temporary);
        return result;
    }
    if (existed) {
        result = rename_noreplace(desk.config, backup);
    }
    if (!result) {
        result = rename_noreplace(temporary, desk.config);
    }
    if (result) {
        if (existed) {
            rename_noreplace(backup, desk.config);
        }
        call1(SYS_unlink, temporary);
        return result;
    }

    return existed ? call1(SYS_unlink, backup) : 0;
}

void settings_apply(void) {
    desk.accent = accents[desk.accent_index];
    desk.selection = selections[desk.accent_index];
    desk.accent_text = accent_texts[desk.accent_index];
    if (desk.dark_mode) {
        static const uint32_t selected[] = {0x26483E, 0x253F54, 0x3D344D};
        static const uint32_t text[] = {0xA0D7BC, 0xA8CDE8, 0xD1BFEE};
        desk.selection = selected[desk.accent_index];
        desk.accent_text = text[desk.accent_index];
    }

    if (desk.wallpaper) {
        wallpaper_generate();
        invalidate_all();
    }
}

static void scale_apply(int index) {
    int old = desk.scale;
    desk.scale_index = index;
    desk.scale = scales[index];
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &desk.windows[i];
        if (!w->alive) {
            continue;
        }
        int center_x = w->box.x + w->box.w / 2, center_y = w->box.y + w->box.h / 2;
        w->box.w = clampi(w->box.w * desk.scale / old, S(530), desk.width);
        w->box.h = clampi(w->box.h * desk.scale / old, S(340), desk.height - S(42));
        w->box.x = clampi(center_x - w->box.w / 2, 0, desk.width - w->box.w);
        w->box.y = clampi(center_y - w->box.h / 2, 0, desk.height - S(42) - w->box.h);
        w->restore.w = clampi(w->restore.w * desk.scale / old, S(530), desk.width);
        w->restore.h = clampi(w->restore.h * desk.scale / old, S(340), desk.height - S(42));
        w->restore.x = clampi(w->restore.x, 0, desk.width - w->restore.w);
        w->restore.y = clampi(w->restore.y, 0, desk.height - S(42) - w->restore.h);
        if (w->maximized) {
            w->box = (Rect){0, 0, desk.width, desk.height - S(42)};
        }
    }

    menu_close();
    desk.launcher = false;
    desk.drag = NULL;
    invalidate_all();
    trace("SCALE", NULL, "");
}

int settings_open(Window *w, const char *path) {
    (void)path;
    Settings *s = allocate(sizeof(*s));
    if (!s) {
        return -E_NOMEM;
    }
    memset(s, 0, sizeof(*s));
    w->state = s;
    wallpaper_prepare_previews();
    return 0;
}

/* Stable control IDs keep general preferences and wallpaper controls distinct. */
enum { SETTINGS_ROWS = 7, SETTINGS_HEIGHT = 1012 };
static const int row_counts[] = {3, WALLPAPER_COUNT, 4, 3, 2, 4, 3};
static const int row_y[] = {93, 181, 665, 749, 833, 431, 509};
static const int heading_y[] = {67, 155, 639, 723, 807, 405, 483};
static const int focus_order[] = {0,  1,  2,  4, 5, 6,  21, 22, 23, 24, 25, 26, 27, 28, 29,
                                  30, 31, 32, 8, 9, 10, 11, 12, 13, 14, 16, 17, 20, 33};

static int choice_focus(int row, int col) {
    if (row == 1 && col >= 3)
        return 21 + col - 3;
    if (row == 5)
        return 24 + col;
    if (row == 6)
        return 28 + col;
    return row * 4 + col;
}

static bool choice_enabled(int row, int col) {
    if (row == 5 || row == 6)
        return desk.wallpaper_index >= WALLPAPER_ALPINE;
    return row != 2 ||
           (desk.width * 100 / scales[col] >= 530 && desk.height * 100 / scales[col] >= 420);
}

static Rect choice(Window *w, int row, int col) {
    Settings *s = w->state;
    Rect b = window_body(w);
    int gap = S(9), width = (b.w - S(52) - gap * 3) / 4;
    int y = row_y[row], height = 31;
    if (row == 1) {
        width = (b.w - S(52) - gap * 2) / 3;
        y += (col / 3) * 94;
        height = 87;
        col %= 3;
    }

    return (Rect){b.x + S(26) + col * (width + gap), b.y + S(y) - s->scroll, width, S(height)};
}

static Rect extra_control(Window *w, int focus) {
    Settings *s = w->state;
    Rect b = window_body(w);
    int width = (b.w - S(52) - S(9)) / 2;

    if (focus == 31 || focus == 32)
        return (Rect){b.x + S(26) + (focus - 31) * (width + S(9)), b.y + S(561) - s->scroll, width,
                      S(31)};

    return (Rect){b.x + S(26) + (focus == 33 ? S(176) : 0), b.y + S(956) - s->scroll,
                  S(focus == 33 ? 155 : 166), S(31)};
}

static Rect focus_rect(Window *w, int focus) {
    for (int row = 0; row < SETTINGS_ROWS; row++)
        for (int col = 0; col < row_counts[row]; col++)
            if (choice_focus(row, col) == focus)
                return choice(w, row, col);
    return extra_control(w, focus);
}

static bool focus_enabled(int focus) {
    for (int row = 0; row < SETTINGS_ROWS; row++)
        for (int col = 0; col < row_counts[row]; col++)
            if (choice_focus(row, col) == focus)
                return choice_enabled(row, col);
    return true;
}

Rect settings_hover(Window *w, int x, int y) {
    Rect b = window_body(w);

    if (!contains(b, x, y))
        return (Rect){0};
    for (unsigned i = 0; i < sizeof(focus_order) / sizeof(*focus_order); i++) {
        int focus = focus_order[i];
        Rect r = focus_rect(w, focus);
        if (focus_enabled(focus) && contains(r, x, y))
            return intersect(r, b);
    }

    return (Rect){0};
}

void settings_draw(Window *w) {
    Settings *s = w->state;
    Rect b = window_body(w), saved = desk.clip;
    clip_set(intersect(saved, b));
    fill(b, THEME_WINDOW);
    int y = b.y - s->scroll;
    label(b.x + S(26), y + S(22), "Desktop preferences", FONT_TITLE, THEME_PRIMARY_TEXT,
          b.w - S(52));
    const char *headings[] = {"Accent",     "Wallpaper",       "Interface scale", "Workspaces",
                              "Appearance", "Image placement", "Background color"};
    const char *accent_names[] = {"Jade", "Blue", "Slate"},
               *scale_names[] = {"100%", "125%", "150%", "200%"},
               *workspace_names[] = {"1 workspace", "2 workspaces", "3 workspaces"},
               *appearance_names[] = {"Light", "Dark"},
               *mode_names[] = {"Fill", "Fit", "Stretch", "Center"},
               *background_names[] = {"Slate", "Black", "Cream"};
    for (int row = 0; row < SETTINGS_ROWS; row++) {
        label(b.x + S(26), y + S(heading_y[row]), headings[row], FONT_BOLD, theme_color(0x41525A),
              b.w - S(52));
        for (int col = 0; col < row_counts[row]; col++) {
            Rect r = choice(w, row, col);
            bool selected = col == (row == 0   ? desk.accent_index
                                    : row == 1 ? desk.wallpaper_index
                                    : row == 2 ? desk.scale_index
                                    : row == 3 ? desk.workspace_count - 1
                                    : row == 4 ? desk.dark_mode
                                    : row == 5 ? desk.wallpaper_mode
                                               : desk.wallpaper_background);
            bool enabled = choice_enabled(row, col);
            bool hover = enabled && contains(r, desk.mouse_x, desk.mouse_y);
            rounded(r, S(5),
                    selected ? desk.selection
                    : hover  ? theme_color(0xEDF1EF)
                             : THEME_TOOLBAR);
            border(r, S(5), selected ? desk.accent : theme_color(0xE5E9E8));
            if (s->focus == choice_focus(row, col))
                border((Rect){r.x - 2, r.y - 2, r.w + 4, r.h + 4}, S(6), theme_color(0x96B5A9));
            int x = r.x + S(10);
            if (row == 1) {
                Rect preview = {r.x + S(5), r.y + S(5), r.w - S(10), r.h - S(31)};
                fill(preview, wallpaper_backgrounds[0]);
                wallpaper_draw_preview(col, preview);
                if (col == WALLPAPER_CUSTOM && !desk.wallpaper_path[0])
                    icon(preview.x + (preview.w - S(22)) / 2, preview.y + (preview.h - S(22)) / 2,
                         I_PLUS, S(22), 0xB9CCCD);
                if (selected) {
                    rounded((Rect){preview.x + preview.w - S(23), preview.y + S(3), S(20), S(20)},
                            S(4), desk.accent);
                    icon(preview.x + preview.w - S(20), preview.y + S(6), I_CHECK, S(14), 0xFFFFFF);
                }
                label(x, r.y + r.h - S(20), wallpaper_names[col], FONT_NORMAL,
                      selected ? desk.accent_text : THEME_SECONDARY_TEXT, r.w - S(20));
                continue;
            }

            if (row == 0 || row == 6) {
                rounded((Rect){x, r.y + S(9), S(13), S(13)}, S(3),
                        row == 0 ? accents[col] : wallpaper_backgrounds[col]);
                x += S(20);
            }
            label(x, r.y + S(8),
                  row == 0   ? accent_names[col]
                  : row == 2 ? scale_names[col]
                  : row == 3 ? workspace_names[col]
                  : row == 4 ? appearance_names[col]
                  : row == 5 ? mode_names[col]
                             : background_names[col],
                  FONT_NORMAL, enabled ? theme_color(0x617176) : theme_color(0xAFB8B9),
                  r.w - (x - r.x) - S(7));
        }
    }

    char selected[OS_PATH_MAX + 32];
    copy_text(selected, sizeof(selected), "Current: ");
    append_text(selected, sizeof(selected),
                desk.wallpaper_index == WALLPAPER_CUSTOM ? basename(desk.wallpaper_path)
                                                         : wallpaper_names[desk.wallpaper_index]);
    label(b.x + S(26), y + S(374), desk.wallpaper_status[0] ? desk.wallpaper_status : selected,
          FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(52));
    label(b.x + S(26), y + S(470),
          desk.wallpaper_index < WALLPAPER_ALPINE ? "Placement applies to image wallpapers."
          : desk.wallpaper_mode == WALLPAPER_FILL ? "Fill covers the desktop and crops the edges."
          : desk.wallpaper_mode == WALLPAPER_FIT
              ? "Fit shows the whole image with borders if needed."
          : desk.wallpaper_mode == WALLPAPER_STRETCH
              ? "Stretch resizes the image to cover the desktop."
              : "Center shows the image at its original size.",
          FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(52));
    button(extra_control(w, 31), "Built-in wallpapers", I_FOLDER, false, true, s->focus == 31);
    button(extra_control(w, 32), "My wallpapers", I_FOLDER, false, true, s->focus == 32);
    label(b.x + S(26), y + S(604), "Choose any PNG, JPEG or uncompressed BMP image.", FONT_NORMAL,
          THEME_SECONDARY_TEXT, b.w - S(52));
    label(b.x + S(26), y + S(882),
          "Preferences apply immediately. Save records the current values.", FONT_NORMAL,
          THEME_SECONDARY_TEXT, b.w - S(52));
    struct os_stat st;
    char parent[OS_PATH_MAX];
    normalize_path(parent, desk.config, "..");
    int result = stat(parent, &st);
    while (result && strcmp(parent, "/")) {
        char next[OS_PATH_MAX];
        normalize_path(next, parent, "..");
        copy_text(parent, sizeof(parent), next);
        result = stat(parent, &st);
    }

    bool disk = !result && st.dev;
    label(b.x + S(26), y + S(904),
          disk ? "Storage: writable disk; saved values survive reboot."
               : "Storage: RAM; saved values last only for this boot.",
          FONT_NORMAL, THEME_SECONDARY_TEXT, b.w - S(52));
    label(b.x + S(26), y + S(926), desk.config, FONT_SMALL, theme_color(0x9DA8AB), b.w - S(52));
    button(extra_control(w, 20), "Save preferences", I_SAVE, true, true, s->focus == 20);
    button(extra_control(w, 33), "Reset wallpaper", I_RELOAD, false, true, s->focus == 33);
    scrollbar((Rect){b.x, b.y, b.w, b.h}, S(SETTINGS_HEIGHT), s->scroll);
    desk.clip = saved;
}

static void wallpaper_error(Window *w, int error) {
    if (error == -E_INVAL)
        dialog_notice(w, "Cannot use this wallpaper",
                      "Choose a valid PNG, JPEG or uncompressed 24/32-bit BMP. Images may be up to "
                      "8192 pixels per edge, 16 megapixels and 64 MiB on disk.");
    else
        dialog_error(w, "Choose wallpaper", error);
    trace("WALLPAPER_ERROR", w, "");
}

static void choose_image(Window *w, const char *value) {
    char path[OS_PATH_MAX];
    int error = normalize_path(path, desk.home, value);
    if (!error)
        error = wallpaper_set(WALLPAPER_CUSTOM, path);
    if (error) {
        wallpaper_error(w, error);
        return;
    }
    settings_apply();
    trace("WALLPAPER", w, desk.wallpaper_path);
}

static void choose_setting(Window *w, int row, int col) {
    Settings *s = w->state;
    s->focus = choice_focus(row, col);
    if (!choice_enabled(row, col))
        return;
    if (row == 0) {
        desk.accent_index = col;
        settings_apply();
        trace("ACCENT", w, "");
    } else if (row == 1) {
        if (col == WALLPAPER_CUSTOM) {
            char path[OS_PATH_MAX];
            if (desk.wallpaper_path[0])
                copy_text(path, sizeof(path), desk.wallpaper_path);
            else
                join_path(path, desk.home, "Pictures/Wallpapers/");
            dialog_input(w, "Choose wallpaper", "Enter the path to a PNG, JPEG or BMP image.", path,
                         choose_image);
            return;
        }

        int error = wallpaper_set(col, NULL);
        if (error) {
            wallpaper_error(w, error);
            return;
        }
        settings_apply();
        trace("WALLPAPER", w, wallpaper_names[col]);
    } else if (row == 2) {
        scale_apply(col);
    } else if (row == 4) {
        desk.dark_mode = col != 0;
        settings_apply();
        trace("APPEARANCE", w, desk.dark_mode ? "dark" : "light");
    } else if (row == 5) {
        desk.wallpaper_mode = col;
        settings_apply();
        trace("WALLPAPER_MODE", w, "");
    } else if (row == 6) {
        desk.wallpaper_background = col;
        settings_apply();
        trace("WALLPAPER_BACKGROUND", w, "");
    } else {
        int count = col + 1;
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (desk.windows[i].alive && desk.windows[i].workspace >= count)
                desk.windows[i].workspace = count - 1;
        desk.workspace_count = count;
        if (desk.workspace >= count) {
            desk.workspace = -1;
            workspace_switch(count - 1);
        }
        invalidate_all();
        trace("WORKSPACE_COUNT", w, "");
    }
}

static void save_preferences(Window *w) {
    int e = config_save();
    if (e)
        dialog_error(w, "Save preferences", e);
    else {
        struct os_stat st;
        bool disk = !stat(desk.config, &st) && st.dev;
        dialog_notice(w, "Preferences saved",
                      disk ? "Saved to writable disk. These preferences will be loaded on the next "
                             "desktop launch and after reboot."
                           : "Saved in the RAM filesystem. These preferences will be loaded on the "
                             "next desktop launch during this boot; reboot clears RAM storage.");
        trace("CONFIG_SAVED", w, desk.config);
    }
}

static void activate(Window *w, int focus) {
    if (focus == 20) {
        save_preferences(w);
        return;
    }
    if (focus == 33) {
        wallpaper_set(0, NULL);
        desk.wallpaper_mode = WALLPAPER_FILL;
        desk.wallpaper_background = 0;
        settings_apply();
        trace("WALLPAPER", w, "Mineral");
        return;
    }

    if (focus == 31 || focus == 32) {
        char path[OS_PATH_MAX];
        if (focus == 31)
            copy_text(path, sizeof(path), "/usr/share/wallpapers");
        else
            join_path(path, desk.home, "Pictures/Wallpapers");
        wm_open(APP_FILES, path);
        return;
    }

    for (int row = 0; row < SETTINGS_ROWS; row++)
        for (int col = 0; col < row_counts[row]; col++)
            if (choice_focus(row, col) == focus) {
                choose_setting(w, row, col);
                return;
            }
}

bool settings_mouse(Window *w, int x, int y, unsigned pressed, int wheel) {
    Settings *s = w->state;
    Rect b = window_body(w);
    if (wheel) {
        smooth_by(&s->animation, s->scroll, -(int64_t)wheel * S(40),
                  MAX(0, S(SETTINGS_HEIGHT) - b.h), now_ms());
        return true;
    }
    if (!(pressed & 1) || !contains(b, x, y))
        return false;
    for (unsigned i = 0; i < sizeof(focus_order) / sizeof(*focus_order); i++) {
        int focus = focus_order[i];
        if (focus_enabled(focus) && contains(focus_rect(w, focus), x, y)) {
            s->focus = focus;
            activate(w, focus);
            invalidate_window(w);
            return true;
        }
    }

    return false;
}

void settings_key(Window *w, Key k) {
    if (!k.down)
        return;
    Settings *s = w->state;
    if ((k.mods & MOD_CTRL) && (k.ascii == 's' || k.ascii == 'S')) {
        save_preferences(w);
        return;
    }
    if (k.code == KEY_PAGEUP || k.code == KEY_PAGEDOWN) {
        settings_mouse(w, 0, 0, 0, k.code == KEY_PAGEUP ? 3 : -3);
        return;
    }
    if (k.code == KEY_TAB || k.code == KEY_RIGHT || k.code == KEY_DOWN || k.code == KEY_LEFT ||
        k.code == KEY_UP) {
        int delta =
            k.code == KEY_LEFT || k.code == KEY_UP || (k.code == KEY_TAB && (k.mods & MOD_SHIFT))
                ? -1
                : 1;
        int count = sizeof(focus_order) / sizeof(*focus_order), at = 0;
        while (at < count && focus_order[at] != s->focus)
            at++;
        do {
            at = (at + delta + count) % count;
        } while (!focus_enabled(focus_order[at]));
        s->focus = focus_order[at];
        Rect r = focus_rect(w, s->focus), b = window_body(w);
        if (r.y - S(3) < b.y)
            s->scroll -= b.y - r.y + S(3);
        if (r.y + r.h + S(3) > b.y + b.h)
            s->scroll += r.y + r.h + S(3) - b.y - b.h;
        s->scroll = clampi(s->scroll, 0, MAX(0, S(SETTINGS_HEIGHT) - b.h));
        smooth_jump(&s->animation, s->scroll, now_ms());
        invalidate_window(w);
    } else if (k.code == KEY_ENTER || k.code == KEY_SPACE) {
        activate(w, s->focus);
        invalidate_window(w);
    }
}

void settings_tick(Window *w) {
    Settings *s = w->state;
    Rect body = window_body(w);
    int position =
        smooth_tick(&s->animation, s->scroll, MAX(0, S(SETTINGS_HEIGHT) - body.h), now_ms());
    if (position != s->scroll) {
        s->scroll = position;
        invalidate(body);
    }
}

bool settings_close(Window *w) {
    deallocate(w->state, sizeof(Settings));
    w->state = NULL;
    return true;
}
