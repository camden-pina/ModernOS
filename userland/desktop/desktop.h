#ifndef MODERNOS_DESKTOP_H
#define MODERNOS_DESKTOP_H
#include "theme.h"
#include <libc.h>
#include <stdbool.h>
#include "smooth_scroll.h"

#define MAX_WINDOWS 16
#define MAX_MENU_ITEMS 20
#define MAX_RECENT 48
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct {
    int x, y, w, h;
} Rect;

typedef enum { APP_FILES, APP_TERMINAL, APP_BROWSER, APP_SETTINGS, APP_EDITOR, APP_COUNT } App;
enum { WALLPAPER_ALPINE = 3, WALLPAPER_DESERT, WALLPAPER_CUSTOM, WALLPAPER_COUNT };
enum { WALLPAPER_FILL, WALLPAPER_FIT, WALLPAPER_STRETCH, WALLPAPER_CENTER };
typedef enum {
    FONT_SMALL,
    FONT_NORMAL,
    FONT_MEDIUM,
    FONT_BODY,
    FONT_BOLD,
    FONT_TITLE,
    FONT_MONO,
    FONT_EDIT
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
    I_COUNT
} Icon;

enum { MOD_CTRL = 0x11, MOD_SHIFT = 0x22, MOD_ALT = 0x44, MOD_SUPER = 0x88 };
enum {
    KEY_ENTER = 0x28,
    KEY_ESCAPE = 0x29,
    KEY_BACKSPACE = 0x2a,
    KEY_TAB = 0x2b,
    KEY_SPACE = 0x2c,
    KEY_F2 = 0x3b,
    KEY_F4 = 0x3d,
    KEY_F5 = 0x3e,
    KEY_HOME = 0x4a,
    KEY_PAGEUP = 0x4b,
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

typedef struct Window {
    bool alive, minimized, maximized;
    App app;
    int id, workspace, z;
    Rect box, restore;
    char title[OS_NAME_MAX + 48];
    void *state;

    uint32_t *surface;
    int surface_width, surface_height;
    Rect surface_damage;
    bool surface_focused, surface_maximized;

    uint8_t *shadow_mask;
    size_t shadow_bytes;
    int shadow_width, shadow_height, shadow_scale;
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
    int count, hover, focus, first, visible;
    int scroll;
    SmoothScroll animation;
    MenuItem items[MAX_MENU_ITEMS];
} Menu;

typedef struct {
    bool open, input;
    Window *owner;
    DialogAction action;
    Rect box;
    char title[80], message[640], value[OS_PATH_MAX];
    int cursor, focus;
    bool replace;
} Dialog;

typedef struct {
    uint32_t *scene, *output, *wallpaper;
    bool drag_moved;
    bool canvas_present;
    uint64_t drag_motion_at;

    uint32_t *paint;
    int paint_stride, paint_x, paint_y;

    int width, height, scale, scale_index, workspace, workspace_count, focused, z, next_id;

    uint32_t accent, selection, accent_text;
    int accent_index, wallpaper_index, wallpaper_mode, wallpaper_background;
    char wallpaper_path[OS_PATH_MAX], wallpaper_status[128];

    bool running, trace, browser_network_trace, browser_exception_trace, launcher, super_used,
        quit_pending;
    bool dark_mode, buffered_display;

    int mouse_x, mouse_y, old_mouse_x, old_mouse_y;
    unsigned buttons, modifiers;

    Window windows[MAX_WINDOWS];

    Rect dirty[32], clip;
    int dirty_count;

    Menu menu;
    Dialog dialog;

    char launcher_query[80];
    int launcher_focus;
    Rect launcher_box;

    Window *drag;
    int drag_mode, drag_start_x, drag_start_y;
    Rect drag_start;
    uint64_t last_click;
    int last_click_window, last_click_hit;

    uint64_t frames, presented_pixels, present_calls, full_frames;
    uint64_t compose_us, upload_us, drag_frames, drag_us, drag_max_us, surface_updates;
    uint64_t frame_at, drag_frame_at, drag_intervals, drag_interval_us, drag_interval_max;
    uint32_t drag_interval_histogram[128];

    uint64_t repeat_at, clock_at;
    Key held;
    char clock[16], date[32], home[OS_PATH_MAX], config[OS_PATH_MAX];

    char recent[MAX_RECENT][OS_PATH_MAX];
    int recent_count;
} Desktop;

extern Desktop desk;
extern const AppOps apps[APP_COUNT];

int S(int value);
int clampi(int value, int low, int high);
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
void damage_window(Window *w);
void invalidate_all(void);

void clip_set(Rect r);
void fill(Rect r, uint32_t color);
void blend(Rect r, uint32_t color, unsigned alpha);
void rounded(Rect r, int radius, uint32_t color);
void border(Rect r, int radius, uint32_t color);
void line(int x1, int y1, int x2, int y2, uint32_t color);
void label(int x, int y, const char *text, Font font, uint32_t color, int max_width);
int text_width(const char *text, Font font);
void icon(int x, int y, Icon kind, int size, uint32_t color);

void button(Rect r, const char *text, Icon kind, bool primary, bool enabled, bool focused);
void input_draw(Rect r, const char *value, const char *placeholder, int cursor, bool focused);
bool input_key(char *value, size_t capacity, int *cursor, Key key);
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

Window *wm_open(App app, const char *path);
void wm_focus(Window *w);
void wm_minimize(Window *w);
void wm_maximize(Window *w);
void wm_close(Window *w);
void wm_snap(Window *w, bool right);
Window *wm_focused(void);
void workspace_switch(int workspace);
void window_move_workspace(Window *w, int workspace);
void wm_draw(Window *w);
void wm_paint(Window *w);
void wm_free_surface(Window *w);
uint32_t *paint_pixel(int x, int y);
void window_shadow(Window *w);
void wm_mouse(int x, int y, unsigned pressed, unsigned released, int wheel);
bool wm_key(Key key);
void wm_actions(Window *w, int x, int y);
Rect window_body(Window *w);

void taskbar_draw(void);
bool taskbar_mouse(int x, int y, unsigned pressed);
Rect taskbar_hover(int x, int y);
bool taskbar_key(Key key);

void launcher_draw(void);
void launcher_toggle(void);
void launcher_close(void);

void menu_open(Window *w, int x, int y, const MenuItem *items, int count, MenuAction action);
void menu_close(void);
void menu_draw(void);
bool menu_mouse(int x, int y, unsigned pressed, int wheel);
void menu_tick(void);
bool menu_key(Key key);
void dialog_notice(Window *w, const char *title, const char *message);
void dialog_error(Window *w, const char *action, long error);
void dialog_input(Window *w, const char *title, const char *message, const char *value,
                  DialogAction action);
void dialog_confirm(Window *w, const char *title, const char *message, DialogAction action);
void dialog_draw(void);
bool dialog_mouse(int x, int y, unsigned pressed);
Rect dialog_hover(int x, int y);
void dialog_key(Key key);
void recent_add(const char *path);
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
bool terminal_mouse(Window *, int, int, unsigned, int);
void terminal_key(Window *, Key);
void terminal_tick(Window *);
bool terminal_close(Window *);
bool terminal_can_close(Window *);
void terminal_reap(void);
int browser_open(Window *, const char *);
void browser_draw(Window *);
bool browser_mouse(Window *, int, int, unsigned, int);
void browser_key(Window *, Key);
void browser_tick(Window *);
bool browser_close(Window *);
void browser_toolbar(Window *);
bool browser_toolbar_mouse(Window *, int, int, unsigned);
Rect browser_toolbar_hover(Window *, int, int);
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
#endif
