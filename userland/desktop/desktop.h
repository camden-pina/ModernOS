#include <uapi/account.h>
#ifndef COAST_DESKTOP_H
#define COAST_DESKTOP_H
#include "pointer_motion.h"
#include "smooth_scroll.h"
#include "theme.h"
#include <libc.h>
#include <stdbool.h>
#include <uapi/pointer.h>

#define MAX_WINDOWS 16
#define MAX_MENU_ITEMS 128
#define MAX_RECENT 48
#define SURFACE_GRID 32
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct {
  int x, y, w, h;
} Rect;

typedef enum {
  APP_FILES,
  APP_TERMINAL,
  /* Retired browser ID. Keep later IDs stable for saved dock preferences. */
  APP_RETIRED_BROWSER,
  APP_SETTINGS,
  APP_EDITOR,
  APP_POWER,
  APP_GAME,
  APP_DISKS,
  APP_CODE,
  APP_WORKBENCH,
  APP_QT,
  APP_CHROMIUM,
  APP_COUNT
} App;
/* Applications using the desktop's native copied-pixel window service. */
static inline bool native_window_app(App app) {
  return app == APP_GAME || app == APP_WORKBENCH || app == APP_QT || app == APP_CHROMIUM;
}
enum {
  WALLPAPER_ALPINE = 3,
  WALLPAPER_DESERT,
  WALLPAPER_CUSTOM,
  WALLPAPER_COUNT
};
enum { WALLPAPER_FILL, WALLPAPER_FIT, WALLPAPER_STRETCH, WALLPAPER_CENTER };
typedef enum {
  FONT_SMALL,
  FONT_NORMAL,
  FONT_MEDIUM,
  FONT_BODY,
  FONT_BOLD,
  FONT_TITLE,
  FONT_MONO,
  FONT_EDIT,
  FONT_TERM9,
  FONT_TERM15,
  FONT_TERM17,
  FONT_TERM19,
  FONT_COUNT
} Font;

typedef enum {
  I_FOLDER,
  I_HOME,
  I_CLOCK,
  I_DOWNLOAD,
  I_IMAGE,
  I_TRASH,
  I_DRIVE,
  I_BACK,
  I_FORWARD,
  I_UP,
  I_SEARCH,
  I_MINUS,
  I_MAX,
  I_CLOSE,
  I_LAYOUT,
  I_PLUS,
  I_GRID,
  I_TERMINAL,
  I_GLOBE,
  I_GEAR,
  I_WIFI,
  I_SOUND,
  I_DOC,
  I_COPY,
  I_CUT,
  I_PASTE,
  I_RENAME,
  I_INFO,
  I_CHECK,
  I_RESTORE,
  I_SAVE,
  I_WORKSPACE,
  I_RELOAD,
  I_CODE,
  I_LIST,
  I_SORT,
  I_MORE,
  I_COUNT
} Icon;

enum { MOD_CTRL = 0x11, MOD_SHIFT = 0x22, MOD_ALT = 0x44, MOD_SUPER = 0x88 };
enum {
  KEY_ENTER = 0x28,
  KEY_ESCAPE = 0x29,
  KEY_BACKSPACE = 0x2a,
  KEY_TAB = 0x2b,
  KEY_SPACE = 0x2c,
  KEY_F1 = 0x3a,
  KEY_F2 = 0x3b,
  KEY_F4 = 0x3d,
  KEY_F5 = 0x3e,
  KEY_HOME = 0x4a,
  KEY_PAGEUP = 0x4b,
  KEY_INSERT = 0x49,
  KEY_DELETE = 0x4c,
  KEY_END = 0x4d,
  KEY_PAGEDOWN = 0x4e,
  KEY_RIGHT = 0x4f,
  KEY_LEFT = 0x50,
  KEY_DOWN = 0x51,
  KEY_UP = 0x52
};

typedef struct {
  unsigned code, ascii, mods;
  bool down, repeat;
} Key;

typedef struct {
  Rect rect, sample;
  uint64_t key;
  uint32_t *pixels;
  size_t bytes;
  bool pending;
} GlassCache;

typedef struct Window {
  bool alive, minimized, maximized, fullscreen;
  App app;
  int id, workspace, z;
  Rect box, restore, fullscreen_restore;
  bool fullscreen_was_maximized;
  bool snapped;
  int snap_layout, snap_slot;
  char title[OS_NAME_MAX + 48];
  void *state;
  struct Window *owner;
  int owner_id;

  uint32_t *surface;
  int surface_width, surface_height;
  int surface_stride, surface_rows;
  Rect surface_damage;
  uint64_t surface_revision;
  uint64_t surface_tiles[SURFACE_GRID * SURFACE_GRID];
  GlassCache glass[2];
  bool surface_focused, surface_maximized, surface_pointer, surface_fullscreen;

} Window;

typedef struct {
  const char *name;
  Icon icon;
  int (*open)(Window *, const char *);
  void (*draw)(Window *);
  bool (*mouse)(Window *, int, int, unsigned, int);
  void (*key)(Window *, Key);
  void (*tick)(Window *);
  bool (*close)(Window *);
  bool (*can_close)(Window *);

  void (*toolbar)(Window *);
  bool (*toolbar_mouse)(Window *, int, int, unsigned);
  Rect (*toolbar_hover)(Window *, int, int);
} AppOps;

typedef struct {
  const char *label;
  Icon icon;
  int action;
  bool enabled;
} MenuItem;

typedef void (*MenuAction)(Window *, int);
typedef void (*DialogAction)(Window *, const char *);
typedef struct {
  bool open;
  Rect box;
  Window *owner;
  MenuAction action;
  int count, hover, focus, first, visible, origin_group;
  int scroll;
  bool palette, color_row, color_keyboard;
  MenuItem color_items[8];
  unsigned palette_selected;
  uint32_t palette_colors[8];
  SmoothScroll animation;
  MenuItem items[MAX_MENU_ITEMS];
} Menu;

typedef struct {
  bool open, input, secret;
  Window *owner;
  DialogAction action, cancel_action;
  Rect box;
  char title[80], message[1600], value[OS_PATH_MAX];
  int cursor, anchor, focus, message_scroll, message_height;
  bool replace;
} Dialog;

typedef struct {
  uint32_t *scene, *output, *wallpaper;
  bool drag_moved, drag_started;
  bool canvas_present;
  uint64_t drag_motion_at;

  uint32_t *paint;
  int paint_stride, paint_x, paint_y;
  bool paint_alpha, glass_disabled;
  uint64_t material_epoch, glass_updates, glass_pixels, glass_time_us,
      glass_fallbacks;

  int width, height, scale, scale_index, workspace, workspace_count, focused, z,
      next_id;

  uint32_t accent, selection, accent_text;
  int accent_index, wallpaper_index, wallpaper_mode, wallpaper_background;
  int ui_motion, glass_style, launcher_rows, desktop_icon_size;
  bool desktop_icons, snap_assist_enabled, snap_hover_enabled, snap_bar_enabled;
  bool files_default_details, files_default_hidden;
  char wallpaper_path[OS_PATH_MAX], wallpaper_status[128];
  int dock_size, dock_magnification;
  int profile_avatar;
  char profile_tagline[81];
  int terminal_palette, terminal_foreground, terminal_background;
  int terminal_opacity, terminal_text_size, terminal_shell;
  bool terminal_ansi;
  unsigned dock_pins;
  uint64_t dock_revision;
  int editor_tab_width, editor_text_size, editor_newline;
  bool editor_line_numbers, editor_spaces, editor_autoindent, editor_whitespace,
      editor_highlight;
  bool config_on_disk;

  bool running, trace, launcher,
      super_used, quit_pending;
  bool dark_mode, buffered_display;

  int mouse_x, mouse_y, old_mouse_x, old_mouse_y;
  Rect cursor_rect;
  unsigned cursor_kind;
  bool cursor_dirty;
  unsigned buttons, modifiers;
  int pointer_speed, touchpad_speed, wheel_steps;
  bool pointer_acceleration, primary_right, primary_right_active,
      natural_scroll, touchpad_natural;
  PointerMotion pointer_motion[256];
  unsigned pointer_buttons[256];
  struct os_pointer_inventory pointer_inventory;
  uint64_t pointer_info_at;

  Window windows[MAX_WINDOWS];

  Rect dirty[32], clip;
  int dirty_count;

  Menu menu;
  Dialog dialog;

  char launcher_query[80];
  int launcher_focus, launcher_cursor, launcher_anchor, launcher_first,
      launcher_visible;
  bool launcher_expanded;
  Rect launcher_box;

  Window *drag;
  int drag_mode, drag_start_x, drag_start_y;
  Rect drag_start;
  Window *snap_owner;
  int snap_owner_id, snap_layout, snap_slot;
  bool snap_open, snap_preview, snap_keyboard, snap_pointer_blocked, snap_bar, snap_assist;
  Rect snap_box, snap_target;
  uint64_t snap_hover_at;
  uint64_t last_click;
  int last_click_window, last_click_hit;

  uint64_t frames, presented_pixels, present_calls, full_frames;
  uint64_t compose_us, upload_us, drag_frames, drag_us, drag_max_us,
      surface_updates;
  uint64_t frame_at, drag_frame_at, drag_intervals, drag_interval_us,
      drag_interval_max;
  uint32_t drag_interval_histogram[128];

  uint64_t repeat_at, clock_at;
  Key held;
  char clock[16], date[32], home[OS_PATH_MAX], config[OS_PATH_MAX];

  char recent[MAX_RECENT][OS_PATH_MAX];
  int recent_count;
} Desktop;

extern Desktop desk;
int game_open(Window *, const char *);
void game_draw(Window *);
bool game_mouse(Window *, int, int, unsigned, int);
void game_key(Window *, Key);
void game_keyboard_source(const struct os_input_event *);
void game_keyboard_overflow(void);
void game_keyboard_publish(void);
void game_tick(Window *);
bool game_close(Window *);
void game_service_start(void);
void game_service_tick(void);
void game_service_shutdown(void);
void game_focus_changed(void);
bool game_pointer_captured(void);
bool game_pointer_event(const struct os_input_event *);

extern const AppOps apps[APP_COUNT];

int S(int value);
int clampi(int64_t value, int low, int high);
bool contains(Rect r, int x, int y);
Rect intersect(Rect a, Rect b);
bool empty(Rect r);

void *allocate(size_t bytes);
void deallocate(void *ptr, size_t bytes);

void copy_text(char *out, size_t size, const char *in);
void append_text(char *out, size_t size, const char *in);
void format_uint(char *out, uint64_t value);
void format_size(char *out, uint64_t value);
void format_date(char *out, int64_t epoch);

const char *basename(const char *path);
int join_path(char *out, const char *dir, const char *name);
int normalize_path(char *out, const char *base, const char *path);

uint64_t now_ms(void);
void trace(const char *event, Window *w, const char *detail);
void trace_scroll(Window *w, const char *app, int position, int target);

void invalidate(Rect r);
void invalidate_window(Window *w);
void invalidate_window_region(Window *w, Rect r);
void damage_window(Window *w);
void damage_native_resize(Window *, Rect);
void invalidate_all(void);
void desktop_verify_recompose(void);

void clip_set(Rect r);
void fill(Rect r, uint32_t color);
void blend(Rect r, uint32_t color, unsigned alpha);
enum { ICON_MAX = 384 };
unsigned corner_coverage(int radius, int x, int y);
unsigned rect_coverage(Rect r, int radius, int x, int y);
static inline uint32_t coverage_over(uint32_t foreground, uint32_t background,
                                     unsigned alpha) {
  if (!alpha)
    return background;
  if (alpha >= 255)
    return foreground;
  uint32_t out = 0;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    unsigned f = (foreground >> shift) & 255, b = (background >> shift) & 255;
    out |= ((f * alpha + b * (255 - alpha) + 127) / 255) << shift;
  }
  return out;
}
void rounded(Rect r, int radius, uint32_t color);
void border(Rect r, int radius, uint32_t color);
void line(int x1, int y1, int x2, int y2, uint32_t color);
void label(int x, int y, const char *text, Font font, uint32_t color,
           int max_width);
int text_width(const char *text, Font font);
int text_width_limit(const char *text, size_t bytes, Font font);
void icon(int x, int y, Icon kind, int size, uint32_t color);

void button(Rect r, const char *text, Icon kind, bool primary, bool enabled,
            bool focused);
void input_draw(Rect r, const char *value, const char *placeholder, int cursor,
                int anchor, bool focused);
bool input_key(char *value, size_t capacity, int *cursor, int *anchor, Key key);
void scrollbar(Rect viewport, int content_height, int scroll);

void wallpaper_generate(void);
extern const char *const wallpaper_names[WALLPAPER_COUNT];
extern const uint32_t wallpaper_backgrounds[3];
int wallpaper_set(int index, const char *path);
void wallpaper_prepare_previews(void);
void wallpaper_draw_preview(int index, Rect r);
void wallpaper_cleanup(void);

long desktop_present(void);
int desktop_main(int argc, char **argv);
void desktop_quit(void);
void power_review_tick(void);
void power_review_cancel(void);
bool power_review_approved(Window *w);
bool power_review_accept(Window *w);
bool power_review_pending(Window *w);

Window *wm_open(App app, const char *path);
void wm_focus(Window *w);
void wm_minimize(Window *w);
void wm_maximize(Window *w);
void wm_close(Window *w);
void wm_snap(Window *w, bool right);
Rect snap_geometry(int layout, int slot);
void snap_apply(Window *w, int layout, int slot);
void snap_show(Window *w);
void snap_cancel(void);
void snap_tick(void);
bool snap_mouse(int x, int y, unsigned pressed);
bool snap_key(Key k);
void snap_drag(Window *w, int x, int y, bool finished);
void snap_draw(void);
bool snap_window_hidden(Window *w);
void window_thumbnail(Window *w, Rect rect);
bool game_thumbnail(Window *w, Rect rect);
void transition_begin(Rect box);
void transition_tick(void);
bool transition_present(void);
void transition_restore(void);
void transition_cleanup(void);
void files_desktop_init(void);
void files_profile_reset(void);
void files_desktop_shutdown(void);
void files_desktop_tick(void);
void files_desktop_draw(void);
bool files_desktop_mouse(int x, int y, unsigned pressed);
void files_desktop_key(Key k);
void files_watch_tick(void);
Window *wm_focused(void);
bool wm_pointer_active(Window *w);
Window *wm_hit_test(int x, int y);
int wm_resize_edges(Window *w, int x, int y);
void workspace_switch(int workspace);
void window_move_workspace(Window *w, int workspace);
void wm_draw(Window *w);
void wm_paint(Window *w);
void wm_free_surface(Window *w);
uint32_t *paint_pixel(int x, int y);
void window_shadow(Window *w, int radius);
Rect files_sidebar_rect(Window *w);
Rect disks_sidebar_rect(Window *w);
Rect taskbar_dock_rect(void);
Rect taskbar_dock_bounds(void);
Rect taskbar_dock_cache_rect(void);
int desktop_bottom(void);
void taskbar_tick(void);
void taskbar_cleanup(void);
void taskbar_preferences_changed(void);
int preferences_save(Window *owner);
void dock_draw(void);
Rect dock_hover(int x, int y);
bool dock_mouse(int x, int y, unsigned pressed, int wheel);
void wm_workarea_changed(void);
void settings_open_dock(void);
void settings_open_terminal(void);
void wallpaper_tick(void);
bool wallpaper_busy(void);
bool wallpaper_preview_ready(int index);
bool wallpaper_preview_failed(int index);
void editor_preferences_open(Window *w);
Rect window_glass_rect(Window *w, int index);
void window_material(Window *w, Rect r, int index);
void glass_plan(GlassCache *cache, Rect rect, uint64_t key);
void glass_build(GlassCache *cache, int role);
void glass_release(GlassCache *cache);
void glass_draw(GlassCache *cache, Rect r, int radius);
uint32_t glass_over(uint32_t foreground, uint32_t background);
void compositor_damage(Rect r);
uint64_t glass_layer_key(Rect rect, const int *order, int count);
void glass_system_plan(const int *order, int count);
void glass_system_damage(void);
void glass_system_prepare(int index);
void glass_system_draw(int index, Rect rect, int radius);
void glass_cleanup(void);
void glass_reset(void);

void wm_mouse(int x, int y, unsigned pressed, unsigned released, int wheel);
bool wm_key(Key key);
void wm_actions(Window *w, int x, int y);
Rect window_body(Window *w);
void wm_fullscreen(Window *w, bool enabled);
bool wm_fullscreen_active(void);

void taskbar_draw(void);
bool taskbar_mouse(int x, int y, unsigned pressed, int wheel);
Rect taskbar_hover(int x, int y);
bool taskbar_key(Key key);

void launcher_draw(void);
void launcher_toggle(void);
void launcher_close(void);
Rect launcher_hover(int, int);
void launcher_pointer(int, int, bool);

void menu_open(Window *w, int x, int y, const MenuItem *items, int count,
               MenuAction action);
enum {
  MENU_FILE,
  MENU_EDIT,
  MENU_VIEW,
  MENU_GO,
  MENU_WINDOW,
  MENU_HELP,
  MENU_APP
};
enum {
  WINDOW_HEADER = 28,
  WINDOW_CORNER = 8,
  DESKTOP_TOP = 22,
  DESKTOP_BOTTOM = 54
};
Rect window_control(Window *w, int index);
int disks_open(Window *, const char *);
void disks_draw(Window *);
bool disks_mouse(Window *, int, int, unsigned, int);
void disks_key(Window *, Key);
bool disks_close(Window *);
void disks_menu(Window *, int, int, int);
void disks_toolbar(Window *);
bool disks_toolbar_mouse(Window *, int, int, unsigned);
Rect disks_toolbar_hover(Window *, int, int);
void menubar_draw(Window *w);
Rect menubar_hover(Window *w, int x, int y);
bool menubar_mouse(Window *w, int x, int y, unsigned pressed);
bool menubar_key(Key key);
void desktop_windows_menu(int x, int y);
void desktop_status_menu(int x, int y);
void desktop_help(Window *w);
void wm_arrange(bool tile);
void wm_trace_geometry(Window *w);
void files_menu(Window *w, int group, int x, int y);
void editor_menu(Window *w, int group, int x, int y);
void terminal_menu(Window *w, int group, int x, int y);
void settings_menu(Window *w, int group, int x, int y);
void power_menu(Window *w, int group, int x, int y);
void menu_send_key(Window *w, unsigned ascii, unsigned code, unsigned mods);
void input_material_draw(Rect, const char *, const char *, int, int, bool);
void menu_close(void);
void menu_palette(Window *, int, int, const MenuItem *, const uint32_t *, unsigned, MenuAction);
void menu_color_row(const MenuItem *, const uint32_t *, unsigned);
void menu_draw(void);
bool menu_mouse(int x, int y, unsigned pressed, int wheel);
void menu_tick(void);
bool menu_key(Key key);
void dialog_notice(Window *w, const char *title, const char *message);
void dialog_error(Window *w, const char *action, long error);
void dialog_input(Window *w, const char *title, const char *message,
                  const char *value, DialogAction action);
void dialog_password(Window *, const char *, const char *, DialogAction, DialogAction);
void dialog_confirm(Window *w, const char *title, const char *message,
                    DialogAction action);
void dialog_draw(void);
bool dialog_mouse(int x, int y, unsigned pressed, int wheel);
Rect dialog_hover(int x, int y);
void dialog_key(Key key);
void recent_add(const char *path);
unsigned desktop_file_scope(unsigned);
int desktop_create_file(const char *,unsigned);
int desktop_create_directory(const char *,unsigned);
void config_load(void);
int config_save(void);
void settings_apply(void);
int files_open(Window *, const char *);
void files_draw(Window *);
bool files_mouse(Window *, int, int, unsigned, int);
void files_key(Window *, Key);
void files_tick(Window *);
bool files_close(Window *);
void files_refresh_all(void);
void files_toolbar(Window *);
bool files_toolbar_mouse(Window *, int, int, unsigned);
Rect files_toolbar_hover(Window *, int, int);
Rect files_hover(Window *, int, int);
int terminal_open(Window *, const char *);
void terminal_draw(Window *);
Font terminal_font(void);
int terminal_line_height(void);
uint32_t terminal_foreground(void);
uint32_t terminal_background(void);
void terminal_preferences_changed(void);
void terminal_preferences_open(Window *);
bool terminal_is_preferences(Window *);
Rect terminal_hover(Window *, int, int);
Font terminal_font_size(int);
uint32_t terminal_color_choice(int, int, bool);
void avatar_draw(Rect, unsigned);
unsigned profiles_avatar(unsigned);
void profiles_avatar_changed(void);
void icon_cleanup(void);
bool settings_slider_pointer(int, int, unsigned);
bool terminal_mouse(Window *, int, int, unsigned, int);
void terminal_key(Window *, Key);
void terminal_tick(Window *);
bool terminal_close(Window *);
bool terminal_can_close(Window *);
void terminal_reap(void);
int codeoss_open(Window *,const char *);
void codeoss_draw(Window *);
bool codeoss_mouse(Window *,int,int,unsigned,int);
void codeoss_key(Window *,Key);
void codeoss_tick(Window *);
bool codeoss_close(Window *);
bool codeoss_can_close(Window *);
void codeoss_menu(Window *,int,int,int);
Rect codeoss_hover(Window *,int,int);
void codeoss_reap(void);
void codeoss_shutdown(void);
int settings_open(Window *, const char *);
void settings_draw(Window *);
bool settings_mouse(Window *, int, int, unsigned, int);
Rect settings_hover(Window *, int, int);
void settings_key(Window *, Key);
void settings_tick(Window *);
bool settings_close(Window *);
int editor_open(Window *, const char *);
void editor_draw(Window *);
bool editor_mouse(Window *, int, int, unsigned, int);
void editor_key(Window *, Key);
void editor_tick(Window *);
bool editor_close(Window *);
bool editor_confirm_close(Window *);
int power_open(Window *, const char *);
void power_draw(Window *);
bool power_mouse(Window *, int, int, unsigned, int);
Rect power_hover(Window *, int, int);
void power_key(Window *, Key);
void power_tick(Window *);
bool power_close(Window *);
/* One bounded guest clipboard, shared with foreground native applications. */
int clipboard_write(const char *, size_t);
int clipboard_read(char **, size_t *);
void clipboard_release(char *);
void clipboard_error(Window *, const char *, int);

void desktop_cursor_draw(void);
bool game_cursor_draw(void);
bool game_cursor_bounds(Rect *);
bool game_window_loading(Window *);
Rect desktop_cursor_bounds(void);
bool editor_text_hit(Window *, int, int);
bool files_text_hit(Window *, int, int);
bool codeoss_text_hit(Window *, int, int);
void pointer_preferences_changed(void);
void pointer_reset_defaults(void);
void pointer_input_reset(void);
void pointer_inventory_refresh(void);
void pointer_transform(struct os_input_event_v2 *);

/* Logical desktop profiles: preferences only, with shared files and
 * permissions. */
void profiles_load(const char *legacy);
unsigned profiles_count(void);
unsigned profiles_active(void);
const char *profiles_name(unsigned at);
const char *profiles_id(unsigned at);
const char *profiles_status(void);
const char *profiles_legacy(void);
int profiles_prepare(void);
int profiles_save_registry(void);
int profiles_switch(unsigned target, const char *password);
unsigned profiles_uid(unsigned);
int profiles_account(unsigned, struct os_account_request *);
void profiles_home_init(void);
int profiles_create(const char *name);
int profiles_rename(unsigned at, const char *name);
int preferences_write(const char *path, const char *content);
void settings_reload_profile(void);
Rect editor_hover(Window *w, int x, int y);

#endif
