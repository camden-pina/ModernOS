#include "desktop.h"
#include "smooth_scroll.h"
#include <uapi/clipboard.h>

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
    bool active, trash, preserve;
    Entry *staged;
    int scan_count;
    bool scan_limited;
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
    bool descending, limited, show_hidden, replace_input, desktop, context_current;
    int color_filter;
    int watch;
    uint64_t changed_at;
    int input_anchor;
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

static Window desktop_files;
static int watch_fd = -1;

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
    A_WALLPAPER,
    A_TERMINAL, A_NEW_WINDOW, A_COPY_PATH, A_DUPLICATE, A_SELECT_ALL,
    A_INVERT, A_DETAILS, A_ICONS, A_SORT_NAME, A_SORT_DATE, A_SORT_SIZE,
    A_SORT_TYPE, A_DESCENDING, A_EDITOR, A_LOCATION, A_DESKTOP,
    A_TERMINAL_CURRENT, A_NEW_WINDOW_CURRENT,
    A_TAG, A_CLEAR_TAG, A_FAVORITE, A_COPY_TO, A_MOVE_TO, A_COPY_NAME, A_COPY_PARENT,
    A_HIDE, A_READONLY, A_EXECUTABLE, A_SHORTCUT, A_SELECT_NONE,
    A_SELECT_FILES, A_SELECT_FOLDERS, A_SELECT_TYPE, A_CLEAR_FILTER, A_TEXT_FILE,
    A_MARKDOWN_FILE, A_SCRIPT_FILE, A_FOLDER_INFO,
    A_COLOR_BASE = 64, A_FILTER_BASE = 80
};

static int selected_count(Files *f);
static bool navigate(Window *w, const char *path, bool history);
static void action(Window *w, int operation);
static void directory_step(Window *w);
static void watch_folder(Window *w);
static void background_refresh(Window *w);
static void files_context_action(Window *w, int operation);
static void view_menu(Window *w, int x, int y);
static void toolbar_action(Window *w, int tool);

/* Per-profile annotations use hex fields so whitespace, newlines and UTF-8
   filenames round-trip without becoming delimiters. Never modify the item to
   attach a tag, including on a read-only mount. */
#define MARK_LIMIT 128
typedef struct { char path[OS_PATH_MAX], tag[48]; unsigned color; bool favorite; } FileMark;
static FileMark marks[MARK_LIMIT];
static int mark_count;
static char mark_file[OS_PATH_MAX];
static const char *color_names[] = {"Default", "Red", "Orange", "Yellow", "Green", "Blue", "Purple", "Gray"};
static const uint32_t mark_colors[] = {0, 0xD94F55, 0xD98235, 0xC5A32D, 0x46936A, 0x4B88CA, 0x9A6BC2, 0x84929C};
static int unhex(char c) { return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : -1; }
static bool decode_field(const char **input, char *out, size_t size, char end) {
    const char *p = *input; size_t n = 0;
    while (*p && *p != end) {
        int a = unhex(p[0]), b = p[1] ? unhex(p[1]) : -1;
        if (a < 0 || b < 0 || !(a*16+b) || n+1 >= size) return false;
        out[n++] = a*16+b; p += 2;
    }
    if (*p != end) return false;
    out[n] = 0; *input = p+1; return true;
}
static void marks_load(void) {
    char path[OS_PATH_MAX]; copy_text(path, sizeof(path), desk.config);
    append_text(path, sizeof(path), ".files");
    if (!strcmp(path, mark_file)) return;
    copy_text(mark_file, sizeof(mark_file), path); mark_count = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW, 0);
    if (fd < 0) return;
    size_t capacity = MARK_LIMIT * (OS_PATH_MAX*2+110) + 1, used = 0;
    char *data = allocate(capacity);
    if (!data) { close(fd); return; }
    while (used+1 < capacity) { long n = read(fd, data+used, capacity-used-1); if (n <= 0) break; used += n; }
    close(fd); data[used] = 0; const char *p = data;
    while (*p && mark_count < MARK_LIMIT) {
        FileMark m = {0};
        if (strlen(p) < 5 || p[0] < '0' || p[0] > '7' || (p[1] != '0' && p[1] != '1') || p[2] != ' ') break;
        m.color = p[0]-'0'; m.favorite = p[1]=='1'; p += 3;
        if (!decode_field(&p,m.path,sizeof(m.path),' ') || !decode_field(&p,m.tag,sizeof(m.tag),'\n') || m.path[0]!='/') break;
        marks[mark_count++] = m;
    }
    deallocate(data, capacity);
}
static FileMark *mark_for(const char *path, bool create) {
    for (int i=0;i<mark_count;i++) if (!strcmp(marks[i].path,path)) return &marks[i];
    if (create && mark_count == MARK_LIMIT) {
        for (int i=0;i<mark_count;i++) if (!marks[i].color && !marks[i].tag[0] && !marks[i].favorite) {
            marks[i]=marks[--mark_count];break;
        }
    }
    if (!create || mark_count == MARK_LIMIT || path[0]!='/' || strlen(path)>=OS_PATH_MAX) return NULL;
    FileMark *m = &marks[mark_count++]; memset(m,0,sizeof(*m)); copy_text(m->path,sizeof(m->path),path); return m;
}
static int marks_save(void) {
    int prepared=profiles_prepare(); if (prepared) return prepared;
    size_t capacity = MARK_LIMIT * (OS_PATH_MAX*2+110) + 1, used = 0;
    char *data = allocate(capacity); if (!data) return -E_NOMEM;
    static const char hex[]="0123456789abcdef";
    for (int i=0;i<mark_count;i++) {
        FileMark *m=&marks[i]; if (!m->color && !m->tag[0] && !m->favorite) continue;
        data[used++]='0'+m->color; data[used++]=m->favorite?'1':'0'; data[used++]=' ';
        const char *fields[]={m->path,m->tag};
        for (int j=0;j<2;j++) {
            for (const unsigned char *p=(const unsigned char *)fields[j];*p;p++) { data[used++]=hex[*p>>4];data[used++]=hex[*p&15]; }
            data[used++]=j?'\n':' ';
        }
    }
    data[used]=0; int error=preferences_write(mark_file,data); deallocate(data,capacity); return error;
}
static void marks_moved(const char *source, const char *target) {
    size_t length=strlen(source); bool changed=false;
    for (int i=0;i<mark_count;i++) {
        FileMark *m=&marks[i];
        if (strncmp(m->path,source,length) || (m->path[length] && m->path[length]!='/')) continue;
        char path[OS_PATH_MAX]; copy_text(path,sizeof(path),target);
        if (strlen(target)+strlen(m->path+length)>=sizeof(path)) continue;
        append_text(path,sizeof(path),m->path+length); copy_text(m->path,sizeof(m->path),path); changed=true;
    }
    if (changed) marks_save();
}

static bool same_text(const char *a, const char *b) { return !strcmp(a, b); }

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
    } else if (f->sort == 3) {
        const char *a = x->name, *b = y->name;
        for (const char *p = x->name; *p; p++) if (*p == '.') a = p + 1;
        for (const char *p = y->name; *p; p++) if (*p == '.') b = p + 1;
        result = strcmp(a, b);
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
        FileMark *m = mark_for(e->path, false);
        if ((!f->show_hidden && e->name[0] == '.') ||
            (!match(e->name, f->query) && !(m && match(m->tag, f->query))) ||
            (f->color_filter > 0 && (!m || m->color != (unsigned)f->color_filter)) ||
            (f->color_filter == -1 && (!m || (!m->color && !m->tag[0])))) {
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
    DirectoryJob *j = &f->listing;
    int *count = j->preserve ? &j->scan_count : &f->count;
    if (*count == ENTRY_LIMIT) {
        if (j->preserve) j->scan_limited = true; else f->limited = true;
        return;
    }
    Entry *e = j->preserve ? &j->staged[(*count)++] : &f->entries[(*count)++];
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
    deallocate(j->staged, ENTRY_LIMIT * sizeof(Entry));
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
    watch_folder(w);
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
        if (j->preserve ? j->scan_limited : f->limited) {
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
    if (j->preserve) {
        if (j->active) return; /* Keep the previous complete view on screen. */
        if (!error) {
            char focused_path[OS_PATH_MAX] = "";
            if (f->visible_count) copy_text(focused_path, sizeof(focused_path),
                                           f->entries[f->order[f->focus]].path);
            for (int i = 0; i < f->count; i++) {
                if (!f->entries[i].selected) continue;
                for (int n = 0; n < j->scan_count; n++)
                    if (!strcmp(f->entries[i].path, j->staged[n].path)) {
                        j->staged[n].selected = true; break;
                    }
            }
            memcpy(f->entries, j->staged, j->scan_count * sizeof(Entry));
            f->count = j->scan_count;
            f->limited = j->scan_limited;
            filter(f);
            for (int i = 0; i < f->visible_count; i++)
                if (!strcmp(focused_path, f->entries[f->order[i]].path)) {
                    f->focus = i; break;
                }
        }
        deallocate(j->staged, ENTRY_LIMIT * sizeof(Entry));
        j->staged = NULL; j->preserve = false;
        previous_focus = -1;
        trace("FILES_AUTO_REFRESH", w, f->directory);
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
                  f->query[0]  ? "Search scope: names and tags in this folder."
                  : f->limited ? "Showing first 2048 entries. Use Terminal for the "
                                 "complete listing."
                               : "");
    }

    uint64_t now = now_ms();
    if (!j->active || now - j->last_update >= 50) {
        j->last_update = now;
        invalidate_window(w);
    }
}

/* One nonblocking queue serves every Files window and the desktop. */
static void watch_drop(Window *w) {
    Files *f = w->state;
    int id = f->watch; f->watch = -1;
    if (id < 0 || watch_fd < 0) return;
    for (int i = 0; i <= MAX_WINDOWS; i++) {
        Window *other = i == MAX_WINDOWS ? &desktop_files : &desk.windows[i];
        if (other != w && other->alive && other->app == APP_FILES && other->state &&
            ((Files *)other->state)->watch == id) return;
    }
    call2(SYS_inotify_rm_watch, watch_fd, id);
}
static void watch_folder(Window *w) {
    Files *f = w->state;
    if (watch_fd < 0) watch_fd = call1(SYS_inotify_init1, O_NONBLOCK | O_CLOEXEC);
    watch_drop(w);
    f->watch = -1; f->changed_at = 0;
    if (watch_fd >= 0 && f->directory[0] != '@')
        f->watch = call3(SYS_inotify_add_watch, watch_fd, f->directory,
                        2 | 4 | 8 | 64 | 128 | 256 | 512 | 1024 | 2048);
}
static void background_refresh(Window *w) {
    Files *f = w->state;
    DirectoryJob *j = &f->listing;
    if (j->active || f->job.active || f->dragging || f->directory[0] == '@') return;
    Entry *staged = allocate(ENTRY_LIMIT * sizeof(Entry));
    if (!staged) return;
    memset(j, 0, sizeof(*j));
    j->fd = -1; j->active = j->preserve = true; j->staged = staged; j->count = 1;
    copy_text(j->paths[0], OS_PATH_MAX, f->directory);
    f->changed_at = 0;
}
void files_watch_tick(void) {
    static uint64_t next;
    if (watch_fd < 0 || now_ms() < next) return;
    next = now_ms() + 32;
    struct { int wd; unsigned mask, cookie, len; } event;
    uint8_t bytes[4096];
    for (int pass = 0; pass < 4; pass++) {
        /* poll reads queue readiness without the VFS lock. An empty watch
           must never wait behind verification of a large Electron image. */
        struct os_pollfd poll = {watch_fd, POLLIN, 0};
        if (call3(SYS_poll, &poll, 1, 0) <= 0 || !(poll.revents & POLLIN)) break;
        long size = read(watch_fd, bytes, sizeof(bytes));
        if (size <= 0) break;
        for (long at = 0; at + (long)sizeof(event) <= size;) {
            memcpy(&event, bytes + at, sizeof(event));
            if (event.len > (unsigned)(size - at - sizeof(event))) break;
            at += sizeof(event) + event.len;
            for (int i = 0; i <= MAX_WINDOWS; i++) {
                Window *w = i == MAX_WINDOWS ? &desktop_files : &desk.windows[i];
                if (!w->alive || w->app != APP_FILES || !w->state) continue;
                Files *f = w->state;
                if ((event.mask & 16384) || f->watch == event.wd) {
                    if (!f->changed_at) f->changed_at = now_ms();
                    /* IN_IGNORED: rebind by pathname on the next reconciliation. */
                    if (event.mask & 32768) f->watch = -1;
                }
            }
        }
    }
    for (int i = 0; i <= MAX_WINDOWS; i++) {
        Window *w = i == MAX_WINDOWS ? &desktop_files : &desk.windows[i];
        if (!w->alive || w->app != APP_FILES || !w->state) continue;
        Files *f = w->state;
        if (f->changed_at && now_ms() - f->changed_at >= 100 &&
            !(desk.dialog.open && desk.dialog.owner == w) && !(desk.menu.open && desk.menu.owner == w)) {
            if (f->watch < 0) { watch_folder(w); f->changed_at = now_ms() - 100; }
            background_refresh(w);
        }
    }
}
void files_refresh_all(void) {
    if (desktop_files.alive) refresh(&desktop_files);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (desk.windows[i].alive && desk.windows[i].app == APP_FILES) {
            refresh(&desk.windows[i]);
        }
    }
}

int files_open(Window *w, const char *path) {
    marks_load();
    Files *f = allocate(sizeof(*f));
    if (!f) {
        return -E_NOMEM;
    }
    memset(f, 0, sizeof(*f));
    w->state = f;
    f->mode=desk.files_default_details;f->show_hidden=desk.files_default_hidden;
    f->drag_entry = -1;
    f->watch = -1;
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
    if(desk.trace)printf("DESKTOP FILES_DEFAULTS window=%d details=%d hidden=%d\n",w->id,f->mode,f->show_hidden);
    return 0;
}

static bool navigate(Window *w, const char *path, bool history) {
    Files *f = w->state;
    if (f->job.active) {
        dialog_notice(w, "File operation in progress",
                      "Wait for the copy or move to finish, or press Escape to "
                      "cancel it before "
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
    f->color_filter = 0;
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

static int sidebar_width(Window *w) { return w->box.w < S(600) ? S(132) : S(148); }

Rect files_sidebar_rect(Window *w) { Rect b=window_body(w); return (Rect){b.x,b.y,sidebar_width(w),b.h}; }

static Rect main_area(Window *w) {
    Rect b = window_body(w);
    int sidebar = sidebar_width(w);

    return (Rect){b.x + sidebar, b.y, b.w - sidebar, b.h};
}

static Rect content_view(Window *w) {
    Rect m = main_area(w);

    return (Rect){m.x + S(20), m.y + S(16), MAX(1, m.w - S(40)), MAX(1, m.h - S(44))};
}

enum { TOOL_NEW, TOOL_VIEW, TOOL_SORT, TOOL_MORE, TOOL_COUNT };
static Rect toolbar_search(Window *w) {
    Files *f = w->state;
    int width = f->input_focus == 2 || f->query[0]
                    ? clampi(w->box.w - S(316), S(24), S(124)) : S(24);
    return (Rect){w->box.x + w->box.w - S(12) - width, w->box.y + S(4), width, S(24)};
}
static Rect toolbar_button(Window *w, int tool) {
    Rect search = toolbar_search(w);
    return (Rect){search.x - S(12) - S(28) * (TOOL_COUNT - tool), search.y, S(24), S(24)};
}
static int toolbar_title_right(Window *w) { return toolbar_button(w, 0).x - S(10); }

static Rect entry_box(Window *w, int position) {
    Files *f = w->state;
    Rect v = content_view(w);
    int cols = v.w < S(480) ? 1 : 2;
    if (f->mode || position >= f->folders) {
        int offset = f->mode ? 0
                             : S(25) + (f->folders + cols - 1) / cols * S(52) +
                                   (f->folders ? S(25) : 0) + S(25);
        int row = f->mode ? position : position - f->folders;

        return (Rect){v.x, v.y + offset + S(24) + row * S(24) - f->scroll, v.w, S(24)};
    }

    int width = (v.w - S(8) * (cols - 1)) / cols;

    return (Rect){v.x + (position % cols) * (width + S(8)),
                  v.y + S(25) + (position / cols) * S(52) - f->scroll, width, S(46)};
}

static int content_height(Window *w) {
    Files *f = w->state;
    Rect v = content_view(w);
    int cols = v.w < S(480) ? 1 : 2;
    return f->mode ? S(24) + f->visible_count * S(24)
                   : S(25) + (f->folders + cols - 1) / cols * S(52) + (f->folders ? S(25) : 0) +
                         S(25) + S(24) + f->files * S(24);
}

static void number_label(int x, int y, int value, Font font, uint32_t color) {
    char n[32];
    format_uint(n, value);
    label(x, y, n, font, color, S(70));
}

void files_toolbar(Window *w) {
    Files *f = w->state;
    Rect b = w->box;
    fill((Rect){b.x + S(89), b.y + S(5), 1, S(18)}, theme_color(0xDFE5E4));
    for (int i = 0; i < 2; i++) {
        Rect r = {b.x + S(98 + i * 25), b.y + S(2), S(24), S(24)};
        bool enabled = i ? f->history_position + 1 < f->history_count : f->history_position > 0;
        if (enabled && contains(r, desk.mouse_x, desk.mouse_y)) {
            rounded(r, S(4), theme_color(0xEAF0ED));
        }
        icon(r.x + S(5), r.y + S(6), i ? I_FORWARD : I_BACK, S(14),
             enabled ? theme_color(0x69787E) : theme_color(0xC3CCCD));
    }

    int left = b.x + S(158), right = toolbar_title_right(w), available = MAX(1, right - left);
    if (f->input_focus == 1) {
        input_draw((Rect){left, b.y + S(2), available, S(24)}, f->path_input, "Path",
                   f->replace_input ? -1 : f->cursor, f->input_anchor, true);
    } else {
        Rect saved = desk.clip;
        clip_set(intersect(saved, (Rect){left, b.y, available, S(WINDOW_HEADER)}));
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
            label(x, b.y + S(7), "..", FONT_NORMAL, theme_color(0x919EA2), S(14));
            x += S(20);
        }
        for (int i = start; i < n; i++) {
            int tw = MIN(text_width(pieces[i], i == n - 1 ? FONT_MEDIUM : FONT_NORMAL),
                         MAX(1, right - x));
            label(x, b.y + S(7), pieces[i], i == n - 1 ? FONT_MEDIUM : FONT_NORMAL,
                  i == n - 1 ? theme_color(0x41525A) : theme_color(0x919EA2), tw);
            x += tw;
            if (i < n - 1) {
                icon(x + S(6), b.y + S(9), I_FORWARD, S(11), theme_color(0xAAB4B5));
                x += S(22);
            }
        }

        desk.clip = saved;
    }

    for (int i = 0; i < TOOL_COUNT; i++) {
        Rect r = toolbar_button(w, i);
        bool enabled = i != TOOL_NEW || f->directory[0] != '@';
        if (enabled && contains(r, desk.mouse_x, desk.mouse_y))
            rounded(r, S(5), THEME_TOOLBAR);
        if ((i == TOOL_NEW && f->keyboard_focus == 2) ||
            (i == TOOL_VIEW && f->keyboard_focus == 4))
            border(r, S(5), desk.accent);
        Icon kind = i == TOOL_NEW ? I_PLUS : i == TOOL_VIEW ? (f->mode ? I_LIST : I_GRID)
                         : i == TOOL_SORT ? I_SORT : I_MORE;
        icon(r.x + S(4), r.y + S(4), kind, S(16),
             enabled ? THEME_SECONDARY_TEXT : THEME_MUTED_TEXT);
    }
    Rect search = toolbar_search(w);
    if (f->query[0] || f->input_focus == 2) {
        input_draw(search, f->query, "Search folder",
                   f->input_focus == 2 ? (f->replace_input ? -1 : f->cursor) : -1,
                   f->input_anchor, f->input_focus == 2);
    } else {
        if (contains(search, desk.mouse_x, desk.mouse_y))
            rounded(search, S(5), THEME_TOOLBAR);
        icon(search.x + S(4), search.y + S(4), I_SEARCH, S(16), THEME_SECONDARY_TEXT);
    }
}

Rect files_toolbar_hover(Window *w, int x, int y) {
    Rect b = w->box;
    for (int i = 0; i < 2; i++) {
        Rect r = {b.x + S(98 + i * 25), b.y + S(2), S(24), S(24)};
        if (contains(r, x, y))
            return r;
    }
    for (int i = 0; i < TOOL_COUNT; i++) {
        Rect r = toolbar_button(w, i);
        if (contains(r, x, y)) return r;
    }
    if (contains(toolbar_search(w), x, y)) return toolbar_search(w);
    return (Rect){0};
}
bool files_text_hit(Window *w, int x, int y) {
    Files *f = w->state;
    Rect b = w->box;
    if ((f->input_focus == 2 || f->query[0]) && contains(toolbar_search(w), x, y))
        return true;
    return f->input_focus == 1 && contains((Rect){b.x + S(158), b.y + S(2),
        MAX(1, toolbar_title_right(w) - b.x - S(158)), S(24)}, x, y);
}

bool files_toolbar_mouse(Window *w, int x, int y, unsigned pressed) {
    Files *f = w->state;
    Rect b = w->box;
    if (!(pressed & 1)) {
        return false;
    }
    if (contains((Rect){b.x + S(98), b.y + S(2), S(24), S(24)}, x, y)) {
        history_step(w, -1);
        return true;
    }
    if (contains((Rect){b.x + S(123), b.y + S(2), S(24), S(24)}, x, y)) {
        history_step(w, 1);
        return true;
    }
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (contains(toolbar_button(w, i), x, y)) {
            toolbar_action(w, i);
            return true;
        }
    }
    if (contains(toolbar_search(w), x, y)) {
        f->input_focus = 2;
        f->cursor = strlen(f->query);
        copy_text(f->status, sizeof(f->status), "Search scope: names and tags in this folder.");
        invalidate_window(w);
        return true;
    }

    int left = b.x + S(158), right = toolbar_title_right(w);
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
    window_material(w, files_sidebar_rect(w), 1);
    fill((Rect){b.x + sw - 1, b.y, 1, b.h}, theme_color(0xE5EAE8));
    label(b.x + S(18), b.y + S(19), "Favorites", FONT_SMALL, theme_color(0xA3AFB0), sw - S(28));
    const char *names[] = {"Home", "Recent", "Downloads", "Projects", "Desktop", "Documents", "Pictures", "Trash"};
    Icon icons[] = {I_HOME, I_CLOCK, I_DOWNLOAD, I_FOLDER, I_LAYOUT, I_DOC, I_IMAGE, I_TRASH};
    for (int i = 0; i < 8; i++) {
        char dest[OS_PATH_MAX];
        if (i == 0) {
            copy_text(dest, sizeof(dest), desk.home);
        } else if (i == 1) {
            copy_text(dest, sizeof(dest), "@Recent");
        } else if (i == 7) {
            copy_text(dest, sizeof(dest), "@Trash");
        } else {
            join_path(dest, desk.home, names[i]);
        }
        Rect r = {b.x + S(9), b.y + S(34 + i * 22), sw - S(18), S(22)};
        bool selected = same_text(dest, f->directory),
             hover = contains(r, desk.mouse_x, desk.mouse_y);
        if (selected || hover) {
            rounded(r, S(5), selected ? desk.accent : theme_color(0xEBF0ED));
        }
        if (f->keyboard_focus == 3 && i == f->anchor) {
            border(r, S(5), desk.accent);
        }
        uint32_t c = selected ? THEME_ON_ACCENT : theme_color(0x617176);
        icon(r.x + S(9), r.y + S(5), icons[i], S(15), c);
        label(r.x + S(35), r.y + S(5), names[i], selected ? FONT_BOLD : FONT_BODY, c, r.w - S(44));
    }

    label(b.x + S(18), b.y + S(228), "Locations", FONT_SMALL, theme_color(0xA3AFB0),
          sw - S(28));
    Rect system = {b.x + S(9), b.y + S(248), sw - S(18), S(22)};
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
    Rect v = content_view(w);
    f->viewport = v;
    f->content_height = content_height(w);
    f->scroll = clampi(f->scroll, 0, MAX(0, f->content_height - v.h));
    f->columns = v.w < S(480) ? 1 : 2;
    clip_set(intersect(saved, v));
    int files_top = f->mode ? v.y - f->scroll
                            : v.y + S(25) + (f->folders + f->columns - 1) / f->columns * S(52) +
                                  (f->folders ? S(25) : 0) - f->scroll;
    if (!f->mode) {
        label(v.x, v.y - f->scroll, "Folders", FONT_BOLD, theme_color(0x344449), v.w);
        number_label(v.x + S(52), v.y + S(2) - f->scroll, f->folders, FONT_SMALL,
                     theme_color(0xA7B0B1));
        label(v.x, files_top, "Files", FONT_BOLD, theme_color(0x344449), v.w);
        number_label(v.x + S(33), files_top + S(2), f->files, FONT_SMALL, theme_color(0xA7B0B1));
        files_top += S(25);
    }

    int modified_x = v.x + v.w - S(167), size_x = v.x + v.w - S(44);
    bool metadata = v.w > S(370);
    label(v.x + S(4), files_top + S(6), "Name", FONT_SMALL, theme_color(0xA4AFB0),
          MAX(S(60), v.w - S(metadata ? 185 : 50)));
    if (metadata) {
        label(modified_x, files_top + S(6), "Modified", FONT_SMALL, theme_color(0xA4AFB0), S(105));
    }
    label(size_x, files_top + S(6), "Size", FONT_SMALL, theme_color(0xA4AFB0), S(36));
    fill((Rect){v.x, files_top + S(23), v.w, 1}, theme_color(0xE9EDED));
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
            border(r, S(7), e->selected ? theme_color(0xB8CDEB) : THEME_BORDER);
            Rect ib = {r.x + S(10), r.y + S(11), S(28), S(28)};
            rounded(ib, S(6), theme_color(bg[i % 4]));
            FileMark *mark = mark_for(e->path, false);
            icon(ib.x + S(6), ib.y + S(6), I_FOLDER, S(18), mark && mark->color ? mark_colors[mark->color] : fg[i % 4]);
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

            label(r.x + S(52), r.y + S(26), mark && mark->tag[0] ? mark->tag : modified, FONT_SMALL,
                  mark && mark->color ? mark_colors[mark->color] : theme_color(0xA2ADAF),
                  r.w - S(63));
        } else {
            if (e->selected || hover) {
                rounded(r, S(4), e->selected ? desk.selection : theme_color(0xF6F8F7));
            }
            bool json = strlen(e->name) > 5 && same_text(e->name + strlen(e->name) - 5, ".json");
            rounded((Rect){r.x + S(4), r.y + S(4), S(16), S(18)}, S(4),
                    folder ? theme_color(0xE3F0E9)
                    : json ? theme_color(0xFAF0E6)
                           : theme_color(0xEAF0F6));
            FileMark *mark = mark_for(e->path, false);
            icon(r.x + S(8), r.y + S(5), folder ? I_FOLDER : I_DOC, S(13),
                 mark && mark->color ? mark_colors[mark->color] : folder ? 0x70A489
                 : json ? 0xCB9569
                        : 0x7D9AB8);
            label(r.x + S(37), r.y + S(6), e->name, FONT_MEDIUM, THEME_TEXT,
                  MAX(S(25), r.w - S(metadata ? 222 : 94)));
            char modified[32], size[32];
            modification(modified, e);
            format_size(size, e->st.size);
            if (metadata) {
                label(modified_x, r.y + S(6), mark && mark->tag[0] ? mark->tag : modified,
                      FONT_NORMAL, mark && mark->color ? mark_colors[mark->color] : THEME_SECONDARY_TEXT, S(105));
            }
            if (!folder) {
                label(r.x + r.w - S(4) - text_width(size, FONT_NORMAL), r.y + S(6), size,
                      FONT_NORMAL, THEME_SECONDARY_TEXT, S(55));
            }
            fill((Rect){r.x, r.y + r.h - 1, r.w, 1}, THEME_DIVIDER);
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
        format_uint(status, f->visible_count);
        append_text(status, sizeof(status), f->visible_count == 1 ? " item" : " items");
        if (f->limited) append_text(status, sizeof(status), " (first 2048 entries)");
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
        if (f->desktop) wm_open(APP_FILES, e->path);
        else navigate(w, e->path, true);
        return;
    }
    if ((e->st.mode & 0170000) != S_IFREG) {
        dialog_notice(w, "Open With",
                      "This file type has no registered GUI application. Device "
                      "nodes and special "
                      "files should be accessed through Terminal.");
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
                          "No GUI application is registered for this binary file. "
                          "Text documents "
                          "open in Text Editor. Use Terminal for supported "
                          "executable programs.");
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
        e = desktop_create_directory(path, 0755);
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
        int fd = desktop_create_file(path, 0644);
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
        marks_moved(e->path, path);
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
        if (!e) marks_moved(entry->path, target);
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
        marks_moved(e->path, original);
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
        if (!moved) marks_moved(source, target);
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
        e = desktop_create_directory(target, st.mode & 0777);
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
    j->output = desktop_create_file(target, (st.mode & 0777) | 0600);
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

static void location(Window *w, const char *path) { navigate(w, path, true); }

static void annotations_changed(void) {
    for (int i=0;i<=MAX_WINDOWS;i++) {
        Window *w=i==MAX_WINDOWS?&desktop_files:&desk.windows[i];
        if (w->alive && w->app==APP_FILES && w->state) { filter(w->state); invalidate_window(w); }
    }
    invalidate_all();
}
static const char *context_path(Files *f) {
    Entry *e=selected_one(f);
    return !f->context_current && e ? e->path : f->directory;
}
static void annotate(Window *w, int color, const char *tag, bool favorite) {
    Files *f=w->state;
    FileMark *before=allocate(sizeof(marks)); if (!before) { dialog_error(w,"Tags",-E_NOMEM);return; }
    memcpy(before,marks,sizeof(marks)); int old_count=mark_count, error=0;
    int total=f->context_current?1:MAX(1,selected_count(f));
    for (int i=0,done=0;i<MAX(f->count,1) && done<total;i++) {
        const char *path=NULL;
        if (f->context_current) path=f->directory;
        else if (selected_count(f)) { if (f->entries[i].selected) path=f->entries[i].path; }
        else { Entry *e=selected_one(f); if (e) path=e->path; }
        if (!path) continue;
        done++; FileMark *m=mark_for(path,true); if (!m) { error=-E_NOSPC;break; }
        if (color>=0) m->color=color;
        if (tag) copy_text(m->tag,sizeof(m->tag),tag);
        if (favorite) m->favorite=!m->favorite;
    }
    if (!error) error=marks_save();
    if (error) { memcpy(marks,before,sizeof(marks));mark_count=old_count;dialog_error(w,"Save file tags",error); }
    else { trace("FILE_TAG",w,context_path(f));annotations_changed(); }
    deallocate(before,sizeof(marks));
}
static void set_tag(Window *w,const char *tag) {
    if(strlen(tag)>=sizeof(marks[0].tag)) { dialog_error(w,"Tag is too long",-E_NAMETOOLONG);return; }
    annotate(w,-1,tag,false);
}
static void transfer_to(Window *w,const char *path) {
    Files *f=w->state; char destination[OS_PATH_MAX]; struct os_stat st;
    int error=normalize_path(destination,f->directory,path);
    if (!error) error=stat(destination,&st);
    if (!error && (st.mode&0170000)!=S_IFDIR) error=-E_NOTDIR;
    if (error) { dialog_error(w,"Destination folder",error);return; }
    paste(w,destination);
}
static void template_file(Window *w,const char *name) {
    Files *f=w->state; char path[OS_PATH_MAX];
    create_file(w,name);
    if (desk.dialog.open || join_path(path,f->directory,name)) return;
    const char *text=strlen(name)>3 && !strcmp(name+strlen(name)-3,".sh")?"#!/bin/sh\n\n":
                     strlen(name)>3 && !strcmp(name+strlen(name)-3,".md")?"# Untitled\n\n":"";
    int fd=open(path,O_WRONLY|O_NOFOLLOW,0);
    int error=fd<0?fd:write_all(fd,text,strlen(text));
    if (!error) error=fsync(fd);
    if (fd>=0) close(fd);
    if (error) dialog_error(w,"New document",error);
    files_refresh_all();
}

static void action(Window *w, int operation) {
    if (!w || !w->alive) {
        return;
    }
    Files *f = w->state;
    marks_load();
    Entry *e = selected_one(f);
    if (f->job.active && operation != A_INFO) {
        dialog_notice(w, "Operation in progress",
                      "Wait or press Escape to cancel the current operation.");
        return;
    }
    if (operation>=A_COLOR_BASE && operation<A_COLOR_BASE+8) { annotate(w,operation-A_COLOR_BASE,NULL,false);return; }
    if (operation>=A_FILTER_BASE && operation<=A_FILTER_BASE+8) {
        f->color_filter=operation==A_FILTER_BASE+8?-1:operation-A_FILTER_BASE;
        filter(f);f->scroll=0;invalidate_window(w);return;
    }
    switch (operation) {
    case A_TAG: {
        FileMark *m=mark_for(context_path(f),false);
        dialog_input(w,"Edit tag","A searchable label for the selected items (up to 47 bytes).",m?m->tag:"",set_tag);break;
    }
    case A_CLEAR_TAG: annotate(w,0,"",false);break;
    case A_FAVORITE: annotate(w,-1,NULL,true);break;
    case A_COPY_TO: case A_MOVE_TO:
        if (clipboard_set(w,operation==A_MOVE_TO)) dialog_input(w,operation==A_MOVE_TO?"Move to folder":"Copy to folder",
            "Enter a destination folder. Existing items are never overwritten.",desk.home,transfer_to);
        break;
    case A_COPY_NAME: case A_COPY_PARENT: {
        char path[OS_PATH_MAX]; const char *value=context_path(f);
        if (operation==A_COPY_NAME) value=basename(value);
        else { normalize_path(path,value,"..");value=path; }
        int error=clipboard_write(value,strlen(value)); if (error) dialog_error(w,"Copy",error);break;
    }
    case A_HIDE:
        if (e && !e->link) {
            char name[OS_NAME_MAX+1];
            if (e->name[0]=='.') copy_text(name,sizeof(name),e->name+1);
            else { copy_text(name,sizeof(name),".");append_text(name,sizeof(name),e->name); }
            if (strlen(e->name)+(e->name[0]!='.')>OS_NAME_MAX) dialog_error(w,"Hide item",-E_NAMETOOLONG);
            else rename_entry(w,name);
        }
        break;
    case A_READONLY: case A_EXECUTABLE:
        if (e && !e->link) {
            int fd=open(e->path,O_RDONLY|O_NOFOLLOW,0); struct os_stat st;
            int error=fd<0?fd:fstat(fd,&st);
            if (!error) error=call2(SYS_fchmod,fd,operation==A_READONLY?
                ((st.mode&0222)?(st.mode&0777&~0222):((st.mode&0777)|0200)):(st.mode&0777)^0100);
            if (fd>=0) close(fd);
            if (error) dialog_error(w,"File permissions",error);else files_refresh_all();
        }
        break;
    case A_SHORTCUT:
        if (e) {
            char root[OS_PATH_MAX],target[OS_PATH_MAX];join_path(root,desk.home,"Desktop");
            int error=join_path(target,root,e->name);
            if (!error) error=call2(SYS_symlink,e->path,target);
            if (error) dialog_error(w,"Desktop shortcut",error);else files_refresh_all();
        }
        break;
    case A_SELECT_NONE: case A_SELECT_FILES: case A_SELECT_FOLDERS: case A_SELECT_TYPE: {
        char suffix[OS_NAME_MAX+1]="";
        if (e) { const char *p=e->name;for(const char *q=e->name;*q;q++) if(*q=='.')p=q;copy_text(suffix,sizeof(suffix),p); }
        for(int i=0;i<f->visible_count;i++) {
            Entry *item=&f->entries[f->order[i]];bool folder=(item->st.mode&0170000)==S_IFDIR;
            const char *ext=item->name;for(const char *q=item->name;*q;q++)if(*q=='.')ext=q;
            item->selected=operation==A_SELECT_FILES?!folder:operation==A_SELECT_FOLDERS?folder:
                           operation==A_SELECT_TYPE && !folder && !strcmp(ext,suffix);
        }
        invalidate_window(w);break;
    }
    case A_CLEAR_FILTER: f->query[0]=0;f->color_filter=0;filter(f);invalidate_window(w);break;
    case A_TEXT_FILE: case A_MARKDOWN_FILE: case A_SCRIPT_FILE:
        dialog_input(w,"New document","Enter a filename in the current folder.",
            operation==A_TEXT_FILE?"Untitled.txt":operation==A_MARKDOWN_FILE?"Untitled.md":"script.sh",template_file);break;
    case A_FOLDER_INFO: {
        char text[640]; struct os_stat st; int error=stat(f->directory,&st);
        if(error)dialog_error(w,"Folder properties",error);
        else { copy_text(text,sizeof(text),f->directory);append_text(text,sizeof(text),"\nVisible contents: ");
            char n[32];format_uint(n,f->visible_count);append_text(text,sizeof(text),n);
            append_text(text,sizeof(text)," items\nOwner UID: ");format_uint(n,st.uid);append_text(text,sizeof(text),n);
            FileMark *m=mark_for(f->directory,false);if(m&&m->tag[0]){append_text(text,sizeof(text),"\nTag: ");append_text(text,sizeof(text),m->tag);}
            dialog_notice(w,"Folder properties",text); }
        break;
    }
    case A_TERMINAL_CURRENT:
        if (f->directory[0] != '@') wm_open(APP_TERMINAL, f->directory);
        break;
    case A_NEW_WINDOW_CURRENT:
        wm_open(APP_FILES, f->directory);
        break;
    case A_TERMINAL:
        if (f->directory[0] != '@' || (e && (e->st.mode & 0170000) == S_IFDIR))
            wm_open(APP_TERMINAL, e && (e->st.mode & 0170000) == S_IFDIR ? e->path : f->directory);
        break;
    case A_NEW_WINDOW:
        wm_open(APP_FILES, e && (e->st.mode & 0170000) == S_IFDIR ? e->path : f->directory);
        break;
    case A_EDITOR:
        if (e && (e->st.mode & 0170000) == S_IFREG) wm_open(APP_EDITOR, e->path);
        break;
    case A_COPY_PATH: {
        char *text = allocate(OS_CLIPBOARD_LIMIT + 1); size_t used = 0;
        if (!text) { dialog_error(w, "Copy path", -E_NOMEM); break; }
        for (int i = 0; i < f->count; i++) if (f->entries[i].selected) {
            size_t length = strlen(f->entries[i].path);
            if (used + length + 1 >= OS_CLIPBOARD_LIMIT + 1) break;
            if (used) text[used++] = '\n';
            memcpy(text + used, f->entries[i].path, length); used += length;
        }
        if (!used) { copy_text(text, OS_CLIPBOARD_LIMIT + 1, f->directory); used = strlen(text); }
        int error = clipboard_write(text, used);
        deallocate(text, OS_CLIPBOARD_LIMIT + 1);
        if (error) dialog_error(w, "Copy path", error);
        break;
    }
    case A_SELECT_ALL:
    case A_INVERT:
        for (int i = 0; i < f->visible_count; i++) {
            Entry *item = &f->entries[f->order[i]];
            item->selected = operation == A_SELECT_ALL || !item->selected;
        }
        invalidate_window(w); break;
    case A_DETAILS: case A_ICONS:
        f->mode = operation == A_DETAILS;
        invalidate_window(w); break;
    case A_SORT_NAME: case A_SORT_DATE: case A_SORT_SIZE: case A_SORT_TYPE:
        f->sort = operation - A_SORT_NAME; filter(f); invalidate_window(w); break;
    case A_DESCENDING:
        f->descending = !f->descending; filter(f); invalidate_window(w); break;
    case A_LOCATION:
        dialog_input(w, "Go to folder", "Enter an absolute or relative folder path.", f->directory,
                     location); break;
    case A_DESKTOP: {
        char path[OS_PATH_MAX]; join_path(path, desk.home, "Desktop");
        if (f->desktop) wm_open(APP_FILES, path); else navigate(w, path, true);
        break;
    }
    case A_DUPLICATE:
        if (e && selected_count(f) == 1 && f->directory[0] != '@') {
            char target[OS_PATH_MAX], name[OS_NAME_MAX + 1], number[24];
            struct os_stat st;
            int error = -E_EXIST;
            for (int i = 1; i <= 999; i++) {
                const char *extension = e->name + strlen(e->name);
                if ((e->st.mode & 0170000) == S_IFREG)
                    for (const char *p=e->name+1;*p;p++) if (*p=='.') extension=p;
                copy_text(name, sizeof(name), e->name);
                name[extension-e->name]=0;
                format_uint(number, i);
                if (strlen(e->name) + strlen(number) + 6 > OS_NAME_MAX) { error = -E_NAMETOOLONG; break; }
                append_text(name, sizeof(name), " copy "); append_text(name, sizeof(name), number);
                append_text(name, sizeof(name), extension);
                error = join_path(target, f->directory, name);
                if (error || stat(target, &st) == -E_NOENT) break;
                error = -E_EXIST;
            }
            if (!error) {
                CopyJob *j = &f->job;
                memset(j, 0, sizeof(*j)); j->input = j->output = -1;
                j->active = true; j->count = j->item = 1;
                copy_text(j->destination, sizeof(j->destination), f->directory);
                error = copy_begin(j, e->path, target);
                if (error) job_cancel(w, true, error);
            } else dialog_error(w, "Duplicate", error);
        }
        break;
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
                           "This cannot be undone. Empty folders and regular files "
                           "can be removed; "
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
            FileMark *m=mark_for(e->path,false);
            if (m) {
                append_text(b,sizeof(b),"\nColor: ");append_text(b,sizeof(b),color_names[m->color]);
                append_text(b,sizeof(b),"    Tag: ");append_text(b,sizeof(b),m->tag);
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

static void view_menu(Window *w, int x, int y) {
    Files *f = w->state;
    MenuItem items[] = {
        {"Details view", f->mode ? I_CHECK : I_LAYOUT, A_DETAILS, true},
        {"Icon view", !f->mode ? I_CHECK : I_GRID, A_ICONS, true},
        {"Sort by name", f->sort == 0 ? I_CHECK : I_DOC, A_SORT_NAME, true},
        {"Sort by modified time", f->sort == 1 ? I_CHECK : I_CLOCK, A_SORT_DATE, true},
        {"Sort by size", f->sort == 2 ? I_CHECK : I_LAYOUT, A_SORT_SIZE, true},
        {"Sort by type", f->sort == 3 ? I_CHECK : I_DOC, A_SORT_TYPE, true},
        {f->descending ? "Ascending order" : "Descending order", I_LAYOUT, A_DESCENDING, true},
        {"Select all", I_CHECK, A_SELECT_ALL, true},
        {"Invert selection", I_CHECK, A_INVERT, true},
        {"Deselect all", I_CHECK, A_SELECT_NONE, true},
        {"Select files only", I_DOC, A_SELECT_FILES, true},
        {"Select folders only", I_FOLDER, A_SELECT_FOLDERS, true},
        {"Select same file extension", I_DOC, A_SELECT_TYPE, selected_one(f)!=NULL},
        {"Filter by color or tag...", I_SEARCH, 203, true},
        {"Clear search and tag filter", I_RELOAD, A_CLEAR_FILTER, true},
        {f->show_hidden ? "Hide hidden files" : "Show hidden files", I_LAYOUT, A_HIDDEN, true}};
    menu_open(w, x, y, items, sizeof(items) / sizeof(*items), files_context_action);
}
static void toolbar_action(Window *w, int tool) {
    Files *f = w->state;
    Rect r = toolbar_button(w, tool);
    if (tool == TOOL_NEW && f->directory[0] == '@') return;
    f->input_focus = 0;
    f->context_current = selected_count(f) == 0;
    if (tool == TOOL_VIEW) {
        f->mode = !f->mode;
        f->scroll = 0;
        smooth_jump(&f->animation, 0, now_ms());
        trace("VIEW", w, f->mode ? "details" : "hybrid");
    } else if (tool == TOOL_SORT) {
        view_menu(w, r.x, w->box.y + S(WINDOW_HEADER + 4));
    } else {
        desk.menu.box = (Rect){r.x, w->box.y + S(WINDOW_HEADER + 4), 0, 0};
        files_context_action(w, tool == TOOL_NEW ? 206 : 201);
    }
    invalidate_window(w);
}
static void files_context_action(Window *w, int operation) {
    Files *f=w->state; int x=desk.menu.box.x,y=desk.menu.box.y;
    if (operation>=300 && operation<300+MARK_LIMIT) {
        FileMark *m=&marks[operation-300];struct os_stat st;int error=stat(m->path,&st);
        if(error)dialog_error(w,"Open favorite",error);
        else if((st.mode&0170000)==S_IFDIR) { if(f->desktop)wm_open(APP_FILES,m->path);else navigate(w,m->path,true); }
        else wm_open(APP_EDITOR,m->path);
        return;
    }
    if (operation == 200) { view_menu(w, desk.menu.box.x, desk.menu.box.y); return; }
    if (operation == 201) {
        Entry *e=f->context_current?NULL:selected_one(f); bool has=e!=NULL, regular=has&&(e->st.mode&0170000)==S_IFREG;
        FileMark *m=mark_for(context_path(f),false);
        MenuItem items[]={
            {"Color label...",I_IMAGE,202,true}, {"Tags...",I_RENAME,A_TAG,true},
            {"Clear color and tag",I_RELOAD,A_CLEAR_TAG,true},
            {m&&m->favorite?"Remove from favorites":"Add to favorites",I_HOME,A_FAVORITE,true},
            {"Copy to folder...",I_COPY,A_COPY_TO,has}, {"Move to folder...",I_CUT,A_MOVE_TO,has},
            {"Copy name",I_COPY,A_COPY_NAME,true}, {"Copy parent folder path",I_COPY,A_COPY_PARENT,true},
            {"Create shortcut on Desktop",I_LAYOUT,A_SHORTCUT,has},
            {e&&e->name[0]=='.'?"Unhide item":"Hide item",I_DOC,A_HIDE,has&&!e->link},
            {e&&(e->st.mode&0222)?"Make read-only":"Allow owner to write",I_INFO,A_READONLY,has&&!e->link},
            {e&&(e->st.mode&0100)?"Remove executable permission":"Make executable",I_TERMINAL,A_EXECUTABLE,regular&&!e->link},
            {"Open with...",I_DOC,205,regular}, {"New document...",I_DOC,206,f->directory[0]!='@'},
            {"Copy\tCtrl+C",I_COPY,A_COPY,has},
            {"Paste\tCtrl+V",I_PASTE,A_PASTE,clipboard.count>0&&f->directory[0]!='@'},
            {"Open terminal here",I_TERMINAL,A_TERMINAL_CURRENT,f->directory[0]!='@'},
            {has?"Properties":"Folder properties",I_INFO,has?A_INFO:A_FOLDER_INFO,true}};
        menu_open(w,x,y,items,sizeof(items)/sizeof(*items),files_context_action);return;
    }
    if(operation==202 || operation==203) {
        MenuItem items[9];FileMark *m=mark_for(context_path(f),false);
        for(int i=0;i<8;i++)items[i]=(MenuItem){color_names[i],
            operation==202&&m&&m->color==(unsigned)i?I_CHECK:I_FOLDER,
            (operation==202?A_COLOR_BASE:A_FILTER_BASE)+i,true};
        int n=8;
        if(operation==203) {items[0].label="All colors";items[n++]=(MenuItem){"Any color or text tag",I_SEARCH,A_FILTER_BASE+8,true};}
        if(operation==202)menu_palette(w,x,y,items,mark_colors,m?m->color:0,files_context_action);
        else menu_open(w,x,y,items,n,files_context_action);
        return;
    }
    if(operation==204) {
        MenuItem items[MAX_MENU_ITEMS];int n=0;
        for(int i=0;i<mark_count&&n<MAX_MENU_ITEMS;i++)if(marks[i].favorite)
            items[n++]=(MenuItem){marks[i].path,I_HOME,300+i,true};
        if(!n)items[n++]=(MenuItem){"Add favorites using Organize and tags",I_HOME,0,false};
        menu_open(w,x,y,items,n,files_context_action);return;
    }
    if(operation==205) {
        MenuItem items[]={{"Text Editor",I_DOC,A_EDITOR,true}};
        menu_open(w,x,y,items,1,files_context_action);return;
    }
    if(operation==206) {
        MenuItem items[]={{"Empty file...",I_DOC,A_FILE,true},{"Text document...",I_DOC,A_TEXT_FILE,true},
            {"Markdown document...",I_DOC,A_MARKDOWN_FILE,true},{"Shell script...",I_TERMINAL,A_SCRIPT_FILE,true},
            {"Folder...",I_FOLDER,A_FOLDER,true}};
        menu_open(w,x,y,items,5,files_context_action);return;
    }
    action(w, operation);
}
static void context_menu(Window *w, int x, int y, bool entry) {
    Files *f = w->state;
    f->context_current=!entry;
    bool ordinary = f->directory[0] != '@';
    Entry *e = entry ? selected_one(f) : NULL;
    bool folder = e && (e->st.mode & 0170000) == S_IFDIR;
    MenuItem items[MAX_MENU_ITEMS]; int n = 0;
    if(entry)items[n++]=(MenuItem){"Open",I_FOLDER,A_OPEN,true};
    items[n++]=(MenuItem){"Open terminal here",I_TERMINAL,folder?A_TERMINAL:A_TERMINAL_CURRENT,ordinary||folder};
    if(entry) {
        items[n++]=(MenuItem){"Copy\tCtrl+C",I_COPY,A_COPY,true};
        items[n++]=(MenuItem){"Cut\tCtrl+X",I_CUT,A_CUT,ordinary};
        items[n++]=(MenuItem){"Rename\tF2",I_RENAME,A_RENAME,ordinary};
        items[n++]=(MenuItem){same_text(f->directory,"@Trash")?"Delete permanently":"Move to Trash",I_TRASH,A_TRASH,true};
        if(same_text(f->directory,"@Trash"))items[n++]=(MenuItem){"Restore from Trash",I_RESTORE,A_RESTORE,true};
        if(wallpaper_entry(e))items[n++]=(MenuItem){"Set as wallpaper",I_IMAGE,A_WALLPAPER,true};
    } else {
        items[n++]=(MenuItem){"New folder",I_FOLDER,A_FOLDER,ordinary};
        items[n++]=(MenuItem){"New document...",I_DOC,206,ordinary};
    }
    items[n++]=(MenuItem){"Paste\tCtrl+V",I_PASTE,A_PASTE,ordinary&&clipboard.count>0};
    items[n++]=(MenuItem){"Tags...",I_RENAME,A_TAG,ordinary||entry};
    FileMark *mark=mark_for(context_path(f),false);
    items[n++]=(MenuItem){mark&&mark->favorite?"Remove from favorites":"Add to favorites",I_HOME,A_FAVORITE,ordinary||entry};
    items[n++]=(MenuItem){entry?"Properties":"Folder properties",I_INFO,entry?A_INFO:A_FOLDER_INFO,true};
    menu_open(w, x, y, items, n, files_context_action);
    if(ordinary||entry) {
      MenuItem colors[8];for(int i=0;i<8;i++)colors[i]=(MenuItem){color_names[i],I_FOLDER,A_COLOR_BASE+i,true};
      menu_color_row(colors,mark_colors,mark?mark->color:0);
    }
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
            int row = (y - b.y - S(34)) / S(22);
            if (y >= b.y + S(34) && row >= 0 && row < 8) {
                const char *names[] = {"",         "@Recent",  "Downloads",
                                       "Projects", "Desktop", "Documents", "Pictures", "@Trash"};
                char path[OS_PATH_MAX];
                if (!row) {
                    copy_text(path, sizeof(path), desk.home);
                } else if (row == 1 || row == 7) {
                    copy_text(path, sizeof(path), names[row]);
                } else {
                    join_path(path, desk.home, names[row]);
                }
                navigate(w, path, true);
                return true;
            }

            if (contains((Rect){b.x + S(9), b.y + S(248), sidebar_width(w) - S(18), S(22)}, x, y)) {
                navigate(w, "/", true);
                return true;
            }
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
            for (int i = 0; i < f->count; i++) f->entries[i].selected = false;
            context_menu(w, x, y, false);
            return true;
        }
        int file_start = f->mode
                             ? v.y - f->scroll
                             : v.y + S(25) + (f->folders + f->columns - 1) / f->columns * S(52) +
                                   (f->folders ? S(25) : 0) + S(25) - f->scroll;
        if ((pressed & 1) && y >= file_start && y < file_start + S(24)) {
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
        copy_text(f->status, sizeof(f->status), "Search scope: names and tags in this folder.");
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
        if (f->replace_input) {
            f->cursor = strlen(text);
            f->input_anchor = 0;
            f->replace_input = false;
        }
        if (input_key(text, f->input_focus == 1 ? sizeof(f->path_input) : sizeof(f->query),
                      &f->cursor, &f->input_anchor, k)) {
            if (f->input_focus == 2) {
                filter(f);
                f->scroll = 0;
                smooth_jump(&f->animation, 0, now_ms());
            }
            invalidate_window(w);
        }

        return;
    }

    if (k.code == 0x65 || (k.code == 0x43 && (k.mods & MOD_SHIFT))) {
        context_menu(w,desk.mouse_x,desk.mouse_y,selected_one(f)!=NULL);return;
    }
    if (k.code == KEY_ENTER && (k.mods & MOD_ALT)) { action(w,A_INFO);return; }
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
            f->anchor = clampi(f->anchor + (k.code == KEY_UP ? -1 : 1), 0, 7);
            invalidate_window(w);
            return;
        }
        if (k.code == KEY_ENTER) {
            Rect b = window_body(w);
            files_mouse(w, b.x + S(50), b.y + S(45 + f->anchor * 22), 1, 0);
            return;
        }
    }

    if (k.code == KEY_ENTER && f->keyboard_focus == 2) {
        toolbar_action(w, TOOL_NEW);
        return;
    }
    if (k.code == KEY_ENTER && f->keyboard_focus == 4) {
        toolbar_action(w, TOOL_VIEW);
        return;
    }

    if (k.mods & MOD_CTRL) {
        unsigned a = k.ascii | 32;
        if (a == 'a') {
            for (int i = 0; i < f->count; i++) {
                f->entries[i].selected = false;
            }
            for (int i = 0; i < f->visible_count; i++) {
                f->entries[f->order[i]].selected = !(k.mods & MOD_SHIFT);
            }
            invalidate_window(w);
            return;
        }

        if (a == 'c') {
            action(w, (k.mods & MOD_SHIFT) ? A_COPY_PATH : A_COPY);
            return;
        }
        if (a == 'd') { action(w,A_DUPLICATE);return; }
        if (a == 't') { action(w,A_TERMINAL_CURRENT);return; }
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
            next += (k.code == KEY_PAGEUP ? -1 : 1) * MAX(1, f->viewport.h / S(24));
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
    if (f->listing.active && f->listing.fd >= 0) close(f->listing.fd);
    watch_drop(w);
    deallocate(f->listing.staged, ENTRY_LIMIT * sizeof(Entry));
    deallocate(f, sizeof(*f));
    w->state = NULL;
    return true;
}

Rect files_hover(Window *w, int x, int y) {
    Rect b = window_body(w), v = content_view(w);
    Files *f = w->state;
    if (x < b.x + sidebar_width(w) && y >= b.y + S(34) && y < b.y + S(210)) {
        return (Rect){b.x + S(9), b.y + S(34) + ((y - b.y - S(34)) / S(22)) * S(22),
                      sidebar_width(w) - S(18), S(22)};
    }
    if (contains(v, x, y)) {
        for (int i = 0; i < f->visible_count; i++) {
            Rect r = entry_box(w, i);
            if (contains(r, x, y)) {
                return intersect(r, v);
            }
        }
    }

    return (Rect){0};
}

/* Application menus reuse the same operations as the context menu and keys. */
static void files_command(Window *w, int command) {
    Files *f = w->state;
    if (command >= 200) { files_context_action(w,command);return; }
    if (command < 100) {
        action(w, command);
        return;
    }
    if (command == 100) {
        wm_open(APP_FILES, f->directory);
        return;
    }
    if (command == 101) {
        wm_close(w);
        return;
    }
    if (command == 102) {
        menu_send_key(w, 'a', 4, MOD_CTRL);
        return;
    }
    if (command == 103 || command == 115) {
        menu_send_key(w, command == 103 ? 'f' : 'l', command == 103 ? 9 : 15, MOD_CTRL);
        return;
    }
    if (command == 116 || command == 117) {
        history_step(w, command == 116 ? -1 : 1);
        return;
    }
    if (command >= 110 && command <= 114) {
        char path[OS_PATH_MAX];
        const char *places[] = {"", "Downloads", "Projects", "@Recent", "@Trash"};
        if (command >= 113)
            copy_text(path, sizeof(path), places[command - 110]);
        else if (command == 110)
            copy_text(path, sizeof(path), desk.home);
        else
            join_path(path, desk.home, places[command - 110]);
        navigate(w, path, true);
        return;
    }
    if (command == 104 || command == 105)
        f->mode = command == 104;
    else if (command >= 106 && command <= 108) {
        f->sort = command - 106;
        f->descending = false;
        filter(f);
    } else if (command == 109) {
        f->descending = !f->descending;
        filter(f);
    }
    f->scroll = 0;
    smooth_jump(&f->animation, 0, now_ms());
    invalidate_window(w);
    trace("VIEW", w, f->mode ? "details" : "hybrid");
}
void files_menu(Window *w, int group, int x, int y) {
    Files *f = w->state;
    f->context_current=selected_count(f)==0;
    bool ordinary = f->directory[0] != '@', has = selected_one(f) != NULL;
    if (group == MENU_FILE) {
        MenuItem items[] = {
            {"New window\tSuper+E", I_FOLDER, 100, true},
            {"Open terminal here", I_TERMINAL, A_TERMINAL_CURRENT, ordinary},
            {"Copy path", I_COPY, A_COPY_PATH, true},
            {"New file\tCtrl+N", I_DOC, A_FILE, ordinary},
            {"New folder\tCtrl+Shift+N", I_FOLDER, A_FOLDER, ordinary},
            {"Open selection\tEnter", I_FOLDER, A_OPEN, has},
            {"Rename\tF2", I_RENAME, A_RENAME, has && !same_text(f->directory, "@Trash")},
            {same_text(f->directory, "@Trash") ? "Delete permanently\tDelete"
                                               : "Move to Trash\tDelete",
             I_TRASH, A_TRASH, has},
            {"Restore from Trash", I_RESTORE, A_RESTORE, has && same_text(f->directory, "@Trash")},
            {"File information", I_INFO, A_INFO, has},
            {"Organize, colors and tags...", I_FOLDER, 201, has || ordinary},
            {"New document...", I_DOC, 206, ordinary},
            {"Close window\tAlt+F4", I_CLOSE, 101, true}};
        menu_open(w, x, y, items, sizeof(items) / sizeof(*items), files_command);
    } else if (group == MENU_EDIT) {
        MenuItem items[] = {{"Copy\tCtrl+C", I_COPY, A_COPY, has},
                            {"Cut\tCtrl+X", I_CUT, A_CUT, has},
                            {"Paste\tCtrl+V", I_PASTE, A_PASTE, ordinary && clipboard.count > 0},
                            {"Select all\tCtrl+A", I_CHECK, 102, f->visible_count > 0},
                            {"Invert selection", I_CHECK, A_INVERT, f->visible_count > 0},
                            {"Duplicate\tCtrl+D", I_COPY, A_DUPLICATE,has&&ordinary},
                            {"Copy path", I_COPY, A_COPY_PATH, true},
                            {"Copy name", I_COPY, A_COPY_NAME, has},
                            {"Copy parent folder path", I_COPY, A_COPY_PARENT,true},
                            {"Copy to folder...", I_COPY, A_COPY_TO, has},
                            {"Move to folder...", I_CUT, A_MOVE_TO, has},
                            {"Deselect all", I_CHECK, A_SELECT_NONE, true},
                            {"Select files only", I_DOC, A_SELECT_FILES, true},
                            {"Select folders only", I_FOLDER, A_SELECT_FOLDERS, true},
                            {"Find in this folder\tCtrl+F", I_SEARCH, 103, true}};
        menu_open(w, x, y, items, sizeof(items) / sizeof(*items), files_command);
    } else if (group == MENU_VIEW) {
        MenuItem items[] = {
            {"Details", f->mode ? I_CHECK : I_LAYOUT, 104, true},
            {"Folders and files", !f->mode ? I_CHECK : I_GRID, 105, true},
            {"", I_COUNT, 0, false},
            {"Sort by name", f->sort == 0 ? I_CHECK : I_DOC, 106, true},
            {"Sort by modified time", f->sort == 1 ? I_CHECK : I_CLOCK, 107, true},
            {"Sort by size", f->sort == 2 ? I_CHECK : I_LAYOUT, 108, true},
            {"Sort by type", f->sort == 3 ? I_CHECK : I_DOC, A_SORT_TYPE, true},
            {f->descending ? "Ascending order" : "Descending order", I_LAYOUT, 109, true},
            {f->show_hidden ? "Hide hidden files" : "Show hidden files", I_LAYOUT, A_HIDDEN, true},
            {"Filter by color or tag...", I_SEARCH, 203, true},
            {"Clear search and tag filter", I_RELOAD, A_CLEAR_FILTER, true},
            {"Refresh\tF5", I_RELOAD, A_REFRESH, true}};
        menu_open(w, x, y, items, sizeof(items) / sizeof(*items), files_command);
    } else if (group == MENU_GO) {
        MenuItem items[] = {
            {"Back\tAlt+Left", I_BACK, 116, f->history_position > 0},
            {"Forward\tAlt+Right", I_FORWARD, 117, f->history_position + 1 < f->history_count},
            {"Parent folder\tAlt+Up", I_UP, A_PARENT, ordinary},
            {"Location\tCtrl+L", I_DRIVE, 115, true},
            {"Home", I_HOME, 110, true},
            {"Downloads", I_DOWNLOAD, 111, true},
            {"Projects", I_FOLDER, 112, true},
            {"Desktop", I_LAYOUT, A_DESKTOP, true},
            {"Saved favorites...", I_HOME, 204, true},
            {"Recent", I_CLOCK, 113, true},
            {"Trash", I_TRASH, 114, true}};
        menu_open(w, x, y, items, sizeof(items) / sizeof(*items), files_command);
    }
}

/* Desktop files share the same bounded model, notifications and file actions. */
static Rect desktop_entry_box(int index) {
    int rows = MAX(1, (desk.height - S(DESKTOP_TOP) - desktop_bottom() - S(20)) / S(94));
    return (Rect){S(12) + index / rows * S(104),
                  S(DESKTOP_TOP + 12) + index % rows * S(94), S(96), S(86)};
}
void files_desktop_init(void) {
    char path[OS_PATH_MAX];
    join_path(path, desk.home, "Desktop");
    desktop_files = (Window){.alive = true, .app = APP_FILES, .id = -1,
                            .box = {0, 0, desk.width, desk.height}};
    if (files_open(&desktop_files, path)) { desktop_files.alive = false; return; }
    ((Files *)desktop_files.state)->desktop = true;
}
void files_desktop_shutdown(void) {
    if (desktop_files.alive) files_close(&desktop_files);
    desktop_files.alive = false;
    if (watch_fd >= 0) close(watch_fd);
    watch_fd = -1;
}
void files_desktop_tick(void) {
    if (!desktop_files.alive) return;
    desktop_files.workspace = desk.workspace;
    desktop_files.box = (Rect){0, 0, desk.width, desk.height};
    files_tick(&desktop_files);
}
void files_desktop_draw(void) {
    if (!desk.desktop_icons || !desktop_files.alive) return;
    Files *f = desktop_files.state;
    for (int i = 0; i < f->visible_count; i++) {
        Rect r = desktop_entry_box(i);
        if (r.x + r.w > desk.width || empty(intersect(r, desk.clip))) continue;
        Entry *e = &f->entries[f->order[i]];
        if (e->selected) { blend(r, desk.accent, 110); border(r, S(5), desk.accent); }
        bool folder = (e->st.mode & 0170000) == S_IFDIR;
        FileMark *mark=mark_for(e->path,false);
        int size=clampi(desk.desktop_icon_size,32,48);
        icon(r.x + (r.w-S(size))/2, r.y + S(4)+(S(48)-S(size))/2, folder ? I_FOLDER : I_DOC, S(size),
             mark && mark->color ? mark_colors[mark->color] : folder ? 0xF6C768 : 0xECF4F6);
        int width = MIN(text_width(e->name, FONT_BODY), r.w - S(4));
        int x = r.x + (r.w - width) / 2;
        label(x + 1, r.y + S(53) + 1, e->name, FONT_BODY, 0x14252A, width);
        label(x, r.y + S(53), e->name, FONT_BODY, 0xFFFFFF, width);
    }
}
bool files_desktop_mouse(int x, int y, unsigned pressed) {
    if (!desktop_files.alive || y < S(DESKTOP_TOP) ||
        y >= desk.height - desktop_bottom()) return false;
    if (!pressed) return false;
    Files *f = desktop_files.state;
    desktop_files.workspace = desk.workspace;
    int hit = -1;
    for (int i = 0; desk.desktop_icons && i < f->visible_count; i++)
        if (contains(desktop_entry_box(i), x, y)) { hit = i; break; }
    if (!(desk.modifiers & (MOD_CTRL | MOD_SHIFT)) &&
        !(hit >= 0 && (pressed & 2) && f->entries[f->order[hit]].selected))
        for (int i = 0; i < f->count; i++) f->entries[i].selected = false;
    if (pressed & 1) {
        Window *w = wm_focused();
        desk.focused = -1;
        if (w) invalidate_window(w);
        invalidate((Rect){0, 0, desk.width, S(DESKTOP_TOP)});
    }
    if (hit >= 0) {
        Entry *e = &f->entries[f->order[hit]];
        e->selected = (pressed & 1) && (desk.modifiers & MOD_CTRL) ? !e->selected : true;
        f->focus = hit;
        if ((pressed & 1) && desk.last_click_window == -1 &&
            desk.last_click_hit == hit && now_ms() - desk.last_click < 420) {
            open_selected(&desktop_files); desk.last_click = 0;
        } else {
            desk.last_click_window = -1; desk.last_click_hit = hit; desk.last_click = now_ms();
        }
    }
    if (pressed & 2) context_menu(&desktop_files, x, y, hit >= 0);
    invalidate_all();
    return true;
}
void files_desktop_key(Key k) {
    if (!desk.desktop_icons || !desktop_files.alive || !k.down) return;
    if (k.code == KEY_ENTER) { action(&desktop_files, A_OPEN); return; }
    if ((k.mods & MOD_CTRL) && (k.ascii | 32) == 'l') {
        wm_open(APP_FILES, ((Files *)desktop_files.state)->directory); return;
    }
    if (k.code == KEY_LEFT || k.code == KEY_RIGHT || k.code == KEY_UP || k.code == KEY_DOWN) {
        Files *f = desktop_files.state;
        int step = k.code == KEY_UP || k.code == KEY_LEFT ? -1 : 1;
        f->focus = clampi(f->focus + step, 0, MAX(0, f->visible_count - 1));
        for (int i = 0; i < f->count; i++) f->entries[i].selected = false;
        if (f->visible_count) f->entries[f->order[f->focus]].selected = true;
        invalidate_all(); return;
    }
    unsigned letter = k.ascii | 32;
    if (k.code == KEY_F2 || k.code == KEY_F5 || k.code == KEY_DELETE || k.code == KEY_ESCAPE ||
        ((k.mods & MOD_CTRL) && (letter == 'a' || letter == 'c' || letter == 'x' || letter == 'v' ||
                                letter == 'n' || letter == 'r' || letter == 'h')))
        files_key(&desktop_files, k);
}

void files_profile_reset(void) {
  memset(&clipboard,0,sizeof(clipboard));
  files_desktop_shutdown();files_desktop_init();
}
