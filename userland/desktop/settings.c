#include "desktop.h"

typedef struct {
  int scroll, focus, page, profile_selection;
  SmoothScroll animation;
  uint64_t save_at;
  uint64_t mode_until;
  unsigned mode_previous_width, mode_previous_height;
  unsigned mode_seconds;
  unsigned account_op, account_target;
  char new_password[128];
} Settings;

enum { SETTINGS_ROWS = 16 };
static void choose_setting(Window *,int,int);
static const int scales[] = {100, 125, 150, 200};
static const struct {
  unsigned width, height;
  const char *label;
} display_modes[] = {
    {2560, 1600, "2560 x 1600"}, {1920, 1200, "1920 x 1200"},
    {1600, 1000, "1600 x 1000"}, {1280, 800, "1280 x 800"},
    {1024, 768, "1024 x 768"}};
static const uint32_t accents[]={THEME_ACCENT,0x3C709A,0x80709F,0x9B5CC4,0xC25D8C,0xCB655D,0xBF833D,0x438D72};
static const uint32_t selections[]={THEME_SELECTION,0xE0EBF4,0xEBE5F3,0xEEE3F7,0xF5E1EC,0xF7E3DF,0xF4E9D9,0xDEEFE6};
static const uint32_t accent_texts[]={THEME_SELECTION_TEXT,0x315E83,0x665684,0x70418D,0x954368,0x9B4842,0x8B602E,0x316B55};

static void home_init(void) {
  copy_text(desk.home, sizeof(desk.home), "/home/user");
  const char *directories[] = {"/home",
                               "/home/user",
                               "/home/user/Downloads",
                               "/home/user/Projects",
                               "/home/user/Documents",
                               "/home/user/Desktop",
                               "/home/user/Pictures",
                               "/home/user/Pictures/Wallpapers",
                               "/home/user/.Trash",
                               "/home/user/.config"};
  for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); i++) {
    int e = call2(SYS_mkdir, directories[i], basename(directories[i])[0]=='.' ? 0700 : 0755);
    if (e && e != -E_EXIST) {
      copy_text(desk.home, sizeof(desk.home), "/");
      break;
    }
    if (i) {
      int fd=open(directories[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0);
      struct os_stat st;
      e=fd<0 ? fd : fstat(fd,&st);
      if (!e && st.uid!=0 && st.uid!=1000) e=-E_PERM;
      if (!e) e=call3(SYS_fchown,fd,1000,1000);
      if (fd>=0) close(fd);
      if (e) { copy_text(desk.home,sizeof(desk.home),"/"); break; }
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
      if (!join_path(dir, path, ".coast") &&
          !join_path(desk.config, dir, "desktop.ini")) {
        return;
      }
    }
  }
}

static void config_storage(void) {
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

  desk.config_on_disk = !result && st.dev;
}

static void config_load_root(void) {
  home_init();
  config_path();
  char legacy_config[OS_PATH_MAX];
  copy_text(legacy_config, sizeof(legacy_config), desk.config);
  profiles_load(legacy_config);
  profiles_home_init();
  config_storage();
  pointer_reset_defaults();
  desk.profile_avatar = 0;
  desk.profile_tagline[0] = 0;
  desk.terminal_palette = desk.terminal_foreground = desk.terminal_background =
      0;
  desk.terminal_opacity = 100;
  desk.terminal_text_size = 11;
  desk.terminal_shell = 0;
  desk.terminal_ansi = true;
  desk.ui_motion=1;desk.glass_style=1;desk.launcher_rows=7;desk.desktop_icon_size=40;
  desk.desktop_icons=true;desk.snap_assist_enabled=desk.snap_hover_enabled=desk.snap_bar_enabled=true;
  desk.files_default_details=desk.files_default_hidden=false;
  desk.dock_size = 30;
  desk.dock_magnification = 16;
  desk.dock_pins = (1u << APP_FILES) | (1u << APP_CHROMIUM) |
                   (1u << APP_TERMINAL) | (1u << APP_DISKS) |
                   (1u << APP_SETTINGS) | (1u << APP_EDITOR);
  desk.editor_tab_width = 4;
  desk.editor_text_size = 13;
  desk.editor_newline = 0;
  desk.editor_line_numbers = true;
  desk.editor_spaces = false;
  desk.editor_autoindent = true;
  desk.editor_whitespace = false;
  desk.editor_highlight = true;
  desk.scale = 100;
  desk.scale_index = 0;
  desk.preferred_width = desk.width;
  desk.preferred_height = desk.height;
  desk.workspace_count = 3;
  desk.wallpaper_index = 0;
  desk.wallpaper_mode = WALLPAPER_FILL;
  desk.wallpaper_background = 0;
  desk.wallpaper_path[0] = 0;
  int fd = open(desk.config, O_RDONLY, 0);
  if (fd == -E_NOENT && profiles_legacy()[0])
    fd = open(profiles_legacy(), O_RDONLY, 0);
  /* Read image defaults only when no user preferences exist. Keep the writable
     user destination separate from the immutable boot image. */
  if (fd == -E_NOENT)
    fd = open("/usr/share/coast/desktop-defaults.ini", O_RDONLY, 0);
  if (fd < 0) {
    settings_apply();
    return;
  }
  char buffer[OS_PATH_MAX + 2048];
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
    bool numeric = value[0] != 0;
    for (const char *digit = value; *digit; digit++)
      numeric &= *digit >= '0' && *digit <= '9';
    if (!strcmp(name, "profile_tagline")) {
      bool valid = strlen(value) <= 80;
      for (const char *c = value; *c; c++)
        valid &= *c >= 32 && *c <= 126;
      if (valid)
        copy_text(desk.profile_tagline, sizeof(desk.profile_tagline), value);
    }
    if (numeric) {
      if (!strcmp(name, "display_width") && v >= 640 && v <= desk.display_max_width)
        desk.preferred_width = v;
      if (!strcmp(name, "display_height") && v >= 480 && v <= desk.display_max_height)
        desk.preferred_height = v;
      if (!strcmp(name, "profile_avatar") && v < 6)
        desk.profile_avatar = v;
      if (!strcmp(name, "terminal_palette") && v < 5)
        desk.terminal_palette = v;
      if (!strcmp(name, "terminal_foreground") && v < 7)
        desk.terminal_foreground = v;
      if (!strcmp(name, "terminal_background") && v < 7)
        desk.terminal_background = v;
      if (!strcmp(name, "terminal_opacity") && v >= 40 && v <= 100)
        desk.terminal_opacity = v;
      if (!strcmp(name, "terminal_text_size") && v >= 9 && v <= 19 && v % 2)
        desk.terminal_text_size = v;
      if (!strcmp(name, "terminal_shell") && v <= 1)
        desk.terminal_shell = v;
      if (!strcmp(name, "terminal_ansi") && v <= 1)
        desk.terminal_ansi = v;
      if (!strcmp(name, "dock_size") && (v >= 16 && v <= 112))
        desk.dock_size = v;
      if (!strcmp(name, "dock_magnification") && (v >= 0 && v <= 100))
        desk.dock_magnification = v;
      if (!strcmp(name, "dock_pins") && v >= 0 && v < (1 << APP_COUNT))
        desk.dock_pins = v;
      if (!strcmp(name, "editor_tab_width") && (v == 2 || v == 4 || v == 8))
        desk.editor_tab_width = v;
      if (!strcmp(name, "editor_text_size") && (v == 11 || v == 13))
        desk.editor_text_size = v;
      if (!strcmp(name, "editor_newline") && v >= 0 && v <= 2)
        desk.editor_newline = v;
      if (!strcmp(name, "editor_line_numbers") && v >= 0 && v <= 1)
        desk.editor_line_numbers = v;
      if (!strcmp(name, "editor_spaces") && v >= 0 && v <= 1)
        desk.editor_spaces = v;
      if (!strcmp(name, "editor_autoindent") && v >= 0 && v <= 1)
        desk.editor_autoindent = v;
      if (!strcmp(name, "editor_whitespace") && v >= 0 && v <= 1)
        desk.editor_whitespace = v;
      if (!strcmp(name, "editor_highlight") && v >= 0 && v <= 1)
        desk.editor_highlight = v;
      if (!strcmp(name, "pointer_speed") && (v >= 25 && v <= 300))
        desk.pointer_speed = v;
      if (!strcmp(name, "touchpad_speed") && (v >= 25 && v <= 300))
        desk.touchpad_speed = v;
      if (!strcmp(name, "wheel_steps") && v >= 1 && v <= 10)
        desk.wheel_steps = v;
      if (!strcmp(name, "pointer_acceleration") && v <= 1)
        desk.pointer_acceleration = v;
      if (!strcmp(name, "primary_right") && v <= 1)
        desk.primary_right = v;
      if (!strcmp(name, "natural_scroll") && v <= 1)
        desk.natural_scroll = v;
      if (!strcmp(name, "touchpad_natural") && v <= 1)
        desk.touchpad_natural = v;
    }
    if(numeric && v>=0) {
    if(!strcmp(name,"ui_motion") && v<=2)desk.ui_motion=v;
    if(!strcmp(name,"glass_style") && v<=3)desk.glass_style=v;
    if(!strcmp(name,"launcher_rows") && (v==5 || v==7 || v==9))desk.launcher_rows=v;
    if(!strcmp(name,"desktop_icon_size") && (v==32 || v==40 || v==48))desk.desktop_icon_size=v;
    if(v<=1) {
      if(!strcmp(name,"desktop_icons"))desk.desktop_icons=v;
      if(!strcmp(name,"snap_assist_enabled"))desk.snap_assist_enabled=v;
      if(!strcmp(name,"snap_hover_enabled"))desk.snap_hover_enabled=v;
      if(!strcmp(name,"snap_bar_enabled"))desk.snap_bar_enabled=v;
      if(!strcmp(name,"files_default_details"))desk.files_default_details=v;
      if(!strcmp(name,"files_default_hidden"))desk.files_default_hidden=v;
    }
    }
    if (!strcmp(name, "accent") && v >= 0 && v < 8) {
      desk.accent_index = v;
    }
    if (!strcmp(name, "wallpaper") && v >= 0 && v < WALLPAPER_COUNT) {
      desk.wallpaper_index = v;
    }
    if (!strcmp(name, "wallpaper_path")) {
      copy_text(desk.wallpaper_path, sizeof(desk.wallpaper_path), value);
    }
    if (!strcmp(name, "wallpaper_mode") && v >= WALLPAPER_FILL &&
        v <= WALLPAPER_CENTER) {
      desk.wallpaper_mode = v;
    }
    if (!strcmp(name, "wallpaper_background") && v >= 0 && v < 3) {
      desk.wallpaper_background = v;
    }
    if (!strcmp(name, "scale") && v >= 0 && v < 4) {
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

  profiles_avatar_changed();
  terminal_preferences_changed();
  pointer_preferences_changed();
  desktop_file_scope(profiles_uid(profiles_active()));
  int error = wallpaper_set(desk.wallpaper_index, desk.wallpaper_path);
  if (error) {
    trace("WALLPAPER_UNAVAILABLE", NULL, desk.wallpaper_path);
    wallpaper_set(0, NULL);
    copy_text(desk.wallpaper_status, sizeof(desk.wallpaper_status),
              "Saved image unavailable. Using Coast; choose another image.");
  }

  settings_apply();
}

static int config_save_root(void) {
  int result = profiles_prepare();
  if (result) {
    return result;
  }
  char content[OS_PATH_MAX + 2048], num[32];
  copy_text(content, sizeof(content), "accent=");
  format_uint(num, desk.accent_index);
  append_text(content, sizeof(content), num);
  append_text(content, sizeof(content), "\nwallpaper=");
  format_uint(num, desk.wallpaper_index);
  append_text(content, sizeof(content), num);
  append_text(content, sizeof(content), "\nscale=");
  format_uint(num, desk.scale_index);
  append_text(content, sizeof(content), num);
  append_text(content, sizeof(content), "\ndisplay_width=");
  format_uint(num, desk.preferred_width);
  append_text(content, sizeof(content), num);
  append_text(content, sizeof(content), "\ndisplay_height=");
  format_uint(num, desk.preferred_height);
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
  const char *pointer_names[] = {"pointer_speed",   "touchpad_speed",
                                 "wheel_steps",     "pointer_acceleration",
                                 "primary_right",   "natural_scroll",
                                 "touchpad_natural"};
  const unsigned pointer_values[] = {
      desk.pointer_speed,        desk.touchpad_speed, desk.wheel_steps,
      desk.pointer_acceleration, desk.primary_right,  desk.natural_scroll,
      desk.touchpad_natural};
  for (unsigned i = 0; i < sizeof(pointer_values) / sizeof(*pointer_values);
       i++) {
    append_text(content, sizeof(content), "\n");
    append_text(content, sizeof(content), pointer_names[i]);
    append_text(content, sizeof(content), "=");
    format_uint(num, pointer_values[i]);
    append_text(content, sizeof(content), num);
  }
  const char *extra_names[] = {
      "profile_avatar",      "terminal_palette",  "terminal_foreground",
      "terminal_background", "terminal_opacity",  "terminal_text_size",
      "terminal_shell",      "terminal_ansi",     "dock_size",
      "dock_magnification",  "dock_pins",         "editor_tab_width",
      "editor_text_size",    "editor_newline",    "editor_line_numbers",
      "editor_spaces",       "editor_autoindent", "editor_whitespace",
      "editor_highlight", "ui_motion", "glass_style", "launcher_rows", "desktop_icon_size",
      "desktop_icons", "snap_assist_enabled", "snap_hover_enabled", "snap_bar_enabled",
      "files_default_details", "files_default_hidden"};
  const unsigned extra_values[] = {
      desk.profile_avatar,      desk.terminal_palette,
      desk.terminal_foreground, desk.terminal_background,
      desk.terminal_opacity,    desk.terminal_text_size,
      desk.terminal_shell,      desk.terminal_ansi,
      desk.dock_size,           desk.dock_magnification,
      desk.dock_pins,           desk.editor_tab_width,
      desk.editor_text_size,    desk.editor_newline,
      desk.editor_line_numbers, desk.editor_spaces,
      desk.editor_autoindent,   desk.editor_whitespace,
      desk.editor_highlight,desk.ui_motion,desk.glass_style,desk.launcher_rows,desk.desktop_icon_size,
      desk.desktop_icons,desk.snap_assist_enabled,desk.snap_hover_enabled,desk.snap_bar_enabled,
      desk.files_default_details,desk.files_default_hidden};
  for (unsigned i = 0; i < sizeof(extra_values) / sizeof(*extra_values); i++) {
    append_text(content, sizeof(content), "\n");
    append_text(content, sizeof(content), extra_names[i]);
    append_text(content, sizeof(content), "=");
    format_uint(num, extra_values[i]);
    append_text(content, sizeof(content), num);
  }
  append_text(content, sizeof(content), "\nprofile_tagline=");
  append_text(content, sizeof(content), desk.profile_tagline);
  append_text(content, sizeof(content), "\nwallpaper_path=");
  append_text(content, sizeof(content), desk.wallpaper_path);
  append_text(content, sizeof(content), "\n");
  return preferences_write(desk.config, content);
}

static int preferences_write_root(const char *path, const char *content) {
  int result = 0;
  /* Complete new contents before replacing the existing configuration. */
  char temporary[OS_PATH_MAX], backup[OS_PATH_MAX];
  if (strlen(path) + 5 >= OS_PATH_MAX) {
    return -E_NAMETOOLONG;
  }
  copy_text(temporary, sizeof(temporary), path);
  append_text(temporary, sizeof(temporary), ".new");
  copy_text(backup, sizeof(backup), path);
  append_text(backup, sizeof(backup), ".bak");
  struct os_stat st;
  bool existed = !stat(path, &st);
  if (existed && (st.mode & 0170000) != S_IFREG)
    return -E_INVAL;
  if (existed && !stat(backup, &st)) {
    return -E_EXIST;
  }
  int fd = desktop_create_file(temporary, 0600);
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
    result = rename_noreplace(path, backup);
  }
  if (!result) {
    result = rename_noreplace(temporary, path);
  }
  if (result) {
    if (existed) {
      rename_noreplace(backup, path);
    }
    call1(SYS_unlink, temporary);
    return result;
  }

  return existed ? call1(SYS_unlink, backup) : 0;
}

void settings_apply(void) {
  if (desk.dock_pins & (1u << APP_RETIRED_BROWSER)) {
    desk.dock_pins &= ~(1u << APP_RETIRED_BROWSER);
    desk.dock_pins |= 1u << APP_CHROMIUM;
  }
  desk.accent = accents[desk.accent_index];
  desk.selection = selections[desk.accent_index];
  desk.accent_text = accent_texts[desk.accent_index];
  if (desk.dark_mode) {
    static const uint32_t selected[] = {0x26483E,0x253F54,0x3D344D,0x443050,0x4C2C3D,0x4F302D,0x4D3C29,0x294438};
    static const uint32_t text[] = {0xA0D7BC,0xA8CDE8,0xD1BFEE,0xD6B3F1,0xF2B3D4,0xF3BCB5,0xEBC995,0xA1D9BD};
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
    w->box.w = clampi(w->box.w * desk.scale / old, MIN(S(480), desk.width),
                      desk.width);
    w->box.h =
        clampi(w->box.h * desk.scale / old,
               MIN(S(340), desk.height - (S(DESKTOP_TOP) + desktop_bottom())),
               desk.height - (S(DESKTOP_TOP) + desktop_bottom()));
    w->box.x = clampi(center_x - w->box.w / 2, 0, desk.width - w->box.w);
    w->box.y = clampi(center_y - w->box.h / 2, S(DESKTOP_TOP),
                      desk.height - desktop_bottom() - w->box.h);
    w->restore.w = clampi(w->restore.w * desk.scale / old,
                          MIN(S(480), desk.width), desk.width);
    w->restore.h =
        clampi(w->restore.h * desk.scale / old,
               MIN(S(340), desk.height - (S(DESKTOP_TOP) + desktop_bottom())),
               desk.height - (S(DESKTOP_TOP) + desktop_bottom()));
    w->restore.x = clampi(w->restore.x, 0, desk.width - w->restore.w);
    w->restore.y = clampi(w->restore.y, S(DESKTOP_TOP),
                          desk.height - desktop_bottom() - w->restore.h);
    if (w->maximized) {
      w->box = (Rect){0, S(DESKTOP_TOP), desk.width,
                      desk.height - (S(DESKTOP_TOP) + desktop_bottom())};
    }
    if (w->app == APP_SETTINGS && w->state) {
      Settings *settings = w->state;
      settings->scroll = clampi((int64_t)settings->scroll * desk.scale / old, 0,
                                MAX(0, S(620) - window_body(w).h));
      smooth_jump(&settings->animation, settings->scroll, now_ms());
    }
    wm_trace_geometry(w);
  }

  snap_cancel();
  wm_workarea_changed();
  menu_close();
  desk.launcher = false;
  desk.drag = NULL;
  invalidate_all();
  trace("SCALE", NULL, "");
}

void settings_reload_profile(void) {
  int old_scale = desk.scale, old_index = desk.scale_index;
  config_load();
  if (desk.display_modeset)
    desktop_set_resolution(desk.preferred_width, desk.preferred_height);
  int target = desk.scale_index;
  desk.scale = old_scale;
  desk.scale_index = old_index;
  if (target != old_index)
    scale_apply(target);
  for (int i = 0; i < MAX_WINDOWS; i++)
    if (desk.windows[i].alive &&
        desk.windows[i].workspace >= desk.workspace_count)
      desk.windows[i].workspace = desk.workspace_count - 1;
  desk.workspace = MIN(desk.workspace, desk.workspace_count - 1);
  desk.recent_count = 0;
  taskbar_preferences_changed();
  invalidate_all();
}

int settings_open(Window *w, const char *path) {
  (void)path;
  Settings *s = allocate(sizeof(*s));
  if (!s) {
    return -E_NOMEM;
  }
  memset(s, 0, sizeof(*s));
  w->state = s;
  s->page=0;
  s->focus = 200;
  s->profile_selection = profiles_active();
  pointer_inventory_refresh();
  return 0;
}

/* Stable control IDs keep general preferences and wallpaper controls distinct.
 */

static const int row_counts[] = {
    3, WALLPAPER_COUNT, 4, 3, 2, 4, 3, 4, 2, 2, 3, 2, 4, 2, 4, 3};
static int choice_focus(int row, int col) {
  if (row >= 7)
    return 100 + (row - 7) * 4 + col;
  if (row == 1 && col >= 3)
    return 21 + col - 3;
  if (row == 5)
    return 24 + col;
  if (row == 6)
    return 28 + col;
  return row * 4 + col;
}

static bool choice_enabled(int row, int col) {
  (void)col;
  if (row == 12 || row == 13)
    return desk.pointer_inventory.relative_touchpads > 0;
  if (row == 5 || row == 6)
    return desk.wallpaper_index >= WALLPAPER_ALPINE;
  return true;
}

enum {
  PAGE_PROFILES,
  PAGE_APPEARANCE,
  PAGE_WALLPAPER,
  PAGE_DOCK,
  PAGE_DISPLAY,
  PAGE_MOUSE,
  PAGE_TOUCHPAD,
  PAGE_DESKTOP, PAGE_WINDOWS, PAGE_FILES,
  PAGE_COUNT,
  PAGE_TERMINAL = 99
};
static const char *page_names[] = {"Profiles", "Appearance", "Wallpaper",
                                   "Dock",     "Display",    "Mouse",
                                   "Touchpad", "Desktop", "Windows", "Files"};
static const Icon page_icons[] = {I_HOME, I_IMAGE, I_IMAGE, I_LAYOUT,
                                  I_MAX,  I_GEAR,  I_GEAR, I_GRID,I_LAYOUT,I_FOLDER};
static const int row_page[] = {1, 2, 4, 1, 1, 2, 2, 5, 5, 5, 5, 5, 6, 6, 3, 3};
static const int page_height[] = {760,690,620,340,425,590,380,440,440,330};
static const int row_y[] = {200, 110, 252, 294, 110, 360, 444, 110,
                            194, 278, 362, 446, 110, 194, 110, 208};
static Rect settings_panel(Window *w) {
  Rect b = window_body(w);
  return (Rect){b.x + S(176), b.y, b.w - S(176), b.h - S(66)};
}
static Rect settings_view(Window *w) { return settings_panel(w); }
static int profile_buttons_y(void) { return 758 + profiles_count() * 44; }
static int content_height(Window *w) {
  Settings *s = w->state;
  return s->page == PAGE_PROFILES ? profile_buttons_y() + 120
                                  : page_height[s->page];
}
static int settings_max_scroll(Window *w) {
  return MAX(0, S(content_height(w)) - settings_view(w).h);
}
static const int page_order[]={0,1,2,3,7,9,8,4,5,6};
static Rect navigation(Window *w,int page) {
  Rect b=window_body(w);int top=b.h>=S(420)?90:60;
  int step=clampi((b.h-S(top+56))/PAGE_COUNT,S(18),S(32)),at=0;
  while(at<PAGE_COUNT && page_order[at]!=page)at++;
  int headers=at<1?1:at<6?2:3;
  return (Rect){b.x+S(10),b.y+S(top+headers*16)+at*step,S(156),MIN(S(30),step-S(2))};
}
static Rect profile_row(Window *w, int at) {
  Settings *s = w->state;
  Rect b = settings_panel(w);
  return (Rect){b.x + S(22), b.y + S(742 + at * 44) - s->scroll, b.w - S(44),
                S(38)};
}
static Rect choice(Window *w, int row, int col) {
  Settings *s = w->state;
  Rect b = settings_panel(w);
  int gap = S(8), width = (b.w - S(44) - gap * 3) / 4, y = row_y[row],
      height = 31;
  if (row == 1) {
    width = (b.w - S(44) - gap * 2) / 3;
    y += (col / 3) * 94;
    height = 87;
    col %= 3;
  }
  return (Rect){b.x + S(22) + col * (width + gap), b.y + S(y) - s->scroll,
                width, S(height)};
}
static Rect extra_control(Window *w, int id) {
  Settings *s = w->state;
  Rect b = settings_panel(w), body = window_body(w);
  int width = (b.w - S(52)) / 2;
  if (id >= 200 && id < 200 + PAGE_COUNT)
    return navigation(w, id - 200);
  if (id >= 150 && id <= 154) {
    int col = id - 150, count = col < 3 ? 3 : 2;
    if (col >= 3) col -= 3;
    int gap = S(8), cell = (b.w - S(44) - (count - 1) * gap) / count;
    return (Rect){b.x + S(22) + col * (cell + gap),
                  b.y + S(count == 3 ? 130 : 171) - s->scroll,
                  cell, S(32)};
  }
  if (id == 155 || id == 156)
    return (Rect){b.x + S(22) + (id - 155) * (width + S(8)),
                  b.y + S(370) - s->scroll, width, S(32)};
  if (id >= 300 && id < 308)
    return profile_row(w, id - 300);
  if (id >= 320 && id <= 325)
    return (Rect){b.x + S(22 + (id - 320) * 56), b.y + S(590) - s->scroll,
                  S(44), S(44)};
  if (id == 326)
    return (Rect){b.x + S(22), b.y + S(646) - s->scroll, b.w - S(44), S(30)};
  if(id>=330 && id<=337) {
    int row=id==330?218:id==331?258:id<=334?334:id<=336?414:460;
    int n=id>=332&&id<=334?3:id>=335&&id<=336?2:1;
    int col=n==3?id-332:n==2?id-335:0;
    int width=(b.w-S(44)-(n-1)*S(8))/n;
    return (Rect){b.x+S(22)+col*(width+S(8)),b.y+S(row)-s->scroll,width,S(30)};
  }
  if (id >= 500 && id < 535) {
    int group = (id - 500) / 7, col = (id - 500) % 7,
        n = group == 0   ? 5
            : group == 3 ? 2
            : group == 4 ? 2
                         : 7;
    int width = (b.w - S(44) - (n - 1) * S(6)) / n;
    return (Rect){b.x + S(22) + col * (width + S(6)),
                  b.y + S(110 + group * 80) - s->scroll, width, S(30)};
  }
  if (id >= 310 && id <= 312) {
    int step = (b.w - S(44)) / 3;
    return (Rect){b.x + S(22) + (id - 310) * step,
                  b.y + S(profile_buttons_y()) - s->scroll, step - S(7), S(30)};
  }
  if (id == 31 || id == 32)
    return (Rect){b.x + S(22) + (id - 31) * (width + S(8)),
                  b.y + S(548) - s->scroll, width, S(30)};
  if (id == 33)
    return (Rect){b.x + S(22), b.y + S(586) - s->scroll, S(150), S(28)};
  if (id == 140)
    return (Rect){b.x + S(22),
                  b.y + S(s->page == PAGE_MOUSE ? 542 : 320) - s->scroll,
                  S(160), S(28)};
  return (Rect){body.x + body.w - S(158), body.y + body.h - S(49), S(136),
                S(30)};
}
typedef struct {
  int *value, min, max, step;
  const char *unit;
} Slider;
static Window *slider_owner;
static int slider_id, slider_original;
static bool slider_for(int id, Slider *out) {
  Slider v;
  switch (id) {
  case 407:
    v = (Slider){&desk.pointer_speed, 25, 300, 5, "%"};
    break;
  case 410:
    v = (Slider){&desk.wheel_steps, 1, 10, 1, " steps"};
    break;
  case 412:
    v = (Slider){&desk.touchpad_speed, 25, 300, 5, "%"};
    break;
  case 414:
    v = (Slider){&desk.dock_size, 16, 112, 1, " px"};
    break;
  case 415:
    v = (Slider){&desk.dock_magnification, 0, 100, 1, "%"};
    break;
  case 416:
    v = (Slider){&desk.terminal_text_size, 9, 19, 2, " px"};
    break;
  case 417:
    v = (Slider){&desk.terminal_opacity, 40, 100, 1, "%"};
    break;
  default:
    return false;
  }
  *out = v;
  return true;
}
static Rect slider_rect(Window *w, int id) {
  Settings *s = w->state;
  Rect b = settings_panel(w);
  int y = id == 416 ? 530 : id == 417 ? 620 : row_y[id - 400];
  return (Rect){b.x + S(22), b.y + S(y) - s->scroll, b.w - S(44), S(32)};
}
static void slider_set(Window *w, int id, int value) {
  if (id == 412 && !desk.pointer_inventory.relative_touchpads)
    return;
  Slider v;
  if (!slider_for(id, &v))
    return;
  value = clampi(v.min + ((value - v.min + v.step / 2) / v.step) * v.step,
                 v.min, v.max);
  if (value == *v.value)
    return;
  *v.value = value;
  Settings *s = w->state;
  s->save_at = now_ms() + 300;
  if (id == 414 || id == 415)
    taskbar_preferences_changed();
  else if (id >= 416)
    terminal_preferences_changed();
  else
    pointer_preferences_changed();
  invalidate_window(w);
  if (desk.trace)
    printf("DESKTOP SETTINGS_SLIDER id=%d value=%d\n", id, value);
}
static void slider_at(Window *w, int id, int x) {
  Slider v;
  if (!slider_for(id, &v))
    return;
  Rect r = slider_rect(w, id);
  int left = r.x + S(9), width = MAX(1, r.w - S(96));
  slider_set(w, id,
             v.min + clampi(x - left, 0, width) * (v.max - v.min) / width);
}
static void slider_draw(Window *w, int id) {
  Slider v;
  if (!slider_for(id, &v))
    return;
  Rect r = slider_rect(w, id);
  Settings *s = w->state;
  bool enabled = id != 412 || desk.pointer_inventory.relative_touchpads > 0;
  int left = r.x + S(9), width = MAX(1, r.w - S(96)),
      x = left + (*v.value - v.min) * width / (v.max - v.min);
  rounded((Rect){left, r.y + S(14), width, S(4)}, S(2), THEME_DIVIDER);
  rounded((Rect){left, r.y + S(14), x - left, S(4)}, S(2),
          enabled ? desk.accent : THEME_DIVIDER);
  Rect knob = {x - S(7), r.y + S(9), S(14), S(14)};
  rounded(knob, S(7), desk.dark_mode ? 0xE4E8F0 : 0xFFFFFF);
  border(knob, S(7), enabled ? desk.accent : THEME_DIVIDER);
  if (s->focus == id)
    border(r, S(5), desk.accent);
  char number[32];
  format_uint(number, *v.value);
  append_text(number, sizeof(number), v.unit);
  label(r.x + r.w - S(70), r.y + S(9), number, FONT_NORMAL, THEME_TEXT, S(70));
}
bool settings_slider_pointer(int x, int y, unsigned buttons) {
  (void)y;
  if (!slider_owner)
    return false;
  Window *w = slider_owner;
  if (!w->alive || desk.dialog.open || desk.menu.open) {
    slider_owner = NULL;
    return false;
  }
  if (buttons & 1)
    slider_at(w, slider_id, x);
  else {
    slider_owner = NULL;
    ((Settings *)w->state)->save_at = now_ms();
  }
  return true;
}
typedef struct {
  int page,count;
  const char *title,*detail,*labels[8];
} PersonalChoice;
static const PersonalChoice personal[]={
  {PAGE_APPEARANCE,2,"Color mode","Choose the overall appearance.",{"Light","Dark"}},
  {PAGE_APPEARANCE,8,"Accent color","A shared color for selections and controls.",{"Azure","Ocean","Slate","Violet","Pink","Coral","Amber","Green"}},
  {PAGE_APPEARANCE,4,"Glass material","Adjust the translucency of bars, sidebars and popups.",{"Clear","Balanced","Frosted","Solid"}},
  {PAGE_APPEARANCE,3,"Window transitions","Control the fades used by windows and popups.",{"Off","Quick","Gentle"}},
  {PAGE_APPEARANCE,3,"Workspaces","Keep separate sets of open windows.",{"One","Two","Three"}},
  {PAGE_DESKTOP,2,"Desktop files","Show the files stored in your Desktop folder.",{"Hidden","Visible"}},
  {PAGE_DESKTOP,3,"Desktop icon size","Choose the size of desktop file and folder icons.",{"Small","Medium","Large"}},
  {PAGE_DESKTOP,3,"Spotlight results","Maximum results shown after searching or expanding.",{"5 results","7 results","9 results"}},
  {PAGE_WINDOWS,2,"Snap Assist","Choose other windows after the first snap.",{"Off","On"}},
  {PAGE_WINDOWS,2,"Maximize hover layouts","Show layout choices when hovering over maximize.",{"Off","On"}},
  {PAGE_WINDOWS,2,"Top snap bar","Show layout choices while dragging to the top.",{"Off","On"}},
  {PAGE_FILES,2,"Default folder view","The initial view in new Files windows.",{"Icons","Details"}},
  {PAGE_FILES,2,"Hidden files","The initial visibility in new Files windows.",{"Hidden","Visible"}}
};
enum { PERSONAL_COUNT=sizeof(personal)/sizeof(*personal) };
static bool personal_page(int page) {
  return page==PAGE_APPEARANCE || page==PAGE_DESKTOP || page==PAGE_WINDOWS || page==PAGE_FILES;
}
static int personal_y(int group) {
  int before=0;for(int i=0;i<group;i++)if(personal[i].page==personal[group].page)before++;
  return 92+before*116;
}
static Rect personal_rect(Window *w,int group,int col) {
  Rect b=settings_panel(w);Settings *s=w->state;
  int count=personal[group].count,gap=S(6),width=(b.w-S(64)-gap*(count-1))/count;
  return (Rect){b.x+S(32)+col*(width+gap),b.y+S(personal_y(group)+58)-s->scroll,width,S(32)};
}
static int personal_value(int group) {
  switch(group) {
    case 0:return desk.dark_mode;case 1:return desk.accent_index;
    case 2:return desk.glass_style;case 3:return desk.ui_motion;
    case 4:return desk.workspace_count-1;case 5:return desk.desktop_icons;
    case 6:return (desk.desktop_icon_size-32)/8;case 7:return (desk.launcher_rows-5)/2;
    case 8:return desk.snap_assist_enabled;case 9:return desk.snap_hover_enabled;
    case 10:return desk.snap_bar_enabled;case 11:return desk.files_default_details;
    default:return desk.files_default_hidden;
  }
}
static void personal_set(Window *w,int group,int col) {
  if(group<0 || group>=PERSONAL_COUNT || col<0 || col>=personal[group].count)return;
  switch(group) {
    case 0:desk.dark_mode=col;settings_apply();break;
    case 1:desk.accent_index=col;settings_apply();break;
    case 2:desk.glass_style=col;glass_reset();break;
    case 3:desk.ui_motion=col;break;
    case 4:choose_setting(w,3,col);break;
    case 5:desk.desktop_icons=col;break;
    case 6:desk.desktop_icon_size=32+col*8;break;
    case 7:desk.launcher_rows=5+col*2;break;
    case 8:desk.snap_assist_enabled=col;snap_cancel();break;
    case 9:desk.snap_hover_enabled=col;snap_cancel();break;
    case 10:desk.snap_bar_enabled=col;snap_cancel();break;
    case 11:desk.files_default_details=col;break;
    case 12:desk.files_default_hidden=col;break;
  }
  ((Settings *)w->state)->focus=600+group*8+col;
  preferences_save(w);invalidate_all();
  if(desk.trace)printf("DESKTOP SETTINGS_CUSTOM group=%d value=%d\n",group,col);
}
static void personal_draw(Window *w) {
  Settings *s=w->state;Rect b=settings_panel(w);int y=b.y-s->scroll;
  const char *intro=s->page==PAGE_APPEARANCE?"Make Coast feel like your desktop."
    :s->page==PAGE_DESKTOP?"A quiet launcher and your files, close at hand."
    :s->page==PAGE_WINDOWS?"Choose how windows arrange themselves."
    :"Set your preferred starting view.";
  label(b.x+S(22),y+S(57),intro,FONT_BODY,THEME_SECONDARY_TEXT,b.w-S(44));
  for(int g=0;g<PERSONAL_COUNT;g++)if(personal[g].page==s->page) {
    Rect card={b.x+S(20),y+S(personal_y(g)),b.w-S(40),S(104)};
    rounded(card,S(9),THEME_TOOLBAR);border(card,S(9),THEME_DIVIDER);
    label(card.x+S(12),card.y+S(12),personal[g].title,FONT_BOLD,THEME_TEXT,card.w-S(24));
    label(card.x+S(12),card.y+S(34),personal[g].detail,FONT_SMALL,THEME_SECONDARY_TEXT,card.w-S(24));
    int selected=personal_value(g);
    for(int col=0;col<personal[g].count;col++) {
      Rect r=personal_rect(w,g,col);bool focus=s->focus==600+g*8+col;
      if(g==1) {
        int size=MIN(S(24),r.w-S(4));Rect dot={r.x+(r.w-size)/2,r.y+(r.h-size)/2,size,size};
        rounded(dot,size/2,accents[col]);
        if(selected==col)icon(dot.x+size/4,dot.y+size/4,I_CHECK,size/2,0xFFFFFF);
        if(focus || selected==col)border((Rect){dot.x-S(3),dot.y-S(3),dot.w+S(6),dot.h+S(6)},size/2+S(3),desk.accent);
      } else {
        button(r,personal[g].labels[col],I_COUNT,selected==col,true,focus);
      }
    }
  }
}

static Rect focus_rect(Window *w, int id) {
  if (id >= 150 && id <= 156) return extra_control(w, id);
  if(id>=600 && id<600+PERSONAL_COUNT*8)return personal_rect(w,(id-600)/8,(id-600)%8);
  Slider v;
  if (slider_for(id, &v))
    return slider_rect(w, id);
  for (int row = 0; row < SETTINGS_ROWS; row++)
    for (int col = 0; col < row_counts[row]; col++)
      if (choice_focus(row, col) == id)
        return choice(w, row, col);
  return extra_control(w, id);
}
static bool focus_enabled(Window *w, int id) {
  if (id >= 150 && id <= 154) {
    unsigned at = id - 150;
    return ((Settings *)w->state)->page == PAGE_DISPLAY && desk.display_modeset &&
           display_modes[at].width <= (unsigned)desk.display_max_width &&
           display_modes[at].height <= (unsigned)desk.display_max_height;
  }
  if (id == 155 || id == 156)
    return ((Settings *)w->state)->page == PAGE_DISPLAY &&
           ((Settings *)w->state)->mode_until != 0;
  if(id>=600 && id<600+PERSONAL_COUNT*8)return personal[(id-600)/8].page==((Settings *)w->state)->page && (id-600)%8<personal[(id-600)/8].count;
  if (id == 412)
    return desk.pointer_inventory.relative_touchpads > 0;
  Settings *s = w->state;
  for (int row = 0; row < SETTINGS_ROWS; row++)
    for (int col = 0; col < row_counts[row]; col++)
      if (choice_focus(row, col) == id)
        return row_page[row] == s->page && choice_enabled(row, col);
  if (id == 522) {
    struct os_stat st;
    return !stat("/usr/lib/coast/linux-ports/bash/bash", &st) &&
           (st.mode & 0170000) == S_IFREG;
  }
  if (id >= 200 && id < 200 + PAGE_COUNT)
    return true;
  if (id >= 300 && id < 308)
    return s->page == PAGE_PROFILES && (unsigned)(id - 300) < profiles_count();
  if(id==335 || id==336 || id==337)return s->profile_selection==(int)profiles_active();
  if (id == 310)
    return profiles_count() < 8 && !profiles_status()[0];
  if (id == 311)
    return !profiles_status()[0];
  if (id == 312)
    return s->profile_selection != (int)profiles_active() &&
           !profiles_status()[0];
  return true;
}
static int controls(Window *w, int *out) {
  Settings *s = w->state;
  int n = 0;
  out[n++] = 200 + s->page;
  if (s->page == PAGE_PROFILES) {
    for (unsigned i = 0; i < profiles_count(); i++)
      out[n++] = 300 + i;
    out[n++] = 310;
    out[n++] = 311;
    out[n++] = 312;
    for (int i = 320; i <= 326; i++)
      out[n++] = i;
    for(int i=330;i<=337;i++)out[n++]=i;
  } else if(personal_page(s->page)) {
    for(int g=0;g<PERSONAL_COUNT;g++)if(personal[g].page==s->page)
      for(int col=0;col<personal[g].count;col++)out[n++]=600+g*8+col;
  } else if (s->page == PAGE_TERMINAL) {
    for (int g = 0; g < 5; g++)
      for (int col = 0; col < (g == 0 ? 5 : g >= 3 ? 2 : 7); col++)
        out[n++] = 500 + g * 7 + col;
    out[n++] = 416;
    out[n++] = 417;
  } else {
    if (s->page == PAGE_DISPLAY)
      for (int mode = 0; mode < 5; mode++) out[n++] = 150 + mode;
    for (int at = 0; at < SETTINGS_ROWS; at++) {
      static const int order[] = {4, 0, 3, 1, 5,  6,  14, 15,
                                  2, 7, 8, 9, 10, 11, 12, 13};
      int row = order[at];
      Slider v;
      if (row_page[row] == s->page && slider_for(400 + row, &v)) {
        out[n++] = 400 + row;
        continue;
      }
      if (row_page[row] == s->page)
        for (int col = 0; col < row_counts[row]; col++)
          out[n++] = choice_focus(row, col);
    }
    if (s->page == PAGE_DISPLAY && s->mode_until) {
      out[n++] = 155;
      out[n++] = 156;
    }
  }
  if (s->page == PAGE_WALLPAPER) {
    out[n++] = 31;
    out[n++] = 32;
    out[n++] = 33;
  }
  if (s->page == PAGE_MOUSE || s->page == PAGE_TOUCHPAD)
    out[n++] = 140;
  out[n++] = 20;
  return n;
}
Rect settings_hover(Window *w, int x, int y) {
  for (int page = 0; page < PAGE_COUNT; page++) {
    Rect r = navigation(w, page);
    if (contains(r, x, y))
      return r;
  }
  int list[64], n = controls(w, list);
  Rect view = settings_view(w);
  for (int i = 0; i < n; i++) {
    int id = list[i];
    if (id >= 200 && id < 200 + PAGE_COUNT)
      continue;
    Rect r = focus_rect(w, id);
    if (id != 20)
      r = intersect(r, view);
    if (focus_enabled(w, id) && contains(r, x, y))
      return r;
  }
  return (Rect){0};
}
static void select_page(Window *w, int page, bool content) {
  Settings *s = w->state;
  slider_owner = NULL;
  s->page = page;
  if (page == PAGE_WALLPAPER)
    wallpaper_prepare_previews();
  s->scroll = 0;
  s->profile_selection = profiles_active();
  smooth_jump(&s->animation, 0, now_ms());
  int list[64], n = controls(w, list);
  s->focus = 200 + page;
  if (content)
    for (int i = 1; i < n; i++)
      if (focus_enabled(w, list[i])) {
        s->focus = list[i];
        break;
      }
  invalidate_window(w);
  trace("SETTINGS_SECTION", w, page_names[page]);
  if (desk.trace) {
    Rect b = settings_panel(w);
    printf("DESKTOP SETTINGS_PANEL page=%d x=%d y=%d width=%d height=%d "
           "scale=%d\n",
           page, b.x, b.y, b.w, b.h, desk.scale);
  }
}
static void create_profile(Window *w, const char *name) {
  int e = profiles_create(name);
  if (e && !desk.dialog.open)
    dialog_error(w, "Create desktop profile", e);
  else {
    Settings *s = w->state;
    s->profile_selection = profiles_active();
    invalidate_window(w);
  }
}
static void rename_profile(Window *w, const char *name) {
  Settings *s = w->state;
  int e = profiles_rename(s->profile_selection, name);
  if (e)
    dialog_error(w, "Rename desktop profile", e);
  else
    invalidate_window(w);
}
static void change_tagline(Window *w, const char *text) {
  if (strlen(text) > 80) {
    dialog_error(w, "Profile description", -E_INVAL);
    return;
  }
  for (const char *c = text; *c; c++)
    if (*c < 32 || *c > 126) {
      dialog_error(w, "Profile description", -E_INVAL);
      return;
    }
  copy_text(desk.profile_tagline, sizeof(desk.profile_tagline), text);
  preferences_save(w);
  invalidate_window(w);
}
static void account_cancel(Window *w,const char *unused) {
  (void)unused;Settings *s=w->state;
  volatile char *p=s->new_password;for(unsigned i=0;i<sizeof(s->new_password);i++)p[i]=0;
}
static void account_commit(Window *w,const char *password) {
  Settings *s=w->state;struct os_account_request q;
  int e=profiles_account(s->account_target,&q);
  q.actor=profiles_uid(profiles_active());q.changes=s->account_op;
  if(password)copy_text(q.password,sizeof(q.password),password);
  if(s->account_op==OS_ACCOUNT_SET_PASSWORD)copy_text(q.new_password,sizeof(q.new_password),s->new_password);
  else if(s->account_op==OS_ACCOUNT_ROLE)q.flags=(q.flags&OS_ACCOUNT_ADMIN)?0:OS_ACCOUNT_ADMIN;
  else {q.flags&=OS_ACCOUNT_ADMIN;q.timeout_minutes=s->account_op>>8;q.changes=OS_ACCOUNT_TIMEOUT;}
  q.flags&=OS_ACCOUNT_ADMIN;
  if(!e)e=call2(SYS_account,OS_ACCOUNT_UPDATE,&q);
  volatile char *secret=(char *)&q;for(unsigned i=0;i<sizeof(q);i++)secret[i]=0;
  account_cancel(w,NULL);
  if(e)dialog_error(w,"Update account",e);
  invalidate_window(w);
}
static void account_authorize(Window *w) {
  struct os_account_request actor;int e=profiles_account(profiles_active(),&actor);
  if(e){account_cancel(w,NULL);dialog_error(w,"Account policy",e);return;}
  if(actor.flags&OS_ACCOUNT_PASSWORD)
    dialog_password(w,"Authorize account change","Enter the active profile's password. Administrator access is required to manage other profiles or change account type.",account_commit,account_cancel);
  else account_commit(w,"");
}
static void confirm_password(Window *w,const char *password) {
  Settings *s=w->state;
  if(strcmp(s->new_password,password)) {account_cancel(w,NULL);dialog_notice(w,"Passwords did not match","No account changes were made. Choose Set Password to try again.");return;}
  account_authorize(w);
}
static void new_password(Window *w,const char *password) {
  Settings *s=w->state;
  if(strlen(password)<8 || strlen(password)>127) {dialog_notice(w,"Choose a longer password","Use 8 to 127 characters. No account changes were made.");return;}
  copy_text(s->new_password,sizeof(s->new_password),password);
  dialog_password(w,"Confirm new password","Enter the new password again.",confirm_password,account_cancel);
}
static void switch_authenticated(Window *w,const char *password) {
  Settings *s=w->state;int e=profiles_switch(s->profile_selection,password);
  if(e && !desk.dialog.open)dialog_error(w,"Switch profile",e);
  else {s->profile_selection=profiles_active();invalidate_window(w);}
}
static void profiles_draw(Window *w) {
  Settings *s=w->state;Rect b=settings_panel(w);int y=b.y-s->scroll;
  unsigned at=s->profile_selection;
  const int tops[]={176,376,550,696};const int heights[]={192,170,138,profile_buttons_y()-696+42};
  for(unsigned i=0;i<4;i++) {Rect card={b.x+S(12),y+S(tops[i]),b.w-S(24),S(heights[i])};rounded(card,S(9),THEME_TOOLBAR);border(card,S(9),THEME_DIVIDER);}
  struct os_account_request q={0};int e=profiles_account(at,&q);
  avatar_draw((Rect){b.x+S(22),y+S(64),S(56),S(56)},profiles_avatar(at));
  label(b.x+S(92),y+S(69),profiles_name(at),FONT_BOLD,THEME_TEXT,b.w-S(114));
  label(b.x+S(92),y+S(91),at==profiles_active()?"Active profile":"Selected profile",FONT_SMALL,THEME_SECONDARY_TEXT,b.w-S(114));
  char identity[80]="User ID ",n[24];format_uint(n,profiles_uid(at));append_text(identity,sizeof(identity),n);
  append_text(identity,sizeof(identity),(q.flags&OS_ACCOUNT_ADMIN)?"  ·  Administrator":"  ·  Standard user");
  label(b.x+S(22),y+S(142),identity,FONT_BODY,THEME_SECONDARY_TEXT,b.w-S(44));
  label(b.x+S(22),y+S(190),"Password & administrator access",FONT_BOLD,THEME_TEXT,b.w-S(44));
  button(extra_control(w,330),(q.flags&OS_ACCOUNT_PASSWORD)?"Change password…":"Set password…",I_COUNT,false,!e,s->focus==330);
  button(extra_control(w,331),(q.flags&OS_ACCOUNT_ADMIN)?"Account type: Administrator":"Account type: Standard user",I_COUNT,false,!e,s->focus==331);
  label(b.x+S(22),y+S(310),"Require password for sudo",FONT_BOLD,THEME_TEXT,b.w-S(44));
  const char *timeouts[]={"Every command","After 5 minutes","After 15 minutes"};const unsigned values[]={0,5,15};
  for(int i=0;i<3;i++)button(extra_control(w,332+i),timeouts[i],I_COUNT,q.timeout_minutes==values[i],!e,s->focus==332+i);
  label(b.x+S(22),y+S(390),at==profiles_active()?"Terminal shell for this profile":"Switch to this profile to choose its shell",FONT_BOLD,THEME_TEXT,b.w-S(44));
  for(int i=0;i<2;i++)button(extra_control(w,335+i),i?"GNU Bash":"Coast",I_TERMINAL,desk.terminal_shell==i,at==profiles_active(),s->focus==335+i);
  button(extra_control(w,337),"Open home folder",I_HOME,false,true,s->focus==337);
  char home[OS_PATH_MAX];copy_text(home,sizeof(home),at?"/home/":"/home/user");if(at)append_text(home,sizeof(home),profiles_id(at));
  label(b.x+S(22),y+S(505),home,FONT_SMALL,THEME_SECONDARY_TEXT,b.w-S(44));
  label(b.x+S(22),y+S(527),"Passwords and administrator policy last for this live boot.",FONT_SMALL,THEME_SECONDARY_TEXT,b.w-S(44));
  label(b.x+S(22),y+S(564),"Personalize the active profile",FONT_BOLD,THEME_TEXT,b.w-S(44));
  for(int i=0;i<6;i++) {Rect r=extra_control(w,320+i);avatar_draw(r,i);if(desk.profile_avatar==i || s->focus==320+i)border((Rect){r.x-2,r.y-2,r.w+4,r.h+4},r.w/2,desk.accent);}
  button(extra_control(w,326),"Edit profile description",I_RENAME,false,true,s->focus==326);
  label(b.x+S(22),y+S(710),"Profiles",FONT_BOLD,THEME_TEXT,b.w-S(44));
  for(unsigned i=0;i<profiles_count();i++) {
    Rect r=profile_row(w,i);rounded(r,S(5),s->profile_selection==(int)i?desk.selection:THEME_TOOLBAR);
    if(s->focus==300+(int)i)border(r,S(5),desk.accent);
    avatar_draw((Rect){r.x+S(6),r.y+S(5),S(28),S(28)},profiles_avatar(i));
    label(r.x+S(44),r.y+S(11),profiles_name(i),FONT_NORMAL,THEME_TEXT,r.w-S(128));
    if(i==profiles_active())label(r.x+r.w-S(72),r.y+S(12),"Active",FONT_SMALL,desk.accent_text,S(64));
  }
  button(extra_control(w,310),"New",I_PLUS,false,focus_enabled(w,310),s->focus==310);
  button(extra_control(w,311),"Rename",I_RENAME,false,focus_enabled(w,311),s->focus==311);
  button(extra_control(w,312),"Switch",I_FORWARD,true,focus_enabled(w,312),s->focus==312);
  label(b.x+S(22),y+S(profile_buttons_y()+56),profiles_status()[0]?profiles_status():"New profiles copy personalization and start as standard users.",FONT_SMALL,THEME_SECONDARY_TEXT,b.w-S(44));
}
static void terminal_settings_draw(Window *w) {
  Settings *s = w->state;
  Rect b = settings_panel(w);
  int y = b.y - s->scroll;
  label(b.x + S(22), y + S(64),
        "Colors apply now. Shell changes apply to new Terminal windows.",
        FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(44));
  const char *heads[] = {"Palette", "Text color", "Background color",
                         "Shell for new windows", "ANSI colors"};
  const char *palettes[] = {"System", "Midnight", "Paper", "Forest", "Ember"};
  const char *colors[] = {"Palette", "Ink",   "Ivory", "Slate",
                          "Mint",    "Amber", "Blue"};
  const char *shells[] = {"Coast", "GNU Bash (basic)"};
  for (int g = 0; g < 5; g++) {
    label(b.x + S(22), y + S(84 + g * 80), heads[g], FONT_BOLD, THEME_TEXT,
          b.w - S(44));
    int selected = g == 0   ? desk.terminal_palette
                   : g == 1 ? desk.terminal_foreground
                   : g == 2 ? desk.terminal_background
                   : g == 3 ? desk.terminal_shell
                            : desk.terminal_ansi;
    int n = g == 0 ? 5 : g >= 3 ? 2 : 7;
    for (int c = 0; c < n; c++) {
      int id = 500 + g * 7 + c;
      Rect r = extra_control(w, id);
      button(r,
             g == 0   ? palettes[c]
             : g == 3 ? shells[c]
             : g == 4 ? (c ? "On" : "Off")
                      : colors[c],
             I_COUNT, c == selected, focus_enabled(w, id), s->focus == id);
    }
  }
  label(b.x + S(22), y + S(389),
        "Bash commands work; use Coast for full-screen applications.",
        FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(44));
  label(b.x + S(22), y + S(500), "Text size (native antialiased glyphs)",
        FONT_BOLD, THEME_TEXT, b.w - S(44));
  slider_draw(w, 416);
  label(b.x + S(22), y + S(590),
        "Background opacity (live backdrop blur below 100%)", FONT_BOLD,
        THEME_TEXT, b.w - S(44));
  slider_draw(w, 417);
  label(b.x + S(22), y + S(680),
        "zsh, fish and Oh My Zsh are not installed. See the port feasibility "
        "notes.",
        FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(44));
}

void settings_draw(Window *w) {
  Settings *s = w->state;
  Rect body = window_body(w), b = settings_panel(w), saved = desk.clip;
  clip_set(intersect(saved, body));
  fill(body, THEME_WINDOW);
  fill((Rect){body.x, body.y, S(176), body.h}, THEME_TOOLBAR);
  int avatar_size = body.h >= S(420) ? 42 : 28;
  avatar_draw(
      (Rect){body.x + S(16), body.y + S(12), S(avatar_size), S(avatar_size)},
      desk.profile_avatar);
  label(body.x + S(16 + avatar_size + 8), body.y + S(17),
        profiles_name(profiles_active()), FONT_BOLD, THEME_TEXT,
        S(112 - avatar_size));
  if (body.h >= S(420))
    label(body.x + S(18), body.y + S(70), "Profiles & accounts", FONT_SMALL,
          THEME_SECONDARY_TEXT, S(112));
  const int starts[]={0,1,6};const char *groups[]={"ACCOUNT","PERSONALIZE","SYSTEM"};
  for(int i=0;i<3;i++) {
    Rect r=navigation(w,page_order[starts[i]]);
    label(body.x+S(20),r.y-S(14),groups[i],FONT_SMALL,THEME_MUTED_TEXT,S(142));
  }
  for (int page = 0; page < PAGE_COUNT; page++) {
    Rect r = navigation(w, page);
    bool selected = page == s->page,
         hover = contains(r, desk.mouse_x, desk.mouse_y);
    if (selected || hover)
      rounded(r, S(5), selected ? desk.selection : theme_color(0xE5EAF0));
    if (s->focus == 200 + page)
      border(r, S(5), desk.accent);
    icon(r.x + S(10), r.y + (r.h - S(16)) / 2, page_icons[page], S(16),
         selected ? desk.accent_text : THEME_SECONDARY_TEXT);
    label(r.x + S(34), r.y + (r.h - S(14)) / 2, page_names[page], FONT_NORMAL,
          selected ? desk.accent_text : THEME_TEXT, r.w - S(42));
  }
  s->scroll = clampi(s->scroll, 0, settings_max_scroll(w));
  clip_set(intersect(saved, settings_view(w)));
  int y = b.y - s->scroll;
  label(b.x + S(22), y + S(22), page_names[s->page], FONT_TITLE, THEME_TEXT,
        b.w - S(44));
  if (s->page == PAGE_PROFILES)
    profiles_draw(w);
  else if (s->page == PAGE_TERMINAL)
    terminal_settings_draw(w);
  else if(personal_page(s->page))personal_draw(w);
  else {
    const char *headings[] = {"Accent",
                              "Wallpaper",
                              "Interface scale",
                              "Workspaces",
                              "Appearance",
                              "Image placement",
                              "Background color",
                              "Mouse speed",
                              "Pointer precision",
                              "Primary button",
                              "Wheel scroll amount",
                              "Mouse scroll direction",
                              "Touchpad speed",
                              "Touchpad scroll direction",
                              "Dock size",
                              "Dock magnification"};
    const char *accent_names[] = {"Azure", "Blue", "Slate"},
               *scale_names[] = {"100%", "125%", "150%", "200%"},
               *workspace_names[] = {"1 workspace", "2 workspaces",
                                     "3 workspaces"},
               *appearance_names[] = {"Light", "Dark"},
               *mode_names[] = {"Fill", "Fit", "Stretch", "Center"},
               *background_names[] = {"Slate", "Black", "Cream"};
    const char *speed_names[] = {"50%", "100%", "150%", "200%"};
    const char *dock_sizes[] = {"24 px", "30 px", "36 px", "42 px"};
    const char *dock_mags[] = {"Off", "Subtle 16%", "Gentle 24%"};
    const int speeds[] = {50, 100, 150, 200};
    const char *precision_names[] = {"Linear", "Adaptive"},
               *button_names[] = {"Left", "Right"},
               *wheel_names[] = {"1 step", "2 steps", "3 steps"},
               *direction_names[] = {"Standard", "Reversed"};
    for (int row = 0; row < SETTINGS_ROWS; row++) {
      if (row_page[row] != s->page)
        continue;
      label(b.x + S(22), y + S(row_y[row] - 26), headings[row], FONT_BOLD,
            theme_color(0x41525A), b.w - S(44));
      Slider slider;
      if (slider_for(400 + row, &slider)) {
        slider_draw(w, 400 + row);
        continue;
      }
      for (int col = 0; col < row_counts[row]; col++) {
        Rect r = choice(w, row, col);
        bool selected =
            col == (row == 0    ? desk.accent_index
                    : row == 1  ? desk.wallpaper_index
                    : row == 2  ? desk.scale_index
                    : row == 3  ? desk.workspace_count - 1
                    : row == 4  ? desk.dark_mode
                    : row == 5  ? desk.wallpaper_mode
                    : row == 6  ? desk.wallpaper_background
                    : row == 7  ? -1
                    : row == 8  ? desk.pointer_acceleration
                    : row == 9  ? desk.primary_right
                    : row == 10 ? desk.wheel_steps - 1
                    : row == 11 ? desk.natural_scroll
                    : row == 12 ? -1
                    : row == 13 ? desk.touchpad_natural
                    : row == 14 ? (desk.dock_size - 24) / 6
                                : (desk.dock_magnification == 0    ? 0
                                   : desk.dock_magnification == 16 ? 1
                                                                   : 2));
        if (row == 7 || row == 12)
          selected = speeds[col] ==
                     (row == 7 ? desk.pointer_speed : desk.touchpad_speed);
        bool enabled = choice_enabled(row, col);
        bool hover = enabled && contains(r, desk.mouse_x, desk.mouse_y);
        rounded(r, S(5),
                selected ? desk.selection
                : hover  ? theme_color(0xEDF1EF)
                         : THEME_TOOLBAR);
        border(r, S(5), selected ? desk.accent : theme_color(0xE5E9E8));
        if (s->focus == choice_focus(row, col))
          border((Rect){r.x - 2, r.y - 2, r.w + 4, r.h + 4}, S(6),
                 theme_color(0x96B5A9));
        int x = r.x + S(10);
        if (row == 1) {
          Rect preview = {r.x + S(5), r.y + S(5), r.w - S(10), r.h - S(31)};
          fill(preview, wallpaper_backgrounds[0]);
          wallpaper_draw_preview(col, preview);
          if (!wallpaper_preview_ready(col) &&
              !(col == WALLPAPER_CUSTOM && !desk.wallpaper_path[0]))
            label(preview.x + S(6), preview.y + preview.h / 2,
                  wallpaper_preview_failed(col) ? "Preview unavailable"
                                                : "Preparing preview...",
                  FONT_SMALL, 0xD1DDE8, preview.w - S(12));
          if (col == WALLPAPER_CUSTOM && !desk.wallpaper_path[0])
            icon(preview.x + (preview.w - S(22)) / 2,
                 preview.y + (preview.h - S(22)) / 2, I_PLUS, S(22), 0xB9CCCD);
          if (selected) {
            rounded((Rect){preview.x + preview.w - S(23), preview.y + S(3),
                           S(20), S(20)},
                    S(4), desk.accent);
            icon(preview.x + preview.w - S(20), preview.y + S(6), I_CHECK,
                 S(14), 0xFFFFFF);
          }
          label(x, r.y + r.h - S(20), wallpaper_names[col], FONT_NORMAL,
                selected ? desk.accent_text : THEME_SECONDARY_TEXT,
                r.w - S(20));
          continue;
        }

        if (row == 0 || row == 6) {
          rounded((Rect){x, r.y + S(9), S(13), S(13)}, S(3),
                  row == 0 ? accents[col] : wallpaper_backgrounds[col]);
          x += S(20);
        }
        label(x, r.y + S(8),
              row == 14               ? dock_sizes[col]
              : row == 15             ? dock_mags[col]
              : row == 0              ? accent_names[col]
              : row == 2              ? scale_names[col]
              : row == 3              ? workspace_names[col]
              : row == 4              ? appearance_names[col]
              : row == 5              ? mode_names[col]
              : row == 6              ? background_names[col]
              : row == 7 || row == 12 ? speed_names[col]
              : row == 8              ? precision_names[col]
              : row == 9              ? button_names[col]
              : row == 10             ? wheel_names[col]
                                      : direction_names[col],
              FONT_NORMAL,
              enabled ? theme_color(0x617176) : theme_color(0xAFB8B9),
              r.w - (x - r.x) - S(7));
      }
    }

    if (s->page == PAGE_APPEARANCE)
      label(b.x + S(22), y + S(64),
            "Color, appearance and workspace preferences for this profile.",
            FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(44));
    if (s->page == PAGE_DISPLAY) {
      label(b.x + S(22), y + S(64),
            "Choose a display resolution and a comfortable interface scale.", FONT_SMALL,
            THEME_SECONDARY_TEXT, b.w - S(44));
      char current[72], number[24];
      copy_text(current, sizeof(current), "Current: ");
      format_uint(number, desk.width);
      append_text(current, sizeof(current), number);
      append_text(current, sizeof(current), " x ");
      format_uint(number, desk.height);
      append_text(current, sizeof(current), number);
      label(b.x + S(22), y + S(96), current, FONT_BOLD, THEME_TEXT,
            b.w - S(44));
      for (int mode = 0; mode < 5; mode++) {
        int id = 150 + mode;
        button(extra_control(w, id), display_modes[mode].label, I_MAX,
               desk.width == (int)display_modes[mode].width &&
                   desk.height == (int)display_modes[mode].height,
               focus_enabled(w, id), s->focus == id);
      }
      label(b.x + S(22), y + S(316),
            desk.display_modeset
                ? "Resolution changes the pixels; scale changes UI size."
                : "This display uses a fixed firmware framebuffer.",
            FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(44));
      if (s->mode_until) {
        char remaining[64];
        copy_text(remaining, sizeof(remaining), "Keep this resolution? Reverting in ");
        format_uint(number, (s->mode_until > now_ms()
                                ? s->mode_until - now_ms() + 999 : 0) / 1000);
        append_text(remaining, sizeof(remaining), number);
        append_text(remaining, sizeof(remaining), " s");
        label(b.x + S(22), y + S(348), remaining, FONT_BOLD,
              THEME_TEXT, b.w - S(44));
        button(extra_control(w, 155), "Keep resolution", I_CHECK, true,
               true, s->focus == 155);
        button(extra_control(w, 156), "Revert", I_RELOAD, false,
               true, s->focus == 156);
      }
    }
    if (s->page == PAGE_DOCK) {
      label(b.x + S(22), y + S(64),
            "Right-click an app to keep it in or remove it from the Dock.",
            FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(44));
      label(b.x + S(22), y + S(272),
            "Running apps stay visible until their last window closes.",
            FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(44));
    }
    if (s->page == PAGE_WALLPAPER) {
      char current[OS_PATH_MAX + 32];
      copy_text(current, sizeof(current), "Current: ");
      append_text(current, sizeof(current),
                  wallpaper_names[desk.wallpaper_index]);
      label(b.x + S(22), y + S(64),
            desk.wallpaper_status[0] ? desk.wallpaper_status : current,
            FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(44));
      label(b.x + S(22), y + S(488),
            "Image placement applies to PNG, JPEG and BMP wallpapers.",
            FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(44));
      button(extra_control(w, 31), "Built-in images", I_FOLDER, false, true,
             s->focus == 31);
      button(extra_control(w, 32), "My images", I_FOLDER, false, true,
             s->focus == 32);
      button(extra_control(w, 33), "Reset wallpaper", I_RELOAD, false, true,
             s->focus == 33);
    }
    if (s->page == PAGE_MOUSE || s->page == PAGE_TOUCHPAD) {
      label(b.x + S(22), y + S(64),
            s->page == PAGE_MOUSE
                ? "Pointer and wheel behavior for supported USB mice."
            : desk.pointer_inventory.relative_touchpads
                ? "Controls for the detected relative USB touchpad."
                : "No supported relative USB touchpad detected.",
            FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(44));
      if (s->page == PAGE_MOUSE)
        label(
            b.x + S(22), y + S(502),
            "Linear is direct movement. Adaptive adds speed to faster motion.",
            FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(44));
      else
        label(
            b.x + S(22), y + S(254),
            "Tap, gestures, absolute, I2C and PS/2 touchpads are unavailable.",
            FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(44));
      button(extra_control(w, 140), "Reset pointer", I_RELOAD, false, true,
             s->focus == 140);
    }
  }
  scrollbar(settings_view(w), S(content_height(w)), s->scroll);
  desk.clip = saved;
  clip_set(intersect(saved, body));
  Rect foot = {b.x, body.y + body.h - S(66), b.w, S(66)};
  fill(foot, THEME_WINDOW);
  fill((Rect){foot.x, foot.y, foot.w, 1}, THEME_DIVIDER);
  label(foot.x + S(22), foot.y + S(13),
        desk.config_on_disk ? "Saved values persist on disk."
                            : "RAM preferences: reboot clears them.",
        FONT_SMALL, THEME_SECONDARY_TEXT, foot.w - S(190));
  label(foot.x + S(22), foot.y + S(36), "Changes apply immediately. Save keeps your choices.", FONT_SMALL,
        THEME_MUTED_TEXT, foot.w - S(190));
  button(extra_control(w, 20), "Save", I_SAVE, true, true, s->focus == 20);
  desk.clip = saved;
}

static void wallpaper_error(Window *w, int error) {
  if (error == -E_INVAL)
    dialog_notice(w, "Cannot use this wallpaper",
                  "Choose a valid PNG, JPEG or uncompressed 24/32-bit BMP. "
                  "Images may be up to "
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
  invalidate_window(w);
  trace("WALLPAPER", w, desk.wallpaper_path);
}

static void choose_setting(Window *w, int row, int col) {
  Settings *s = w->state;
  s->focus = choice_focus(row, col);
  if (!choice_enabled(row, col))
    return;
  if (row >= 14) {
    if (row == 14)
      desk.dock_size = 24 + col * 6;
    else
      desk.dock_magnification = col == 0 ? 0 : col == 1 ? 16 : 24;
    taskbar_preferences_changed();
    preferences_save(w);
    return;
  }
  if (row >= 7) {
    static const int speeds[] = {50, 100, 150, 200};
    if (row == 7)
      desk.pointer_speed = speeds[col];
    else if (row == 8)
      desk.pointer_acceleration = col;
    else if (row == 9)
      desk.primary_right = col;
    else if (row == 10)
      desk.wheel_steps = col + 1;
    else if (row == 11)
      desk.natural_scroll = col;
    else if (row == 12)
      desk.touchpad_speed = speeds[col];
    else if (row == 13)
      desk.touchpad_natural = col;
    pointer_preferences_changed();
  } else if (row == 0) {
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
      dialog_input(w, "Choose wallpaper",
                   "Enter the path to a PNG, JPEG or BMP image.", path,
                   choose_image);
      return;
    }

    int error = wallpaper_set(col, NULL);
    if (error) {
      wallpaper_error(w, error);
      return;
    }
    invalidate_window(w);
    trace("WALLPAPER", w, wallpaper_names[col]);
  } else if (row == 2) {
    scale_apply(col);
  } else if (row == 4) {
    desk.dark_mode = col != 0;
    settings_apply();
    trace("APPEARANCE", w, desk.dark_mode ? "dark" : "light");
  } else if (row == 5) {
    desk.wallpaper_mode = col;
    wallpaper_generate();
    invalidate_window(w);
    trace("WALLPAPER_MODE", w, "");
  } else if (row == 6) {
    desk.wallpaper_background = col;
    wallpaper_generate();
    invalidate_window(w);
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
  if (!e)
    e = profiles_save_registry();
  if (e)
    dialog_error(w, "Save preferences", e);
  else {
    struct os_stat st;
    bool disk = !stat(desk.config, &st) && st.dev;
    dialog_notice(w, "Preferences saved",
                  disk ? "Saved to writable disk. These preferences will be "
                         "loaded on the next "
                         "desktop launch and after reboot."
                       : "Saved in the RAM filesystem. These preferences will "
                         "be loaded on the "
                         "next desktop launch during this boot; reboot clears "
                         "RAM storage.");
    trace("CONFIG_SAVED", w, desk.config);
  }
}

static void display_restore(Window *w) {
  Settings *s = w->state;
  if (!s->mode_until) return;
  unsigned width = s->mode_previous_width, height = s->mode_previous_height;
  s->mode_until = 0;
  int error = desktop_set_resolution(width, height);
  if (error) dialog_error(w, "Revert display resolution", error);
  invalidate_window(w);
}

static void display_choose(Window *w, unsigned at) {
  Settings *s = w->state;
  if (at >= 5 || !focus_enabled(w, 150 + at)) return;
  unsigned width = display_modes[at].width, height = display_modes[at].height;
  if (width == (unsigned)desk.width && height == (unsigned)desk.height) return;
  if (!s->mode_until) {
    s->mode_previous_width = desk.width;
    s->mode_previous_height = desk.height;
  }
  int error = desktop_set_resolution(width, height);
  if (error) {
    dialog_error(w, "Change display resolution", error);
    return;
  }
  if (width == s->mode_previous_width && height == s->mode_previous_height)
    s->mode_until = 0;
  else {
    s->mode_until = now_ms() + 15000;
    s->focus = 155;
  }
  invalidate_all();
}

static void display_keep(Window *w) {
  Settings *s = w->state;
  if (!s->mode_until) return;
  s->mode_until = 0;
  desk.preferred_width = desk.width;
  desk.preferred_height = desk.height;
  preferences_save(w);
  invalidate_window(w);
}

static void activate(Window *w, int focus) {
  if(focus>=600 && focus<600+PERSONAL_COUNT*8){personal_set(w,(focus-600)/8,(focus-600)%8);return;}
  Settings *s = w->state;
  if (focus >= 150 && focus <= 154) {
    display_choose(w, focus - 150);
    return;
  }
  if (focus == 155) {
    display_keep(w);
    return;
  }
  if (focus == 156) {
    display_restore(w);
    return;
  }
  if (focus >= 320 && focus <= 325) {
    desk.profile_avatar = focus - 320;
    profiles_avatar_changed();
    preferences_save(w);
    invalidate_all();
    return;
  }
  if (focus == 326) {
    dialog_input(w, "Profile description",
                 "1-80 printable ASCII characters. Stored with this profile's "
                 "preferences.",
                 desk.profile_tagline, change_tagline);
    return;
  }
  if (focus >= 500 && focus < 535) {
    int g = (focus - 500) / 7, c = (focus - 500) % 7;
    if (g == 0)
      desk.terminal_palette = c;
    if (g == 1)
      desk.terminal_foreground = c;
    if (g == 2)
      desk.terminal_background = c;
    if (g == 3)
      desk.terminal_shell = c;
    if (g == 4)
      desk.terminal_ansi = c;
    terminal_preferences_changed();
    preferences_save(w);
    invalidate_window(w);
    return;
  }
  Slider slider;
  if (slider_for(focus, &slider))
    return;
  if (focus >= 200 && focus < 200 + PAGE_COUNT) {
    select_page(w, focus - 200, false);
    return;
  }
  if (focus >= 300 && focus < 308) {
    s->profile_selection = focus - 300;
    invalidate_window(w);
    return;
  }
  if(focus>=330 && focus<=337) {
    account_cancel(w,NULL);s->account_target=s->profile_selection;
    if(focus==330) {s->account_op=OS_ACCOUNT_SET_PASSWORD;dialog_password(w,"Set profile password","Use 8 to 127 characters. This password protects profile switching and authorizes sudo for administrators.",new_password,account_cancel);}
    else if(focus==331) {s->account_op=OS_ACCOUNT_ROLE;account_authorize(w);}
    else if(focus<=334) {const unsigned values[]={0,5,15};s->account_op=OS_ACCOUNT_TIMEOUT|(values[focus-332]<<8);account_authorize(w);}
    else if(focus<=336) {desk.terminal_shell=focus-335;terminal_preferences_changed();preferences_save(w);invalidate_window(w);}
    else {char home[OS_PATH_MAX];copy_text(home,sizeof(home),s->profile_selection?"/home/":"/home/user");if(s->profile_selection)append_text(home,sizeof(home),profiles_id(s->profile_selection));wm_open(APP_FILES,home);}
    return;
  }
  if (focus == 310) {
    dialog_input(
        w, "New desktop profile",
        "Create a standard-user profile with its own home. Creating it switches profiles and closes apps and terminal sessions; save your work first. Use 1-40 printable ASCII characters.",
        "", create_profile);
    return;
  }
  if (focus == 311) {
    dialog_input(
        w, "Rename desktop profile",
        "Changes the display name only. No account or authentication changes.",
        profiles_name(s->profile_selection), rename_profile);
    return;
  }
  if (focus == 312) {
    struct os_account_request q;int e=profiles_account(s->profile_selection,&q);
    if(e)dialog_error(w,"Switch profile",e);
    else if(q.flags&OS_ACCOUNT_PASSWORD)dialog_password(w,"Switch profile","Enter the selected profile's password. Switching closes apps and terminal sessions and clears sudo approvals. Save your work first.",switch_authenticated,NULL);
    else dialog_confirm(w,"Switch profile","Switching closes apps and terminal sessions and clears sudo approvals. Save your work first.",switch_authenticated);
    return;
  }

  if (focus == 140) {
    pointer_reset_defaults();
    invalidate_window(w);
    return;
  }
  if (focus == 20) {
    save_preferences(w);
    return;
  }
  if (focus == 33) {
    wallpaper_set(0, NULL);
    desk.wallpaper_mode = WALLPAPER_FILL;
    desk.wallpaper_background = 0;
    settings_apply();
    trace("WALLPAPER", w, "Coast");
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
  Rect view = settings_view(w);
  if (wheel) {
    int list[64], n = controls(w, list);
    for (int i = 0; i < n; i++) {
      Slider slider;
      if (slider_for(list[i], &slider) && focus_enabled(w, list[i]) &&
          contains(intersect(slider_rect(w, list[i]), view), x, y)) {
        s->focus = list[i];
        int notches = wheel / MAX(1, desk.wheel_steps);
        if (!notches)
          notches = wheel > 0 ? 1 : -1;
        slider_set(w, list[i], *slider.value + notches * slider.step);
        return true;
      }
    }
    if (contains(view, x, y))
      smooth_by(&s->animation, s->scroll, -(int64_t)wheel * S(40),
                settings_max_scroll(w), now_ms());
    return true;
  }
  if (!(pressed & 1))
    return false;
  for (int page = 0; page < PAGE_COUNT; page++)
    if (contains(navigation(w, page), x, y)) {
      select_page(w, page, false);
      return true;
    }
  int list[64], n = controls(w, list);
  for (int i = 1; i < n; i++) {
    Rect r = focus_rect(w, list[i]);
    if (list[i] != 20)
      r = intersect(r, view);
    if (focus_enabled(w, list[i]) && contains(r, x, y)) {
      s->focus = list[i];
      Slider slider;
      if (slider_for(list[i], &slider)) {
        slider_owner = w;
        slider_id = list[i];
        slider_original = *slider.value;
        smooth_jump(&s->animation, s->scroll, now_ms());
        slider_at(w, slider_id, x);
      } else
        activate(w, list[i]);
      invalidate_window(w);
      return true;
    }
  }
  return contains(window_body(w), x, y);
}
void settings_key(Window *w, Key k) {
  if (!k.down)
    return;
  Settings *s = w->state;
  Slider slider;
  if (k.code == KEY_ESCAPE && slider_owner == w) {
    slider_set(w, slider_id, slider_original);
    slider_owner = NULL;
    return;
  }
  if (slider_for(s->focus, &slider) &&
      (k.code == KEY_LEFT || k.code == KEY_RIGHT || k.code == KEY_UP ||
       k.code == KEY_DOWN || k.code == KEY_HOME || k.code == KEY_END)) {
    int amount = slider.step * ((k.mods & MOD_SHIFT) ? 10 : 1);
    slider_set(w, s->focus,
               k.code == KEY_HOME ? slider.min
               : k.code == KEY_END
                   ? slider.max
                   : *slider.value + ((k.code == KEY_LEFT || k.code == KEY_DOWN)
                                          ? -amount
                                          : amount));
    return;
  }
  if ((k.mods & MOD_CTRL) && (k.ascii == 's' || k.ascii == 'S')) {
    save_preferences(w);
    return;
  }
  if ((k.mods & MOD_CTRL) && k.ascii >= '0' && k.ascii <= '9') {
    select_page(w, k.ascii=='0'?9:k.ascii-'1', true);
    return;
  }
  if (s->focus >= 200 && s->focus < 200 + PAGE_COUNT &&
      (k.code == KEY_UP || k.code == KEY_DOWN)) {
    int at=0;while(at<PAGE_COUNT && page_order[at]!=s->page)at++;
    select_page(w,page_order[(at+(k.code==KEY_DOWN?1:PAGE_COUNT-1))%PAGE_COUNT],false);
    return;
  }
  if (k.code == KEY_PAGEUP || k.code == KEY_PAGEDOWN) {
    smooth_by(&s->animation, s->scroll,
              (k.code == KEY_PAGEUP ? -1 : 1) * S(200), settings_max_scroll(w),
              now_ms());
    return;
  }
  if (k.code == KEY_TAB || k.code == KEY_LEFT || k.code == KEY_RIGHT ||
      k.code == KEY_UP || k.code == KEY_DOWN) {
    int delta = k.code == KEY_LEFT || k.code == KEY_UP ||
                        (k.code == KEY_TAB && (k.mods & MOD_SHIFT))
                    ? -1
                    : 1;
    int list[64], n = controls(w, list), at = 0;
    while (at < n && list[at] != s->focus)
      at++;
    if (at == n)
      at = 0;
    do {
      at = (at + delta + n) % n;
    } while (!focus_enabled(w, list[at]));
    s->focus = list[at];
    Rect r = focus_rect(w, s->focus), v = settings_view(w);
    if ((s->focus != 20 && s->focus < 200) || s->focus >= 300) {
      if (r.y < v.y)
        s->scroll -= v.y - r.y + S(3);
      if (r.y + r.h > v.y + v.h)
        s->scroll += r.y + r.h - v.y - v.h + S(3);
    }
    s->scroll = clampi(s->scroll, 0, settings_max_scroll(w));
    smooth_jump(&s->animation, s->scroll, now_ms());
    invalidate_window(w);
  } else if (k.code == KEY_ENTER || k.code == KEY_SPACE) {
    activate(w, s->focus);
    invalidate_window(w);
  }
}
void settings_tick(Window *w) {
  Settings *s = w->state;
  if (s->mode_until) {
    uint64_t now = now_ms();
    if (now >= s->mode_until)
      display_restore(w);
    else if (s->mode_seconds != (s->mode_until - now + 999) / 1000) {
      s->mode_seconds = (s->mode_until - now + 999) / 1000;
      invalidate_window(w);
    }
  }
  if (s->save_at && slider_owner != w && now_ms() >= s->save_at) {
    s->save_at = 0;
    preferences_save(w);
  }
  if ((s->page == PAGE_MOUSE || s->page == PAGE_TOUCHPAD) &&
      now_ms() - desk.pointer_info_at > 1000) {
    struct os_pointer_inventory before = desk.pointer_inventory;
    pointer_inventory_refresh();
    if (memcmp(&before, &desk.pointer_inventory, sizeof(before)))
      invalidate_window(w);
  }
  int position =
      smooth_tick(&s->animation, s->scroll, settings_max_scroll(w), now_ms());
  if (position != s->scroll) {
    s->scroll = position;
    invalidate(settings_view(w));
  }
}

bool settings_close(Window *w) {
  account_cancel(w,NULL);
  Settings *s = w->state;
  if (s->mode_until) display_restore(w);
  if (slider_owner == w)
    slider_owner = NULL;
  if (s->save_at) {
    s->save_at = 0;
    if (preferences_save(w))
      return false;
  }
  deallocate(w->state, sizeof(Settings));
  w->state = NULL;
  return true;
}

static void settings_command(Window *w, int command) {
  if(command>=20 && command<20+PAGE_COUNT){select_page(w,command-20,true);return;}
  if (command == 0)
    save_preferences(w);
  else if (command == 1)
    wm_close(w);
  else if (command == 2 || command == 3 || command == 6 || command == 7 ||
           command == 8 || command == 9 || command == 10) {
    int page = command == 2 ? PAGE_APPEARANCE
               : command == 3 ? PAGE_MOUSE
               : command == 6 ? PAGE_WALLPAPER
               : command == 7 ? PAGE_DISPLAY
               : command == 8 ? PAGE_DOCK
               : command == 9 ? PAGE_TOUCHPAD
                              : PAGE_PROFILES;
    select_page(w, page, true);
  } else if (command == 4) {
    wm_open(APP_FILES, "/usr/share/wallpapers");
  } else if (command == 5) {
    char path[OS_PATH_MAX];
    join_path(path, desk.home, "Pictures/Wallpapers");
    wm_open(APP_FILES, path);
  }
}
void settings_menu(Window *w, int group, int x, int y) {
  if (group == MENU_FILE) {
    MenuItem items[] = {{"Save preferences\tCtrl+S", I_SAVE, 0, true},
                        {"Built-in wallpapers", I_IMAGE, 4, true},
                        {"My wallpapers", I_FOLDER, 5, true},
                        {"Close Settings\tAlt+F4", I_CLOSE, 1, true}};
    menu_open(w, x, y, items, 4, settings_command);
  } else if (group == MENU_VIEW) {
    MenuItem items[] = {
        {"Appearance", I_LAYOUT, 2, true}, {"Wallpaper", I_IMAGE, 6, true},
        {"Dock", I_LAYOUT, 8, true},       {"Display", I_MAX, 7, true},
        {"Mouse", I_GEAR, 3, true},        {"Touchpad", I_GEAR, 9, true},
        {"Profiles", I_HOME, 10, true},
        {"Desktop",I_GRID,27,true},{"Windows",I_LAYOUT,28,true},{"Files",I_FOLDER,29,true}};
    menu_open(w, x, y, items, 10, settings_command);
  }
}

int preferences_save(Window *owner) {
  int error = config_save();
  if (!error)
    error = profiles_save_registry();
  if (error)
    dialog_error(owner, "Save preferences (active for this session)", error);
  else
    trace("CONFIG_SAVED", owner, desk.config);
  return error;
}
static void settings_open_category(int command) {
  Window *w = NULL;
  for (int i = 0; i < MAX_WINDOWS; i++)
    if (desk.windows[i].alive && desk.windows[i].app == APP_SETTINGS) {
      w = &desk.windows[i];
      break;
    }
  if (w)
    wm_focus(w);
  else
    w = wm_open(APP_SETTINGS, NULL);
  if (w)
    settings_command(w, command);
}

void settings_open_dock(void) { settings_open_category(8); }

void config_load(void) {
  desktop_file_scope(0);config_load_root();
  desktop_file_scope(profiles_uid(profiles_active()));
}
int config_save(void) {
  unsigned old=desktop_file_scope(0);int e=config_save_root();desktop_file_scope(old);return e;
}
int preferences_write(const char *path,const char *content) {
  unsigned old=desktop_file_scope(0);int e=preferences_write_root(path,content);desktop_file_scope(old);return e;
}
