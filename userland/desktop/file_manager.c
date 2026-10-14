#include "desktop.h"
#include "smooth_scroll.h"

#define ENTRY_LIMIT 2048
#define CLIPBOARD_LIMIT 128
#define COPY_DEPTH 16

typedef struct {
    char name[OS_NAME_MAX + 1], path[OS_PATH_MAX];
    struct os_stat st;
    int error;
    bool selected, link;
} Entry;

typedef struct {
    char source[OS_PATH_MAX], target[OS_PATH_MAX];
    int fd;
    unsigned mode;
    uint8_t buffer[4096];
    int offset, used;
} CopyLevel;

typedef struct {
    bool active, cut;
    int item, count, depth, input, output;
    unsigned mode;
    uint64_t bytes, last_update;
    char source[CLIPBOARD_LIMIT][OS_PATH_MAX], destination[OS_PATH_MAX], target[OS_PATH_MAX],
        remove_source[OS_PATH_MAX];
    CopyLevel levels[COPY_DEPTH];
    uint8_t chunk[32768];
} CopyJob;

typedef struct {
    bool active, trash;
    int fd, root, count, offset, used;
    char paths[64][OS_PATH_MAX];
    uint8_t buffer[4096];
    uint64_t last_update;
} DirectoryJob;

typedef struct {
    char directory[OS_PATH_MAX], history[32][OS_PATH_MAX], path_input[OS_PATH_MAX], query[96],
        status[256];
    int history_count, history_position, mode, input_focus, cursor, sort, scroll, focus, anchor,
        visible_count, folders, files, count, keyboard_focus;
    bool descending, limited, show_hidden, replace_input;
    int order[ENTRY_LIMIT];
    Entry entries[ENTRY_LIMIT];
    Rect viewport;
    int content_height, columns;
    SmoothScroll animation;
    int drag_entry, drag_x, drag_y;
    bool dragging;
    CopyJob job;
    DirectoryJob listing;
} Files;

static struct {
    int count;
    bool cut;
    char paths[CLIPBOARD_LIMIT][OS_PATH_MAX];
} clipboard;

enum {
    A_OPEN,
    A_COPY,
    A_CUT,
    A_PASTE,
    A_RENAME,
    A_TRASH,
    A_FOLDER,
    A_FILE,
    A_REFRESH,
    A_INFO,
    A_PARENT,
    A_RESTORE,
    A_HIDDEN,
    A_WALLPAPER
};

static bool navigate(Window *w, const char *path, bool history);
static void action(Window *w, int operation);
static void directory_step(Window *w);

static bool same_text(const char *a, const char *b) {
    return !strcmp(a, b);
}

static bool match(const char *name, const char *query) {
    if (!query[0]) {
        return true;
    }
    for (int i = 0; name[i]; i++) {
        int j = 0;
        while (query[j] && name[i + j]) {
            char a = name[i + j], b = query[j];
            if (a >= 'A' && a <= 'Z') {
                a += 32;
            }
            if (b >= 'A' && b <= 'Z') {
                b += 32;
            }
            if (a != b) {
                break;
            }
            j++;
        }

        if (!query[j]) {
            return true;
        }
    }

    return false;
}

static int compare(Files *f, int a, int b) {
    Entry *x = &f->entries[a], *y = &f->entries[b];
    bool xd = (x->st.mode & 0170000) == S_IFDIR, yd = (y->st.mode & 0170000) == S_IFDIR;
    if (xd != yd) {
        return xd ? -1 : 1;
    }
    int result = 0;
    if (f->sort == 1) {
        result = x->st.mtime_sec < y->st.mtime_sec ? -1 : x->st.mtime_sec > y->st.mtime_sec ? 1 : 0;
    } else if (f->sort == 2) {
        result = x->st.size < y->st.size ? -1 : x->st.size > y->st.size ? 1 : 0;
    }
    if (!result) {
        result = strcmp(x->name, y->name);
    }
    return f->descending ? -result : result;
}

static void filter(Files *f) {
    f->visible_count = f->folders = f->files = 0;
    for (int i = 0; i < f->count; i++) {
        Entry *e = &f->entries[i];
        if ((!f->show_hidden && e->name[0] == '.') || !match(e->name, f->query)) {
            e->selected = false;
            continue;
        }
        f->order[f->visible_count++] = i;
        if ((e->st.mode & 0170000) == S_IFDIR) {
            f->folders++;
        } else {
            f->files++;
        }
    }

    /* Iterative merge sort keeps large folders O(n log n). */
    int temp[ENTRY_LIMIT];
    for (int width = 1; width < f->visible_count; width *= 2) {
        for (int first = 0; first < f->visible_count; first += width * 2) {
            int mid = MIN(first + width, f->visible_count),
                end = MIN(first + width * 2, f->visible_count), a = first, b = mid;
            for (int k = first; k < end; k++) {
                temp[k] = (a < mid && (b == end || compare(f, f->order[a], f->order[b]) <= 0))
                              ? f->order[a++]
                              : f->order[b++];
            }
        }

        memcpy(f->order, temp, f->visible_count * sizeof(int));
    }

    f->focus = clampi(f->focus, 0, MAX(0, f->visible_count - 1));
    f->anchor = clampi(f->anchor, 0, MAX(0, f->visible_count - 1));
}

static void add_entry(Files *f, const char *path, const char *name, bool link) {
    if (f->count == ENTRY_LIMIT) {
        f->limited = true;
        return;
    }
    Entry *e = &f->entries[f->count++];
    memset(e, 0, sizeof(*e));
    e->link = link;
    copy_text(e->path, sizeof(e->path), path);
    copy_text(e->name, sizeof(e->name), name);
    e->error = stat(path, &e->st);
}

static int mount_list(char roots[64][OS_PATH_MAX]) {
    char buffer[8192];
    long n = call3(SYS_sysinfo, INFO_MOUNTS, buffer, sizeof(buffer));
    if (n < 0) {
        return n;
    }
    buffer[MIN(n, (long)sizeof(buffer) - 1)] = 0;
    int count = 0;
    char *p = buffer;
    while (*p && count < 64) {
        char *first = p;
        while (*p && *p != ' ' && *p != '\n') {
            p++;
        }
        char save = *p;
        *p = 0;
        copy_text(roots[count++], OS_PATH_MAX, first);
        *p = save;
        while (*p && *p != '\n') {
            p++;
        }
        if (*p) {
            p++;
        }
    }

    return count;
}

static int trash_root(const char *source, char *out) {
    struct os_stat st;
    int e = stat(source, &st);
    if (e) {
        return e;
    }
    if (!st.dev) {
        return join_path(out, desk.home, ".Trash");
    }
    char parent[OS_PATH_MAX];
    copy_text(parent, sizeof(parent), source);
    while (strlen(parent) > 1) {
        char next[OS_PATH_MAX];
        normalize_path(next, parent, "..");
        struct os_stat ps;
        e = stat(next, &ps);
        if (e) {
            return e;
        }
        if (ps.dev != st.dev) {
            break;
        }
        copy_text(parent, sizeof(parent), next);
    }

    return join_path(out, parent, ".Trash");
}

static void refresh(Window *w) {
    Files *f = w->state;
    DirectoryJob *j = &f->listing;
    if (j->active && j->fd >= 0) {
        close(j->fd);
    }
    memset(j, 0, sizeof(*j));
    j->fd = -1;
    f->count = 0;
    f->limited = false;
    f->scroll = 0;
    smooth_jump(&f->animation, 0, now_ms());
    f->focus = f->anchor = 0;
    if (same_text(f->directory, "@Recent")) {
        for (int i = 0; i < desk.recent_count; i++) {
            struct os_stat st;
            if (!stat(desk.recent[i], &st)) {
                add_entry(f, desk.recent[i], basename(desk.recent[i]), false);
            }
        }

        filter(f);
        copy_text(f->status, sizeof(f->status), "Recent files opened in this desktop session.");
    } else if (same_text(f->directory, "@Trash")) {
        j->trash = true;
        join_path(j->paths[j->count++], desk.home, ".Trash");
        char (*roots)[OS_PATH_MAX] = allocate(64 * OS_PATH_MAX);
        if (roots) {
            int count = mount_list(roots);
            for (int i = 0; i < count && j->count < 64; i++) {
                if (strcmp(roots[i], "/")) {
                    if (!join_path(j->paths[j->count], roots[i], ".Trash")) {
                        j->count++;
                    }
                }
            }

            deallocate(roots, 64 * OS_PATH_MAX);
        } else {
            dialog_error(w, "Read Trash", -E_NOMEM);
        }
        j->active = true;
    } else {
        copy_text(j->paths[0], OS_PATH_MAX, f->directory);
        j->count = 1;
        j->active = true;
    }
    if (j->active) {
        copy_text(f->status, sizeof(f->status), "Reading directory...");
        directory_step(w);
    }
    invalidate_window(w);
    trace("FILES_REFRESH", w, f->directory);
}

static void directory_step(Window *w) {
    Files *f = w->state;
    DirectoryJob *j = &f->listing;
    if (!j->active) {
        return;
    }
    int error = 0, previous_focus = f->visible_count ? f->order[f->focus] : -1;
    for (int processed = 0; processed < 16 && j->active; processed++) {
        if (j->fd < 0) {
            if (j->root == j->count) {
                j->active = false;
                break;
            }
            j->fd = open(j->paths[j->root++], O_RDONLY | O_DIRECTORY, 0);
            j->offset = j->used = 0;
            if (j->fd < 0) {
                if (j->trash && j->fd == -E_NOENT) {
                    continue;
                }
                error = j->fd;
                break;
            }
        }

        if (j->offset == j->used) {
            long n = call3(SYS_getdents64, j->fd, j->buffer, sizeof(j->buffer));
            if (n < 0) {
                error = n;
                break;
            }
            j->offset = 0;
            j->used = n;
            if (!n) {
                close(j->fd);
                j->fd = -1;
                continue;
            }
        }

        struct os_dirent *d = (void *)(j->buffer + j->offset);
        if (d->reclen < 24 || d->reclen > j->used - j->offset) {
            error = -E_IO;
            break;
        }
        j->offset += d->reclen;
        if (same_text(d->name, ".") || same_text(d->name, "..") ||
            (j->trash && same_text(d->name, ".info"))) {
            continue;
        }
        char path[OS_PATH_MAX];
        error = join_path(path, j->paths[j->root - 1], d->name);
        if (error) {
            break;
        }
        add_entry(f, path, d->name, d->type == 10);
        if (f->limited) {
            j->active = false;
        }
    }

    if (error) {
        j->active = false;
    }
    if (!j->active && j->fd >= 0) {
        close(j->fd);
        j->fd = -1;
    }
    filter(f);
    if (previous_focus >= 0) {
        for (int i = 0; i < f->visible_count; i++) {
            if (f->order[i] == previous_focus) {
                f->focus = i;
                break;
            }
        }
    }

    if (error) {
        copy_text(f->status, sizeof(f->status), error_text(error));
        dialog_error(w, "Read folder", error);
    } else if (!j->active) {
        copy_text(f->status, sizeof(f->status),
                  f->query[0] ? "Search scope: names in this folder only."
                  : f->limited
                      ? "Showing first 2048 entries. Use Terminal for the complete listing."
                      : "");
    }

    uint64_t now = now_ms();
    if (!j->active || now - j->last_update >= 50) {
        j->last_update = now;
        invalidate_window(w);
    }
}

void files_refresh_all(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (desk.windows[i].alive && desk.windows[i].app == APP_FILES) {
            refresh(&desk.windows[i]);
        }
    }
}

int files_open(Window *w, const char *path) {
    Files *f = allocate(sizeof(*f));
    if (!f) {
        return -E_NOMEM;
    }
    memset(f, 0, sizeof(*f));
    w->state = f;
    f->drag_entry = -1;
    char projects[OS_PATH_MAX];
    join_path(projects, desk.home, "Projects");
    struct os_stat st;
    const char *dir = path                                                     ? path
                      : !stat(projects, &st) && (st.mode & 0170000) == S_IFDIR ? projects
                                                                               : desk.home;
    copy_text(f->directory, sizeof(f->directory), dir);
    copy_text(f->history[0], OS_PATH_MAX, dir);
    f->history_count = 1;
    refresh(w);
    return 0;
}

static bool navigate(Window *w, const char *path, bool history) {
    Files *f = w->state;
    if (f->job.active) {
        dialog_notice(w, "File operation in progress",
                      "Wait for the copy or move to finish, or press Escape to cancel it before "
                      "changing folders.");
        return false;
    }

    char normalized[OS_PATH_MAX];
    int e =
        path[0] == '@'
            ? (copy_text(normalized, sizeof(normalized), path), 0)
            : normalize_path(normalized, f->directory[0] == '@' ? desk.home : f->directory, path);
    if (!e && normalized[0] != '@') {
        struct os_stat st;
        e = stat(normalized, &st);
        if (!e && (st.mode & 0170000) != S_IFDIR) {
            e = -E_NOTDIR;
        }
    }

    if (e) {
        dialog_error(w, "Open folder", e);
        return false;
    }
    if (history && !same_text(normalized, f->directory)) {
        f->history_count = f->history_position + 1;
        if (f->history_count == 32) {
            memmove(f->history, f->history + 1, 31 * OS_PATH_MAX);
            f->history_count = 31;
        }
        copy_text(f->history[f->history_count++], OS_PATH_MAX, normalized);
        f->history_position = f->history_count - 1;
    }

    copy_text(f->directory, sizeof(f->directory), normalized);
    f->query[0] = 0;
    f->input_focus = 0;
    refresh(w);
    trace("NAVIGATE", w, normalized);
    return true;
}

static void history_step(Window *w, int direction) {
    Files *f = w->state;
    int next = f->history_position + direction;
    if (next >= 0 && next < f->history_count && !f->job.active) {
        if (navigate(w, f->history[next], false)) {
            f->history_position = next;
        }
    }
}

static const char *directory_name(Files *f) {
    if (same_text(f->directory, "@Recent")) {
        return "Recent";
    }
    if (same_text(f->directory, "@Trash")) {
        return "Trash";
    }
    if (same_text(f->directory, desk.home)) {
        return "Home";
    }
    if (same_text(f->directory, "/")) {
        return "System";
    }
    return basename(f->directory);
}

static int sidebar_width(Window *w) {
    return w->box.w < S(740) ? S(145) : S(185);
}

static Rect main_area(Window *w) {
    Rect b = window_body(w);
    int sidebar = sidebar_width(w);

    return (Rect){b.x + sidebar, b.y, b.w - sidebar, b.h};
}

static Rect content_view(Window *w) {
    Rect m = main_area(w);

    return (Rect){m.x + S(26), m.y + S(71), MAX(1, m.w - S(52)), MAX(1, m.h - S(96))};
}

static Rect entry_box(Window *w, int position) {
    Files *f = w->state;
    Rect v = content_view(w);
    int cols = v.w < S(480) ? 1 : 2;
    if (f->mode || position >= f->folders) {
        int offset = f->mode ? 0
                             : S(30) + (f->folders + cols - 1) / cols * S(61) +
                                   (f->folders ? S(25) : 0) + S(30);
        int row = f->mode ? position : position - f->folders;

        return (Rect){v.x, v.y + offset + S(28) + row * S(36) - f->scroll, v.w, S(36)};
    }

    int width = (v.w - S(8) * (cols - 1)) / cols;

    return (Rect){v.x + (position % cols) * (width + S(8)),
                  v.y + S(30) + (position / cols) * S(61) - f->scroll, width, S(53)};
}

static int content_height(Window *w) {
    Files *f = w->state;
    Rect v = content_view(w);
    int cols = v.w < S(480) ? 1 : 2;
    return f->mode ? S(28) + f->visible_count * S(36)
                   : S(30) + (f->folders + cols - 1) / cols * S(61) + (f->folders ? S(25) : 0) +
                         S(30) + S(28) + f->files * S(36);
}

static void number_label(int x, int y, int value, Font font, uint32_t color) {
    char n[32];
    format_uint(n, value);
    label(x, y, n, font, color, S(70));
}

void files_toolbar(Window *w) {
    Files *f = w->state;
    Rect b = w->box;
    fill((Rect){b.x + S(89), b.y + S(10), 1, S(19)}, theme_color(0xDFE5E4));
    for (int i = 0; i < 2; i++) {
        Rect r = {b.x + S(98 + i * 25), b.y + S(8), S(24), S(25)};
        bool enabled = i ? f->history_position + 1 < f->history_count : f->history_position > 0;
        if (enabled && contains(r, desk.mouse_x, desk.mouse_y)) {
            rounded(r, S(4), theme_color(0xEAF0ED));
        }
        icon(r.x + S(5), r.y + S(6), i ? I_FORWARD : I_BACK, S(14),
             enabled ? theme_color(0x69787E) : theme_color(0xC3CCCD));
    }

    int left = b.x + S(158), right = b.x + b.w - S(207), available = MAX(1, right - left);
    if (f->input_focus == 1) {
        input_draw((Rect){left, b.y + S(7), available, S(26)}, f->path_input, "Path",
                   f->replace_input ? -1 : f->cursor, true);
    } else {
        Rect saved = desk.clip;
        clip_set(intersect(saved, (Rect){left, b.y, available, S(39)}));
        char pieces[24][OS_NAME_MAX + 1];
        int n = 0;
        const char *p = f->directory;
        if (p[0] == '@') {
            copy_text(pieces[n++], sizeof(pieces[0]), directory_name(f));
        } else {
            if (!strncmp(p, desk.home, strlen(desk.home)) &&
                (p[strlen(desk.home)] == '/' || !p[strlen(desk.home)])) {
                copy_text(pieces[n++], sizeof(pieces[0]), "Home");
                p += strlen(desk.home);
            } else {
                copy_text(pieces[n++], sizeof(pieces[0]), "System");
            }
            while (*p && n < 24) {
                while (*p == '/') {
                    p++;
                }
                int len = 0;
                while (*p && *p != '/') {
                    if (len < OS_NAME_MAX) {
                        pieces[n][len++] = *p;
                    }
                    p++;
                }

                if (len) {
                    pieces[n][len] = 0;
                    n++;
                }
            }
        }

        int total = 0, start = 0;
        for (int i = 0; i < n; i++) {
            total += text_width(pieces[i], FONT_NORMAL) + (i ? S(22) : 0);
        }
        while (total > available && start < n - 1) {
            total -= text_width(pieces[start], FONT_NORMAL) + S(22);
            start++;
        }
        int x = left;
        if (start) {
            label(x, b.y + S(13), "..", FONT_NORMAL, theme_color(0x919EA2), S(14));
            x += S(20);
        }
        for (int i = start; i < n; i++) {
            int tw = MIN(text_width(pieces[i], i == n - 1 ? FONT_MEDIUM : FONT_NORMAL),
                         MAX(1, right - x));
            label(x, b.y + S(13), pieces[i], i == n - 1 ? FONT_MEDIUM : FONT_NORMAL,
                  i == n - 1 ? theme_color(0x41525A) : theme_color(0x919EA2), tw);
            x += tw;
            if (i < n - 1) {
                icon(x + S(6), b.y + S(14), I_FORWARD, S(11), theme_color(0xAAB4B5));
                x += S(22);
            }
        }

        desk.clip = saved;
    }

    Rect search = {b.x + b.w - S(192), b.y + S(7), S(99), S(26)};
    input_draw(search, f->query, f->input_focus == 2 ? "This folder" : "Search",
               f->input_focus == 2 ? (f->replace_input ? -1 : f->cursor) : 0, f->input_focus == 2);
    if (!f->query[0] && f->input_focus != 2) {
        icon(search.x + S(7), search.y + S(7), I_SEARCH, S(13), theme_color(0x9BA5A8));
        fill((Rect){search.x + S(7), search.y + S(6), S(57), S(15)}, THEME_WINDOW);
        icon(search.x + S(7), search.y + S(7), I_SEARCH, S(13), theme_color(0x9BA5A8));
        label(search.x + S(26), search.y + S(6), "Search", FONT_NORMAL, theme_color(0x9BA5A8),
              S(65));
    }
}

Rect files_toolbar_hover(Window *w, int x, int y) {
    Rect b = w->box;
    for (int i = 0; i < 2; i++) {
        Rect r = {b.x + S(98 + i * 25), b.y + S(8), S(24), S(25)};
        if (contains(r, x, y))
            return r;
    }

    return (Rect){0};
}

bool files_toolbar_mouse(Window *w, int x, int y, unsigned pressed) {
    Files *f = w->state;
    Rect b = w->box;
    if (!(pressed & 1)) {
        return false;
    }
    if (contains((Rect){b.x + S(98), b.y + S(8), S(24), S(25)}, x, y)) {
        history_step(w, -1);
        return true;
    }
    if (contains((Rect){b.x + S(123), b.y + S(8), S(24), S(25)}, x, y)) {
        history_step(w, 1);
        return true;
    }
    if (contains((Rect){b.x + b.w - S(192), b.y + S(7), S(99), S(26)}, x, y)) {
        f->input_focus = 2;
        f->cursor = strlen(f->query);
        copy_text(f->status, sizeof(f->status), "Search scope: names in this folder only.");
        invalidate_window(w);
        return true;
    }

    int left = b.x + S(158), right = b.x + b.w - S(207);
    if (x >= left && x < right && f->directory[0] != '@') {
        if (f->input_focus == 1) {
            f->cursor = strlen(f->path_input);
            invalidate_window(w);
            return true;
        }
        /* Breadcrumb hit widths mirror the rendered, truncated suffix. */
        char paths[24][OS_PATH_MAX], names[24][OS_NAME_MAX + 1];
        int n = 0;
        const char *p = f->directory;
        char current[OS_PATH_MAX] = "/";
        if (!strncmp(p, desk.home, strlen(desk.home)) &&
            (p[strlen(desk.home)] == '/' || !p[strlen(desk.home)])) {
            copy_text(current, sizeof(current), desk.home);
            copy_text(names[n], sizeof(names[n]), "Home");
            p += strlen(desk.home);
        } else {
            copy_text(names[n], sizeof(names[n]), "System");
        }
        copy_text(paths[n++], OS_PATH_MAX, current);
        while (*p && n < 24) {
            while (*p == '/') {
                p++;
            }
            int len = 0;
            while (*p && *p != '/') {
                if (len < OS_NAME_MAX) {
                    names[n][len++] = *p;
                }
                p++;
            }

            if (len) {
                names[n][len] = 0;
                char next[OS_PATH_MAX];
                if (join_path(next, current, names[n])) {
                    break;
                }
                copy_text(current, sizeof(current), next);
                copy_text(paths[n++], OS_PATH_MAX, current);
            }
        }

        int total = 0, start = 0;
        for (int i = 0; i < n; i++) {
            total += text_width(names[i], FONT_NORMAL) + (i ? S(22) : 0);
        }
        while (total > right - left && start < n - 1) {
            total -= text_width(names[start], FONT_NORMAL) + S(22);
            start++;
        }
        int xx = left + (start ? S(20) : 0);
        for (int i = start; i < n; i++) {
            int tw = MIN(text_width(names[i], i == n - 1 ? FONT_MEDIUM : FONT_NORMAL),
                         MAX(1, right - xx));
            if (x >= xx && x < xx + tw) {
                navigate(w, paths[i], true);
                return true;
            }
            xx += tw + S(22);
        }

        /* Empty toolbar space belongs to the shared window drag region. */
        return false;
    }

    return x >= b.x + S(89) && x < b.x + S(148);
}

static void sidebar(Window *w) {
    Files *f = w->state;
    Rect b = window_body(w);
    int sw = sidebar_width(w);
    fill((Rect){b.x, b.y, sw, b.h - S(8)}, THEME_SIDEBAR);
    rounded((Rect){b.x, b.y + b.h - S(20), sw, S(20)}, S(8), THEME_SIDEBAR);
    fill((Rect){b.x + S(9), b.y + b.h - S(20), sw - S(9), S(20)}, THEME_SIDEBAR);
    fill((Rect){b.x + sw - 1, b.y, 1, b.h}, theme_color(0xE5EAE8));
    label(b.x + S(18), b.y + S(19), "P L A C E S", FONT_SMALL, theme_color(0xA3AFB0), sw - S(28));
    const char *names[] = {"Home", "Recent", "Downloads", "Projects", "Pictures", "Trash"};
    Icon icons[] = {I_HOME, I_CLOCK, I_DOWNLOAD, I_FOLDER, I_IMAGE, I_TRASH};
    for (int i = 0; i < 6; i++) {
        char dest[OS_PATH_MAX];
        if (i == 0) {
            copy_text(dest, sizeof(dest), desk.home);
        } else if (i == 1) {
            copy_text(dest, sizeof(dest), "@Recent");
        } else if (i == 5) {
            copy_text(dest, sizeof(dest), "@Trash");
        } else {
            join_path(dest, desk.home, names[i]);
        }
        Rect r = {b.x + S(9), b.y + S(42 + i * 30), sw - S(18), S(30)};
        bool selected = same_text(dest, f->directory),
             hover = contains(r, desk.mouse_x, desk.mouse_y);
        if (selected || hover) {
            rounded(r, S(5), selected ? desk.selection : theme_color(0xEBF0ED));
        }
        if (f->keyboard_focus == 3 && i == f->anchor) {
            border(r, S(5), desk.accent);
        }
        uint32_t c = selected ? desk.accent_text : theme_color(0x617176);
        icon(r.x + S(9), r.y + S(7), icons[i], S(15), c);
        label(r.x + S(35), r.y + S(7), names[i], selected ? FONT_BOLD : FONT_BODY, c, r.w - S(44));
    }

    label(b.x + S(18), b.y + S(250), "D E V I C E S", FONT_SMALL, theme_color(0xA3AFB0),
          sw - S(28));
    Rect system = {b.x + S(9), b.y + S(273), sw - S(18), S(30)};
    if (same_text(f->directory, "/") || contains(system, desk.mouse_x, desk.mouse_y)) {
        rounded(system, S(5),
                same_text(f->directory, "/") ? desk.selection : theme_color(0xEBF0ED));
    }
    icon(system.x + S(9), system.y + S(7), I_DRIVE, S(15), theme_color(0x617176));
    label(system.x + S(35), system.y + S(7), "System", FONT_BODY, theme_color(0x617176),
          system.w - S(44));
    /* No statfs/free-space interface exists: hide utilization instead of
       inventing a disk capacity. Device details remain available in Terminal. */
}

static void modification(char *out, const Entry *e) {
    if (!e->st.mtime_sec) {
        copy_text(out, 32, "Unavailable");
        return;
    }
    struct os_timespec now;
    int result = call2(SYS_clock_gettime, 0, &now);
    if (!result && now.sec / 86400 == e->st.mtime_sec / 86400) {
        copy_text(out, 32, "Today");
        unsigned minutes = (uint64_t)e->st.mtime_sec / 60 % 1440;
        char time[8] = {',',
                        ' ',
                        (char)('0' + minutes / 60 / 10),
                        (char)('0' + minutes / 60 % 10),
                        ':',
                        (char)('0' + minutes % 60 / 10),
                        (char)('0' + minutes % 60 % 10),
                        0};
        append_text(out, 32, time);
    } else if (!result && now.sec / 86400 - e->st.mtime_sec / 86400 == 1) {
        copy_text(out, 32, "Yesterday");
    } else {
        format_date(out, e->st.mtime_sec);
    }
}

void files_draw(Window *w) {
    Files *f = w->state;
    Rect saved = desk.clip;
    sidebar(w);
    Rect m = main_area(w);
    clip_set(intersect(saved, m));
    fill(m, THEME_WINDOW);
    int x = m.x + S(26), y = m.y + S(23);
    label(x, y - S(4), directory_name(f), FONT_TITLE, THEME_PRIMARY_TEXT, m.w - S(190));
    int title_width = MIN(text_width(directory_name(f), FONT_TITLE), MAX(S(20), m.w - S(190)));
    char count[48];
    format_uint(count, f->visible_count);
    append_text(count, sizeof(count), f->visible_count == 1 ? " item" : " items");
    label(x + title_width + S(12), y + S(6), count, FONT_NORMAL, THEME_MUTED_TEXT,
          MAX(0, m.w - title_width - S(160)));
    Rect layout = {m.x + m.w - S(136), m.y + S(22), S(28), S(28)};
    if (contains(layout, desk.mouse_x, desk.mouse_y)) {
        rounded(layout, S(4), theme_color(0xF0F4F1));
    }
    icon(layout.x + S(6), layout.y + S(6), I_LAYOUT, S(15), theme_color(0x899B9C));
    if (f->keyboard_focus == 4) {
        border(layout, S(4), desk.accent);
    }
    Rect new_button = {m.x + m.w - S(97), m.y + S(22), S(71), S(28)};
    button(new_button, "New", I_PLUS, true, f->directory[0] != '@', f->keyboard_focus == 2);
    Rect v = content_view(w);
    f->viewport = v;
    f->content_height = content_height(w);
    f->scroll = clampi(f->scroll, 0, MAX(0, f->content_height - v.h));
    f->columns = v.w < S(480) ? 1 : 2;
    clip_set(intersect(saved, v));
    int files_top = f->mode ? v.y - f->scroll
                            : v.y + S(30) + (f->folders + f->columns - 1) / f->columns * S(61) +
                                  (f->folders ? S(25) : 0) - f->scroll;
    if (!f->mode) {
        label(v.x, v.y - f->scroll, "Folders", FONT_BOLD, theme_color(0x344449), v.w);
        number_label(v.x + S(52), v.y + S(2) - f->scroll, f->folders, FONT_SMALL,
                     theme_color(0xA7B0B1));
        label(v.x, files_top, "Files", FONT_BOLD, theme_color(0x344449), v.w);
        number_label(v.x + S(33), files_top + S(2), f->files, FONT_SMALL, theme_color(0xA7B0B1));
        files_top += S(30);
    }

    int modified_x = v.x + v.w - S(167), size_x = v.x + v.w - S(44);
    bool metadata = v.w > S(370);
    label(v.x + S(4), files_top + S(6), "Name", FONT_SMALL, theme_color(0xA4AFB0),
          MAX(S(60), v.w - S(metadata ? 185 : 50)));
    if (metadata) {
        label(modified_x, files_top + S(6), "Modified", FONT_SMALL, theme_color(0xA4AFB0), S(105));
    }
    label(size_x, files_top + S(6), "Size", FONT_SMALL, theme_color(0xA4AFB0), S(42));
    fill((Rect){v.x, files_top + S(27), v.w, 1}, theme_color(0xE9EDED));
    static const uint32_t bg[] = {0xE7EFF8, 0xE3F0E9, 0xEFEDFA, 0xFBF0E0},
                          fg[] = {0x6B92BC, 0x70A489, 0x9589BE, 0xCBA46D};
    for (int i = 0; i < f->visible_count; i++) {
        Rect r = entry_box(w, i);
        if (empty(intersect(r, v)) || empty(intersect(r, desk.clip))) {
            continue;
        }
        Entry *e = &f->entries[f->order[i]];
        bool folder = (e->st.mode & 0170000) == S_IFDIR, card = folder && !f->mode,
             hover = contains(r, desk.mouse_x, desk.mouse_y);
        if (card) {
            rounded(r, S(7),
                    e->selected ? desk.selection
                    : hover     ? theme_color(0xF8FAF9)
                                : THEME_WINDOW);
            border(r, S(7), e->selected ? theme_color(0xB7D6C7) : THEME_BORDER);
            Rect ib = {r.x + S(10), r.y + S(11), S(31), S(31)};
            rounded(ib, S(6), theme_color(bg[i % 4]));
            icon(ib.x + S(6), ib.y + S(6), I_FOLDER, S(18), fg[i % 4]);
            label(r.x + S(52), r.y + S(10), e->name, FONT_BOLD, theme_color(0x36474B), r.w - S(63));
            char modified[32];
            modification(modified, e);
            if (!e->st.mtime_sec) {
                copy_text(modified, sizeof(modified), "Date unavailable");
            } else {
                char value[32];
                copy_text(value, sizeof(value), modified);
                if (!strncmp(value, "Today", 5)) {
                    copy_text(value, sizeof(value), "today");
                } else if (!strncmp(value, "Yesterday", 9)) {
                    copy_text(value, sizeof(value), "yesterday");
                }
                copy_text(modified, sizeof(modified), "Modified ");
                append_text(modified, sizeof(modified), value);
            }

            label(r.x + S(52), r.y + S(29), modified, FONT_SMALL, theme_color(0xA2ADAF),
                  r.w - S(63));
        } else {
            if (e->selected || hover) {
                rounded(r, S(4), e->selected ? desk.selection : theme_color(0xF6F8F7));
            }
            bool json = strlen(e->name) > 5 && same_text(e->name + strlen(e->name) - 5, ".json");
            rounded((Rect){r.x + S(4), r.y + S(6), S(21), S(24)}, S(4),
                    folder ? theme_color(0xE3F0E9)
                    : json ? theme_color(0xFAF0E6)
                           : theme_color(0xEAF0F6));
            icon(r.x + S(8), r.y + S(11), folder ? I_FOLDER : I_DOC, S(13),
                 folder ? 0x70A489
                 : json ? 0xCB9569
                        : 0x7D9AB8);
            label(r.x + S(37), r.y + S(10), e->name, FONT_MEDIUM, THEME_TEXT,
                  MAX(S(30), r.w - S(metadata ? 222 : 94)));
            char modified[32], size[32];
            modification(modified, e);
            format_size(size, e->st.size);
            if (metadata) {
                label(modified_x, r.y + S(10), modified, FONT_NORMAL, THEME_SECONDARY_TEXT, S(105));
            }
            if (!folder) {
                label(r.x + r.w - S(4) - text_width(size, FONT_NORMAL), r.y + S(10), size,
                      FONT_NORMAL, THEME_SECONDARY_TEXT, S(55));
            }
            fill((Rect){r.x, r.y + S(35), r.w, 1}, THEME_DIVIDER);
        }

        if (i == f->focus && !f->input_focus && f->keyboard_focus == 0 && wm_focused() == w) {
            border((Rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, S(card ? 6 : 3),
                   e->selected ? desk.accent : theme_color(0xA3BEB4));
        }
    }

    if (!f->visible_count) {
        label(v.x, v.y + S(114),
              f->query[0] ? "No matching names in this folder." : "This folder is empty.",
              FONT_BODY, THEME_SECONDARY_TEXT, v.w);
    }
    scrollbar(v, f->content_height, f->scroll);
    desk.clip = saved;
    clip_set(intersect(saved, m));
    int selected = 0;
    for (int i = 0; i < f->count; i++) {
        selected += f->entries[i].selected;
    }
    char status[300];
    if (f->job.active) {
        format_size(status, f->job.bytes);
        append_text(status, sizeof(status), " copied - Escape cancels safely");
    } else if (f->status[0]) {
        copy_text(status, sizeof(status), f->status);
    } else if (selected) {
        format_uint(status, selected);
        append_text(status, sizeof(status), " selected");
    } else {
        copy_text(status, sizeof(status), f->limited ? "Directory limit: first 2048 entries" : "");
    }
    label(m.x + S(26), m.y + m.h - S(19), status, FONT_SMALL, THEME_SECONDARY_TEXT, m.w - S(52));
    desk.clip = saved;
}

static int selected_count(Files *f) {
    int n = 0;
    for (int i = 0; i < f->count; i++) {
        n += f->entries[i].selected;
    }
    return n;
}

static Entry *selected_one(Files *f) {
    for (int i = 0; i < f->count; i++) {
        if (f->entries[i].selected) {
            return &f->entries[i];
        }
    }
    return f->visible_count ? &f->entries[f->order[f->focus]] : NULL;
}

static void select_entry(Window *w, int position, unsigned mods) {
    Files *f = w->state;
    if (position < 0 || position >= f->visible_count) {
        return;
    }
    if (!(mods & (MOD_CTRL | MOD_SHIFT))) {
        for (int i = 0; i < f->count; i++) {
            f->entries[i].selected = false;
        }
    }
    if (mods & MOD_SHIFT) {
        for (int i = MIN(position, f->anchor); i <= MAX(position, f->anchor); i++) {
            f->entries[f->order[i]].selected = true;
        }
    } else {
        Entry *e = &f->entries[f->order[position]];
        e->selected = (mods & MOD_CTRL) ? !e->selected : true;
        f->anchor = position;
    }
    f->focus = position;
    f->input_focus = 0;
    f->keyboard_focus = 0;
    f->status[0] = 0;
    Rect r = entry_box(w, position), v = content_view(w);
    if (r.y < v.y) {
        f->scroll = MAX(0, f->scroll - (v.y - r.y));
    } else if (r.y + r.h > v.y + v.h) {
        f->scroll += r.y + r.h - v.y - v.h;
    }
    smooth_jump(&f->animation, f->scroll, now_ms());
    invalidate_window(w);
}

static void open_selected(Window *w) {
    Files *f = w->state;
    Entry *e = selected_one(f);
    if (!e) {
        return;
    }
    if (e->error) {
        dialog_error(w, "Read metadata", e->error);
        return;
    }
    if ((e->st.mode & 0170000) == S_IFDIR) {
        navigate(w, e->path, true);
        return;
    }
    if ((e->st.mode & 0170000) != S_IFREG) {
        dialog_notice(w, "Open With",
                      "This file type has no registered GUI application. Device nodes and special "
                      "files should be accessed through Terminal.");
        return;
    }

    const char *extension = NULL;
    for (const char *p = e->path; *p; p++)
        if (*p == '.')
            extension = p;
    if (extension && (same_text(extension, ".html") || same_text(extension, ".htm") ||
                      same_text(extension, ".svg") || same_text(extension, ".png") ||
                      same_text(extension, ".jpg") || same_text(extension, ".jpeg") ||
                      same_text(extension, ".gif") || same_text(extension, ".bmp"))) {
        if (wm_open(APP_BROWSER, e->path))
            recent_add(e->path);
        return;
    }

    int fd = open(e->path, O_RDONLY, 0);
    if (fd < 0) {
        dialog_error(w, "Open file", fd);
        return;
    }
    unsigned char bytes[1024];
    long n = read(fd, bytes, sizeof(bytes));
    close(fd);
    if (n < 0) {
        dialog_error(w, "Read file", n);
        return;
    }
    for (long i = 0; i < n; i++) {
        if (!bytes[i] ||
            (bytes[i] < 32 && bytes[i] != '\n' && bytes[i] != '\r' && bytes[i] != '\t')) {
            dialog_notice(w, "Open With",
                          "No GUI application is registered for this binary file. Text documents "
                          "open in Text Editor. Use Terminal for supported executable programs.");
            return;
        }
    }

    Window *editor = wm_open(APP_EDITOR, e->path);
    if (editor) {
        recent_add(e->path);
    }
}

static void create_folder(Window *w, const char *name) {
    Files *f = w->state;
    char path[OS_PATH_MAX];
    if (!name[0] || same_text(name, ".") || same_text(name, "..")) {
        dialog_error(w, "New folder", -E_INVAL);
        return;
    }
    for (const char *p = name; *p; p++) {
        if (*p == '/') {
            dialog_notice(w, "New folder", "Enter one folder name, without a slash.");
            return;
        }
    }

    int e = join_path(path, f->directory, name);
    if (!e) {
        e = call2(SYS_mkdir, path, 0755);
    }
    if (e) {
        dialog_error(w, "New folder", e);
    } else {
        files_refresh_all();
        trace("CREATE_FOLDER", w, path);
    }
}

static void create_file(Window *w, const char *name) {
    Files *f = w->state;
    char path[OS_PATH_MAX];
    if (!name[0] || same_text(name, ".") || same_text(name, "..")) {
        dialog_error(w, "New file", -E_INVAL);
        return;
    }
    for (const char *p = name; *p; p++) {
        if (*p == '/') {
            dialog_notice(w, "New file", "Enter one filename, without a slash.");
            return;
        }
    }

    int e = join_path(path, f->directory, name);
    if (!e) {
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
        e = fd < 0 ? fd : fsync(fd);
        if (fd >= 0) {
            close(fd);
        }
    }

    if (e) {
        dialog_error(w, "New file", e);
    } else {
        files_refresh_all();
        trace("CREATE_FILE", w, path);
    }
}

static void rename_entry(Window *w, const char *name) {
    Files *f = w->state;
    Entry *e = selected_one(f);
    if (!e) {
        return;
    }
    if (!name[0] || same_text(name, ".") || same_text(name, "..")) {
        dialog_error(w, "Rename", -E_INVAL);
        return;
    }
    for (const char *p = name; *p; p++) {
        if (*p == '/') {
            dialog_notice(w, "Rename", "Use one filename, without a slash.");
            return;
        }
    }

    char parent[OS_PATH_MAX], path[OS_PATH_MAX];
    normalize_path(parent, e->path, "..");
    int result = join_path(path, parent, name);
    struct os_stat st;
    if (!result && !same_text(path, e->path) && !stat(path, &st)) {
        result = -E_EXIST;
    }
    if (!result) {
        result = rename_noreplace(e->path, path);
    }
    if (result) {
        dialog_error(w, "Rename", result);
    } else {
        trace("RENAME", w, path);
        files_refresh_all();
    }
}

static int move_trash(Entry *entry) {
    char root[OS_PATH_MAX], target[OS_PATH_MAX], info_dir[OS_PATH_MAX], info[OS_PATH_MAX];
    int e = trash_root(entry->path, root);
    if (e) {
        return e;
    }
    if (!strncmp(entry->path, root, strlen(root)) && entry->path[strlen(root)] == '/') {
        return -E_INVAL;
    }
    e = call2(SYS_mkdir, root, 0700);
    if (e && e != -E_EXIST) {
        return e;
    }
    e = join_path(info_dir, root, ".info");
    if (!e) {
        e = call2(SYS_mkdir, info_dir, 0700);
    }
    if (e && e != -E_EXIST) {
        return e;
    }
    char name[OS_NAME_MAX + 1];
    copy_text(name, sizeof(name), entry->name);
    unsigned number = 0;
    struct os_stat st;
    while (!(e = join_path(target, root, name)) && !stat(target, &st)) {
        char count[32];
        format_uint(count, ++number);
        copy_text(name, sizeof(name), entry->name);
        size_t max = OS_NAME_MAX - strlen(count) - 1;
        if (strlen(name) > max) {
            name[max] = 0;
        }
        append_text(name, sizeof(name), ".");
        append_text(name, sizeof(name), count);
    }

    if (e) {
        return e;
    }
    e = join_path(info, info_dir, name);
    if (e) {
        return e;
    }
    int fd = open(info, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return fd;
    }
    e = write_all(fd, entry->path, strlen(entry->path));
    if (!e) {
        e = fsync(fd);
    }
    close(fd);
    if (!e) {
        e = rename_noreplace(entry->path, target);
    }
    if (e) {
        call1(SYS_unlink, info);
    }
    return e;
}

static void permanent_delete(Window *w, const char *unused) {
    (void)unused;
    Files *f = w->state;
    int e = 0;
    for (int i = 0; i < f->count; i++) {
        if (f->entries[i].selected) {
            Entry *entry = &f->entries[i];
            long result =
                call1((entry->st.mode & 0170000) == S_IFDIR ? SYS_rmdir : SYS_unlink, entry->path);
            if (result) {
                e = result;
                break;
            }
            char parent[OS_PATH_MAX], info_dir[OS_PATH_MAX], info[OS_PATH_MAX];
            normalize_path(parent, entry->path, "..");
            if (!join_path(info_dir, parent, ".info") && !join_path(info, info_dir, entry->name)) {
                call1(SYS_unlink, info);
            }
        }
    }

    files_refresh_all();
    if (e) {
        dialog_error(w, "Delete permanently", e);
    }
}

static void restore_trash(Window *w) {
    Files *f = w->state;
    Entry *e = selected_one(f);
    if (!e) {
        return;
    }
    char parent[OS_PATH_MAX], dir[OS_PATH_MAX], info[OS_PATH_MAX], original[OS_PATH_MAX];
    normalize_path(parent, e->path, "..");
    int result = join_path(dir, parent, ".info");
    if (!result) {
        result = join_path(info, dir, e->name);
    }
    int fd = result ? -1 : open(info, O_RDONLY, 0);
    if (fd < 0) {
        dialog_error(w, "Restore", result ? result : fd);
        return;
    }
    long n = read(fd, original, sizeof(original) - 1);
    close(fd);
    if (n <= 0) {
        dialog_error(w, "Restore", n < 0 ? n : -E_IO);
        return;
    }
    original[n] = 0;
    struct os_stat st;
    result = !stat(original, &st) ? -E_EXIST : rename_noreplace(e->path, original);
    if (result) {
        dialog_error(w, "Restore", result);
    } else {
        call1(SYS_unlink, info);
        files_refresh_all();
        trace("RESTORE_FILE", w, original);
    }
}

static bool clipboard_set(Window *w, bool cut) {
    Files *f = w->state;
    int count = selected_count(f);
    if (!count) {
        select_entry(w, f->focus, 0);
        count = selected_count(f);
    }
    if (!count) {
        return false;
    }
    if (count > CLIPBOARD_LIMIT) {
        dialog_notice(w, "Clipboard limit",
                      "Select at most 128 items for one copy or cut operation.");
        return false;
    }
    for (int i = 0; i < f->count; i++) {
        if (f->entries[i].selected && f->entries[i].link) {
            dialog_notice(w, "Copy symbolic link",
                          "Symbolic links can be opened. Copying or moving link nodes needs a "
                          "no-follow VFS interface; copy the real target explicitly instead.");
            trace("ERROR", w, "Copy symbolic link");
            return false;
        }
    }

    clipboard.count = 0;
    clipboard.cut = cut;
    for (int i = 0; i < f->count; i++) {
        if (f->entries[i].selected) {
            copy_text(clipboard.paths[clipboard.count++], OS_PATH_MAX, f->entries[i].path);
        }
    }
    format_uint(f->status, clipboard.count);
    append_text(f->status, sizeof(f->status),
                cut ? " items cut. Paste to move." : " items copied. Paste to duplicate.");
    invalidate_window(w);
    trace(cut ? "CUT" : "COPY", w, "");
    return true;
}

static void job_close(CopyJob *j) {
    if (j->input >= 0) {
        close(j->input);
    }
    if (j->output >= 0) {
        close(j->output);
    }
    j->input = j->output = -1;
    while (j->depth) {
        j->depth--;
        if (j->levels[j->depth].fd >= 0) {
            close(j->levels[j->depth].fd);
        }
    }
}

static void job_cancel(Window *w, bool error, long code) {
    Files *f = w->state;
    CopyJob *j = &f->job;
    job_close(j);
    /* Only the unfinished regular file is rolled back. Completed files and
       created directories stay visible, and cancellation says so clearly. */
    if (j->target[0]) {
        call1(SYS_unlink, j->target);
    }
    j->active = false;
    files_refresh_all();
    if (error) {
        dialog_error(w, "Copy / move stopped", code);
    } else {
        copy_text(f->status, sizeof(f->status),
                  "Cancelled. Completed files remain; unfinished file removed.");
        invalidate_window(w);
    }
}

static int copy_begin(CopyJob *j, const char *source, const char *target) {
    struct os_stat st, destination;
    int e = stat(source, &st);
    if (e) {
        return e;
    }
    e = stat(target, &destination);
    if (!e) {
        return -E_EXIST;
    }
    if (e != -E_NOENT) {
        return e;
    }
    size_t n = strlen(source);
    if (!strncmp(target, source, n) && (target[n] == '/' || !target[n])) {
        return -E_INVAL;
    }
    if (j->cut) {
        int moved = rename_noreplace(source, target);
        if (moved != -E_XDEV || (st.mode & 0170000) != S_IFREG) {
            return moved;
        }
        /* Cross-mount regular-file moves copy and flush before unlinking. */
        copy_text(j->remove_source, sizeof(j->remove_source), source);
    }

    if ((st.mode & 0170000) == S_IFDIR) {
        if (j->depth == COPY_DEPTH) {
            return -E_2BIG;
        }
        e = call2(SYS_mkdir, target, st.mode & 0777);
        if (e) {
            return e;
        }
        CopyLevel *l = &j->levels[j->depth];
        memset(l, 0, sizeof(*l));
        l->fd = open(source, O_RDONLY | O_DIRECTORY, 0);
        if (l->fd < 0) {
            return l->fd;
        }
        copy_text(l->source, sizeof(l->source), source);
        copy_text(l->target, sizeof(l->target), target);
        l->mode = st.mode & 0777;
        j->depth++;
        return 0;
    }

    if ((st.mode & 0170000) != S_IFREG) {
        return -E_NOTSUP;
    }
    j->input = open(source, O_RDONLY, 0);
    if (j->input < 0) {
        return j->input;
    }
    /* Initialize a writable destination, then apply source permissions before
       the final flush. FAT32 creation does not preserve its read-only bit. */
    j->output = open(target, O_WRONLY | O_CREAT | O_EXCL, (st.mode & 0777) | 0600);
    if (j->output < 0) {
        close(j->input);
        j->input = -1;
        return j->output;
    }
    copy_text(j->target, sizeof(j->target), target);
    j->mode = st.mode & 0777;
    return 0;
}

static void paste(Window *w, const char *destination) {
    Files *f = w->state;
    if (f->job.active) {
        dialog_notice(w, "Operation in progress",
                      "Wait or press Escape to cancel the current operation.");
        return;
    }
    if (!clipboard.count || destination[0] == '@') {
        return;
    }
    CopyJob *j = &f->job;
    memset(j, 0, sizeof(*j));
    j->input = j->output = -1;
    j->count = clipboard.count;
    j->cut = clipboard.cut;
    j->active = true;
    memcpy(j->source, clipboard.paths, j->count * OS_PATH_MAX);
    copy_text(j->destination, sizeof(j->destination), destination);
    invalidate_window(w);
}

void files_tick(Window *w) {
    Files *f = w->state;
    int scroll = smooth_tick(&f->animation, f->scroll,
                             MAX(0, content_height(w) - content_view(w).h), now_ms());
    if (scroll != f->scroll) {
        f->scroll = scroll;
        trace_scroll(w, "Files", scroll, f->animation.target);
        invalidate(content_view(w));
    }
    CopyJob *j = &f->job;
    if (f->listing.active) {
        directory_step(w);
    }
    if (!j->active) {
        return;
    }
    int e = 0;
    if (j->input >= 0) {
        long n = read(j->input, j->chunk, sizeof(j->chunk));
        if (n < 0) {
            e = n;
        } else if (n) {
            e = write_all(j->output, j->chunk, n);
            if (!e) {
                j->bytes += n;
            }
        } else {
            e = call2(SYS_fchmod, j->output, j->mode);
            if (!e) {
                e = fsync(j->output);
            }
            if (e) {
                call2(SYS_fchmod, j->output, j->mode | 0600);
            }
            close(j->input);
            close(j->output);
            j->input = j->output = -1;
            if (!e) {
                j->target[0] = 0;
                if (j->remove_source[0]) {
                    e = call1(SYS_unlink, j->remove_source);
                    j->remove_source[0] = 0;
                }
            }
        }
    } else if (j->depth) {
        CopyLevel *level = &j->levels[j->depth - 1];
        if (level->offset == level->used) {
            long n = call3(SYS_getdents64, level->fd, level->buffer, sizeof(level->buffer));
            if (n < 0) {
                e = n;
            } else {
                level->offset = 0;
                level->used = n;
                if (!n) {
                    e = call2(SYS_fchmod, level->fd, level->mode);
                    if (!e) {
                        e = fsync(level->fd);
                    }
                    close(level->fd);
                    level->fd = -1;
                    j->depth--;
                }
            }
        }

        if (!e && j->depth && level->offset < level->used) {
            struct os_dirent *d = (void *)(level->buffer + level->offset);
            if (d->reclen < 24 || d->reclen > level->used - level->offset) {
                e = -E_IO;
            } else {
                level->offset += d->reclen;
                if (!same_text(d->name, ".") && !same_text(d->name, "..")) {
                    char source[OS_PATH_MAX], target[OS_PATH_MAX];
                    e = d->type == 10 ? -E_NOTSUP : join_path(source, level->source, d->name);
                    if (!e) {
                        e = join_path(target, level->target, d->name);
                    }
                    if (!e) {
                        e = copy_begin(j, source, target);
                    }
                }
            }
        }
    } else if (j->item < j->count) {
        char target[OS_PATH_MAX];
        const char *source = j->source[j->item];
        e = join_path(target, j->destination, basename(source));
        if (!e) {
            e = copy_begin(j, source, target);
        }
        if (!e) {
            j->item++;
        }
    } else {
        j->active = false;
        if (j->cut) {
            clipboard.count = 0;
        }
        files_refresh_all();
        copy_text(f->status, sizeof(f->status), "File operation completed.");
        trace("PASTE_COMPLETE", w, j->destination);
        invalidate_window(w);
        return;
    }

    if (e) {
        job_cancel(w, true, e);
        return;
    }
    uint64_t now = now_ms();
    if (now - j->last_update >= 80) {
        j->last_update = now;
        Rect m = main_area(w);
        if (!w->minimized && w->workspace == desk.workspace) {
            invalidate((Rect){m.x, m.y + m.h - S(25), m.w, S(24)});
        }
    }
}

static void action(Window *w, int operation) {
    if (!w || !w->alive) {
        return;
    }
    Files *f = w->state;
    Entry *e = selected_one(f);
    if (f->job.active && operation != A_INFO) {
        dialog_notice(w, "Operation in progress",
                      "Wait or press Escape to cancel the current operation.");
        return;
    }
    switch (operation) {
    case A_OPEN:
        open_selected(w);
        break;
    case A_WALLPAPER:
        if (e) {
            int error = wallpaper_set(WALLPAPER_CUSTOM, e->path);
            if (error) {
                dialog_error(w, "Set wallpaper", error);
                break;
            }
            settings_apply();
            copy_text(f->status, sizeof(f->status),
                      "Wallpaper changed. Save preferences in Settings to keep it.");
            trace("WALLPAPER", w, e->path);
        }
        break;
    case A_COPY:
        clipboard_set(w, false);
        break;
    case A_CUT:
        clipboard_set(w, true);
        break;
    case A_PASTE:
        paste(w, f->directory);
        break;
    case A_RENAME:
        if (selected_count(f) > 1) {
            dialog_notice(w, "Rename",
                          "Select one item to rename. Batch renaming is not supported.");
            break;
        }
        if (e) {
            dialog_input(w, "Rename",
                         "Choose a new name. Existing destinations are never overwritten.", e->name,
                         rename_entry);
        }
        break;
    case A_FOLDER:
        dialog_input(w, "New folder", "Create a real folder in the current directory.", "",
                     create_folder);
        break;
    case A_FILE:
        dialog_input(w, "New file", "Create an empty file in the current directory.", "",
                     create_file);
        break;
    case A_REFRESH:
        refresh(w);
        break;
    case A_PARENT:
        if (f->directory[0] == '@') {
            navigate(w, desk.home, true);
        } else {
            navigate(w, "..", true);
        }
        break;
    case A_TRASH:
        if (same_text(f->directory, "@Trash")) {
            dialog_confirm(w, "Delete permanently",
                           "This cannot be undone. Empty folders and regular files can be removed; "
                           "nonempty folders must first be emptied.",
                           permanent_delete);
            break;
        }

        if (!selected_count(f) && f->visible_count) {
            select_entry(w, f->focus, 0);
        }
        for (int i = 0; i < f->count; i++) {
            if (f->entries[i].selected) {
                int result = move_trash(&f->entries[i]);
                if (result) {
                    files_refresh_all();
                    dialog_error(w, "Move to Trash", result);
                    return;
                }
            }
        }

        files_refresh_all();
        trace("TRASH", w, "");
        break;
    case A_RESTORE:
        restore_trash(w);
        break;
    case A_HIDDEN:
        f->show_hidden = !f->show_hidden;
        filter(f);
        invalidate_window(w);
        break;
    case A_INFO:
        if (e) {
            char b[640], size[32], mode[32];
            copy_text(b, sizeof(b), e->path);
            append_text(b, sizeof(b), "\nType: ");
            append_text(b, sizeof(b),
                        e->link                             ? "Symbolic link (resolved metadata)"
                        : (e->st.mode & 0170000) == S_IFDIR ? "Folder"
                                                            : "File");
            format_size(size, e->st.size);
            append_text(b, sizeof(b), "    Size: ");
            append_text(b, sizeof(b), size);
            append_text(b, sizeof(b), "\nPermissions: ");
            mode[0] = '0';
            mode[1] = '0' + ((e->st.mode >> 6) & 7);
            mode[2] = '0' + ((e->st.mode >> 3) & 7);
            mode[3] = '0' + (e->st.mode & 7);
            mode[4] = 0;
            append_text(b, sizeof(b), mode);
            append_text(b, sizeof(b), "\nModification time: ");
            if (e->st.mtime_sec) {
                format_uint(mode, e->st.mtime_sec);
                append_text(b, sizeof(b), mode);
                append_text(b, sizeof(b), " (Unix UTC)");
            } else {
                append_text(b, sizeof(b), "unavailable from this filesystem");
            }
            if (e->error) {
                append_text(b, sizeof(b), "\nMetadata error: ");
                append_text(b, sizeof(b), error_text(e->error));
            }
            dialog_notice(w, "File information", b);
        }
        break;
    }
}

static bool wallpaper_entry(Entry *entry) {
    if (!entry || entry->error || (entry->st.mode & 0170000) != S_IFREG)
        return false;
    const char *extension = NULL;
    for (const char *p = entry->name; *p; p++)
        if (*p == '.')
            extension = p;
    if (!extension || strlen(extension) > 5)
        return false;
    char suffix[6];
    copy_text(suffix, sizeof(suffix), extension);
    for (char *p = suffix; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
    return !strcmp(suffix, ".png") || !strcmp(suffix, ".jpg") || !strcmp(suffix, ".jpeg") ||
           !strcmp(suffix, ".bmp");
}

static void context_menu(Window *w, int x, int y, bool entry) {
    Files *f = w->state;
    bool ordinary = f->directory[0] != '@';
    MenuItem items[13] = {
        {"Open", I_FOLDER, A_OPEN, entry},
        {"Copy", I_COPY, A_COPY, entry},
        {"Cut", I_CUT, A_CUT, entry},
        {"Paste", I_PASTE, A_PASTE, ordinary && clipboard.count > 0},
        {"Rename  F2", I_RENAME, A_RENAME, entry && !same_text(f->directory, "@Trash")},
        {same_text(f->directory, "@Trash") ? "Delete permanently" : "Move to Trash", I_TRASH,
         A_TRASH, entry},
        {"Restore from Trash", I_RESTORE, A_RESTORE, entry && same_text(f->directory, "@Trash")},
        {"New folder", I_FOLDER, A_FOLDER, ordinary},
        {"New file", I_DOC, A_FILE, ordinary},
        {"Refresh  F5", I_RESTORE, A_REFRESH, true},
        {"File information", I_INFO, A_INFO, entry},
        {f->show_hidden ? "Hide hidden files" : "Show hidden files", I_LAYOUT, A_HIDDEN, true},
        {"Set as wallpaper", I_IMAGE, A_WALLPAPER, true}};
    menu_open(w, x, y, items, entry && wallpaper_entry(selected_one(f)) ? 13 : 12, action);
}

bool files_mouse(Window *w, int x, int y, unsigned pressed, int wheel) {
    Files *f = w->state;
    Rect b = window_body(w), m = main_area(w), v = content_view(w);
    if (wheel && contains(m, x, y)) {
        smooth_by(&f->animation, f->scroll, -(int64_t)wheel * S(54),
                  MAX(0, content_height(w) - v.h), now_ms());
        return true;
    }
    if (pressed & 1) {
        f->input_focus = 0;
        if (x < b.x + sidebar_width(w)) {
            int row = (y - b.y - S(42)) / S(30);
            if (y >= b.y + S(42) && row >= 0 && row < 6) {
                const char *names[] = {"",         "@Recent",  "Downloads",
                                       "Projects", "Pictures", "@Trash"};
                char path[OS_PATH_MAX];
                if (!row) {
                    copy_text(path, sizeof(path), desk.home);
                } else if (row == 1 || row == 5) {
                    copy_text(path, sizeof(path), names[row]);
                } else {
                    join_path(path, desk.home, names[row]);
                }
                navigate(w, path, true);
                return true;
            }

            if (contains((Rect){b.x + S(9), b.y + S(273), sidebar_width(w) - S(18), S(30)}, x, y)) {
                navigate(w, "/", true);
                return true;
            }
        }

        if (contains((Rect){m.x + m.w - S(136), m.y + S(22), S(28), S(28)}, x, y)) {
            f->mode = !f->mode;
            f->scroll = 0;
            smooth_jump(&f->animation, 0, now_ms());
            invalidate_window(w);
            trace("VIEW", w, f->mode ? "details" : "hybrid");
            return true;
        }

        if (contains((Rect){m.x + m.w - S(97), m.y + S(22), S(71), S(28)}, x, y)) {
            if (f->directory[0] != '@') {
                MenuItem items[] = {{"New folder", I_FOLDER, A_FOLDER, true},
                                    {"New empty file", I_DOC, A_FILE, true}};
                menu_open(w, m.x + m.w - S(228), m.y + S(54), items, 2, action);
            }
            return true;
        }
    }

    if (contains(v, x, y)) {
        int hit = -1;
        for (int i = 0; i < f->visible_count; i++) {
            if (contains(entry_box(w, i), x, y)) {
                hit = i;
                break;
            }
        }

        if (hit >= 0) {
            Entry *e = &f->entries[f->order[hit]];
            if (pressed & 2) {
                if (!e->selected) {
                    select_entry(w, hit, 0);
                }
                context_menu(w, x, y, true);
                return true;
            }

            if (pressed & 1) {
                if (!(e->selected && selected_count(f) > 1 &&
                      !(desk.modifiers & (MOD_CTRL | MOD_SHIFT)))) {
                    select_entry(w, hit, desk.modifiers);
                }
                uint64_t now = now_ms();
                if (desk.last_click_window == w->id && desk.last_click_hit == hit &&
                    now - desk.last_click < 420 && !(desk.modifiers & (MOD_CTRL | MOD_SHIFT))) {
                    desk.last_click = 0;
                    open_selected(w);
                    return true;
                }

                desk.last_click = now;
                desk.last_click_window = w->id;
                desk.last_click_hit = hit;
                f->drag_entry = hit;
                f->drag_x = x;
                f->drag_y = y;
                f->dragging = false;
            }

            if (f->drag_entry >= 0 && (desk.buttons & 1) &&
                (x - f->drag_x > S(6) || f->drag_x - x > S(6) || y - f->drag_y > S(6) ||
                 f->drag_y - y > S(6))) {
                f->dragging = true;
            }
            if (f->dragging && !(desk.buttons & 1)) {
                if ((e->st.mode & 0170000) == S_IFDIR && !e->selected) {
                    char dest[OS_PATH_MAX];
                    copy_text(dest, sizeof(dest), e->path);
                    if (clipboard_set(w, true)) {
                        paste(w, dest);
                    }
                }

                f->dragging = false;
                f->drag_entry = -1;
            }

            return true;
        }

        if (pressed & 2) {
            context_menu(w, x, y, false);
            return true;
        }
        int file_start = f->mode
                             ? v.y - f->scroll
                             : v.y + S(30) + (f->folders + f->columns - 1) / f->columns * S(61) +
                                   (f->folders ? S(25) : 0) + S(30) - f->scroll;
        if ((pressed & 1) && y >= file_start && y < file_start + S(28)) {
            int sort = x >= v.x + v.w - S(55) ? 2 : x >= v.x + v.w - S(167) ? 1 : 0;
            if (sort == f->sort) {
                f->descending = !f->descending;
            } else {
                f->sort = sort;
                f->descending = false;
            }
            filter(f);
            invalidate_window(w);
            return true;
        }

        if (pressed & 1) {
            for (int i = 0; i < f->count; i++) {
                f->entries[i].selected = false;
            }
            invalidate_window(w);
        }
    }

    if (!(desk.buttons & 1)) {
        f->drag_entry = -1;
        f->dragging = false;
    }
    return false;
}

void files_key(Window *w, Key k) {
    if (!k.down) {
        return;
    }
    Files *f = w->state;
    if (k.code == KEY_ESCAPE) {
        if (f->job.active) {
            job_cancel(w, false, 0);
        } else {
            f->input_focus = 0;
            f->query[0] = 0;
            f->status[0] = 0;
            filter(f);
            invalidate_window(w);
        }

        return;
    }

    if ((k.mods & MOD_CTRL) && (k.ascii == 'l' || k.ascii == 'L')) {
        f->input_focus = 1;
        f->replace_input = true;
        copy_text(f->path_input, sizeof(f->path_input),
                  f->directory[0] == '@' ? desk.home : f->directory);
        f->cursor = strlen(f->path_input);
        invalidate_window(w);
        return;
    }

    if ((k.mods & MOD_CTRL) && (k.ascii == 'f' || k.ascii == 'F')) {
        f->input_focus = 2;
        f->replace_input = true;
        f->cursor = strlen(f->query);
        copy_text(f->status, sizeof(f->status), "Search scope: names in this folder only.");
        invalidate_window(w);
        return;
    }

    if (k.code == KEY_TAB && f->input_focus) {
        f->keyboard_focus = f->input_focus == 2 ? 1 : 0;
        f->input_focus = 0;
    }
    if (f->input_focus) {
        if (k.code == KEY_ENTER) {
            if (f->input_focus == 1) {
                navigate(w, f->path_input, true);
            } else {
                f->input_focus = 0;
                invalidate_window(w);
            }
            return;
        }

        char *text = f->input_focus == 1 ? f->path_input : f->query;
        if ((k.mods & MOD_CTRL) && (k.ascii == 'a' || k.ascii == 'A')) {
            f->replace_input = true;
            invalidate_window(w);
            return;
        }
        if (f->replace_input && ((k.ascii >= 32 && !(k.mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))) ||
                                 k.code == KEY_BACKSPACE || k.code == KEY_DELETE)) {
            text[0] = 0;
            f->cursor = 0;
            f->replace_input = false;
        }

        if (k.code == KEY_LEFT || k.code == KEY_RIGHT || k.code == KEY_HOME || k.code == KEY_END) {
            f->replace_input = false;
        }
        if (input_key(text, f->input_focus == 1 ? sizeof(f->path_input) : sizeof(f->query),
                      &f->cursor, k)) {
            if (f->input_focus == 2) {
                filter(f);
                f->scroll = 0;
                smooth_jump(&f->animation, 0, now_ms());
            }
            invalidate_window(w);
        }

        return;
    }

    if (k.code == KEY_TAB) {
        f->keyboard_focus = (f->keyboard_focus + ((k.mods & MOD_SHIFT) ? 4 : 1)) % 5;
        if (f->keyboard_focus == 1) {
            f->input_focus = 2;
            f->cursor = strlen(f->query);
        }
        if (f->keyboard_focus == 3) {
            f->anchor = 0;
        }
        invalidate_window(w);
        return;
    }

    if (f->keyboard_focus == 3) {
        if (k.code == KEY_UP || k.code == KEY_DOWN) {
            f->anchor = clampi(f->anchor + (k.code == KEY_UP ? -1 : 1), 0, 5);
            invalidate_window(w);
            return;
        }
        if (k.code == KEY_ENTER) {
            Rect b = window_body(w);
            files_mouse(w, b.x + S(50), b.y + S(57 + f->anchor * 30), 1, 0);
            return;
        }
    }

    if (k.code == KEY_ENTER && f->keyboard_focus == 2) {
        Rect m = main_area(w);
        files_mouse(w, m.x + m.w - S(70), m.y + S(30), 1, 0);
        return;
    }
    if (k.code == KEY_ENTER && f->keyboard_focus == 4) {
        f->mode = !f->mode;
        f->scroll = 0;
        smooth_jump(&f->animation, 0, now_ms());
        invalidate_window(w);
        return;
    }

    if (k.mods & MOD_CTRL) {
        unsigned a = k.ascii | 32;
        if (a == 'a') {
            for (int i = 0; i < f->count; i++) {
                f->entries[i].selected = false;
            }
            for (int i = 0; i < f->visible_count; i++) {
                f->entries[f->order[i]].selected = true;
            }
            invalidate_window(w);
            return;
        }

        if (a == 'c') {
            action(w, A_COPY);
            return;
        }
        if (a == 'x') {
            action(w, A_CUT);
            return;
        }
        if (a == 'v') {
            action(w, A_PASTE);
            return;
        }
        if (a == 'h') {
            action(w, A_HIDDEN);
            return;
        }
        if (a == 'r') {
            action(w, A_REFRESH);
            return;
        }
        if (a == 'n') {
            action(w, (k.mods & MOD_SHIFT) ? A_FOLDER : A_FILE);
            return;
        }
    }

    if ((k.mods & MOD_ALT) && k.code == KEY_LEFT) {
        history_step(w, -1);
        return;
    }
    if ((k.mods & MOD_ALT) && k.code == KEY_RIGHT) {
        history_step(w, 1);
        return;
    }
    if ((k.mods & MOD_ALT) && k.code == KEY_UP) {
        action(w, A_PARENT);
        return;
    }
    if (k.code == KEY_BACKSPACE) {
        if (f->history_position) {
            history_step(w, -1);
        } else {
            action(w, A_PARENT);
        }
        return;
    }

    if (k.code == KEY_F2) {
        action(w, A_RENAME);
        return;
    }
    if (k.code == KEY_DELETE) {
        action(w, A_TRASH);
        return;
    }
    if (k.code == KEY_F5) {
        action(w, A_REFRESH);
        return;
    }
    if (k.code == KEY_ENTER) {
        action(w, A_OPEN);
        return;
    }
    if (k.code == KEY_UP || k.code == KEY_DOWN || k.code == KEY_LEFT || k.code == KEY_RIGHT ||
        k.code == KEY_HOME || k.code == KEY_END || k.code == KEY_PAGEUP || k.code == KEY_PAGEDOWN) {
        int step = (f->focus < f->folders && !f->mode && (k.code == KEY_UP || k.code == KEY_DOWN))
                       ? f->columns
                       : 1;
        int next = f->focus;
        if (k.code == KEY_UP || k.code == KEY_LEFT) {
            next -= step;
        } else if (k.code == KEY_DOWN || k.code == KEY_RIGHT) {
            next += step;
        } else if (k.code == KEY_HOME) {
            next = 0;
        } else if (k.code == KEY_END) {
            next = f->visible_count - 1;
        } else {
            next += (k.code == KEY_PAGEUP ? -1 : 1) * MAX(1, f->viewport.h / S(36));
        }
        select_entry(w, clampi(next, 0, MAX(0, f->visible_count - 1)), k.mods & MOD_SHIFT);
    } else if (k.ascii >= 32 && !(k.mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))) {
        /* Small type-to-select interaction exposes long names in the status. */
        for (int i = 1; i <= f->visible_count; i++) {
            int pos = (f->focus + i) % f->visible_count;
            Entry *e = &f->entries[f->order[pos]];
            if ((unsigned)((uint8_t)e->name[0] | 32) == (k.ascii | 32)) {
                select_entry(w, pos, 0);
                copy_text(f->status, sizeof(f->status), e->name);
                break;
            }
        }
    }
}

bool files_close(Window *w) {
    Files *f = w->state;
    if (f->job.active) {
        job_cancel(w, false, 0);
    }
    if (f->listing.active && f->listing.fd >= 0) {
        close(f->listing.fd);
    }
    deallocate(f, sizeof(*f));
    w->state = NULL;
    return true;
}

Rect files_hover(Window *w, int x, int y) {
    Rect b = window_body(w), m = main_area(w), v = content_view(w);
    Files *f = w->state;
    if (x < b.x + sidebar_width(w) && y >= b.y + S(42) && y < b.y + S(222)) {
        return (Rect){b.x + S(9), b.y + S(42) + ((y - b.y - S(42)) / S(30)) * S(30),
                      sidebar_width(w) - S(18), S(30)};
    }
    if (contains(v, x, y)) {
        for (int i = 0; i < f->visible_count; i++) {
            Rect r = entry_box(w, i);
            if (contains(r, x, y)) {
                return intersect(r, v);
            }
        }
    }

    if (contains((Rect){m.x + m.w - S(136), m.y + S(22), S(28), S(28)}, x, y)) {
        return (Rect){m.x + m.w - S(136), m.y + S(22), S(28), S(28)};
    }
    if (contains((Rect){m.x + m.w - S(97), m.y + S(22), S(71), S(28)}, x, y)) {
        return (Rect){m.x + m.w - S(97), m.y + S(22), S(71), S(28)};
    }

    return (Rect){0};
}
