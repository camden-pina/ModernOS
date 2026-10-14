#include "desktop.h"
#include "smooth_scroll.h"

#define DOCUMENT_LIMIT (256 * 1024)

typedef struct {
    char text[DOCUMENT_LIMIT + 1], path[OS_PATH_MAX], status[192];
    size_t length, cursor, anchor, original_length;
    uint64_t original_hash;
    int top, horizontal;
    SmoothScroll animation;
    unsigned mode;
    bool dirty, exists, readonly;
} Editor;

static char text_clipboard[DOCUMENT_LIMIT + 1];
static size_t text_clipboard_length;

static uint64_t hash(uint64_t h, const char *p, size_t n) {
    while (n--) {
        h ^= (uint8_t)*p++;
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t start_line(Editor *e, size_t p) {
    while (p && e->text[p - 1] != '\n') {
        p--;
    }
    return p;
}

static size_t end_line(Editor *e, size_t p) {
    while (p < e->length && e->text[p] != '\n') {
        p++;
    }
    return p;
}

static size_t previous_char(Editor *e, size_t p) {
    if (!p) {
        return 0;
    }
    p--;
    while (p && ((uint8_t)e->text[p] & 0xc0) == 0x80) {
        p--;
    }
    return p;
}

static size_t next_char(Editor *e, size_t p) {
    if (p == e->length) {
        return p;
    }
    p++;
    while (p < e->length && ((uint8_t)e->text[p] & 0xc0) == 0x80) {
        p++;
    }
    return p;
}

/* Positions are UTF-8 byte offsets; display columns expand tabs and ignore
   carriage returns without modifying the document's original bytes. */
static int visual_columns(Editor *e, size_t first, size_t last) {
    int column = 0;
    while (first < last) {
        if (e->text[first] == '\t') {
            column += 4 - column % 4;
        } else if (e->text[first] != '\r') {
            column++;
        }
        first = next_char(e, first);
    }

    return column;
}

static size_t position_at_column(Editor *e, size_t first, int wanted) {
    size_t last = end_line(e, first);
    int column = 0;
    while (first < last) {
        int width = e->text[first] == '\t' ? 4 - column % 4 : e->text[first] == '\r' ? 0 : 1;
        if (width && wanted * 2 < (column * 2 + width)) {
            break;
        }
        column += width;
        first = next_char(e, first);
    }

    return first;
}

static int line_count(Editor *e) {
    int count = 1;
    for (size_t i = 0; i < e->length; i++) {
        count += e->text[i] == '\n';
    }
    return count;
}

static void title(Window *w) {
    Editor *e = w->state;
    copy_text(w->title, sizeof(w->title), e->path[0] ? basename(e->path) : "Untitled document");
    append_text(w->title, sizeof(w->title), e->dirty ? " * - Text Editor" : " - Text Editor");
}

static int read_hash(const char *path, uint64_t *out, size_t *length) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        return fd;
    }
    char buffer[4096];
    *out = 14695981039346656037ULL;
    *length = 0;
    long n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        *out = hash(*out, buffer, n);
        *length += n;
    }
    close(fd);
    return n < 0 ? n : 0;
}

int editor_open(Window *w, const char *path) {
    Editor *e = allocate(sizeof(*e));
    if (!e) {
        return -E_NOMEM;
    }
    memset(e, 0, sizeof(*e));
    e->mode = 0644;
    w->state = e;
    if (path && path[0]) {
        struct os_stat st;
        int error = stat(path, &st);
        if (!error && (st.mode & 0170000) != S_IFREG) {
            error = -E_NOTSUP;
        }
        if (!error && st.size > DOCUMENT_LIMIT) {
            error = -E_FBIG;
        }
        if (error) {
            deallocate(e, sizeof(*e));
            return error;
        }
        int fd = open(path, O_RDONLY, 0);
        if (fd < 0) {
            deallocate(e, sizeof(*e));
            return fd;
        }
        long n = 0;
        while (e->length < (size_t)st.size &&
               (n = read(fd, e->text + e->length, st.size - e->length)) > 0) {
            e->length += n;
        }
        close(fd);
        if (n < 0) {
            deallocate(e, sizeof(*e));
            return n;
        }
        for (size_t i = 0; i < e->length; i++) {
            if (!e->text[i] || ((uint8_t)e->text[i] < 32 && e->text[i] != '\n' &&
                                e->text[i] != '\r' && e->text[i] != '\t')) {
                deallocate(e, sizeof(*e));
                return -E_NOTSUP;
            }
        }

        copy_text(e->path, sizeof(e->path), path);
        e->exists = true;
        e->mode = st.mode & 0777;
        e->original_length = e->length;
        e->original_hash = hash(14695981039346656037ULL, e->text, e->length);
        e->readonly = call2(SYS_access, path, 2) != 0;
    }

    copy_text(e->status, sizeof(e->status),
              e->readonly ? "Read-only filesystem or file. Use Save As for a writable copy."
                          : "Ctrl+S saves  |  Ctrl+Shift+S saves as  |  256 KiB document limit");
    title(w);
    return 0;
}

static void edit_changed(Window *w) {
    Editor *e = w->state;
    e->dirty = true;
    title(w);
    invalidate_window(w);
}

static bool replace_selection(Window *w, const char *bytes, size_t size) {
    Editor *e = w->state;
    if (e->readonly) {
        dialog_notice(w, "Read-only document",
                      "Choose Save As to create a writable copy before editing.");
        return false;
    }
    size_t a = MIN(e->cursor, e->anchor), b = MAX(e->cursor, e->anchor);
    if (e->length - (b - a) + size > DOCUMENT_LIMIT) {
        dialog_notice(
            w, "Document limit",
            "Text Editor supports documents up to 256 KiB. Existing content is preserved.");
        return false;
    }

    memmove(e->text + a + size, e->text + b, e->length - b + 1);
    if (size) {
        memcpy(e->text + a, bytes, size);
    }
    e->length = e->length - (b - a) + size;
    e->cursor = e->anchor = a + size;
    edit_changed(w);
    return true;
}

static int save_to(Window *w, const char *path) {
    Editor *e = w->state;
    char target[OS_PATH_MAX];
    int error = normalize_path(target, desk.home, path);
    if (error) {
        return error;
    }
    struct os_stat st;
    int exists = stat(target, &st);
    bool same = e->path[0] && !strcmp(target, e->path);
    if (exists && exists != -E_NOENT) {
        return exists;
    }
    if (!exists && (!same || !e->exists)) {
        return -E_EXIST;
    }
    if (same && e->exists) {
        uint64_t h;
        size_t length;
        error = read_hash(target, &h, &length);
        if (error) {
            return error;
        }
        if (h != e->original_hash || length != e->original_length) {
            dialog_notice(w, "Document changed on disk",
                          "Another application modified this file. Use Save As to preserve your "
                          "edits under a new name, or reopen the file.");
            return -E_AGAIN;
        }
    }

    char temporary[OS_PATH_MAX], backup[OS_PATH_MAX], name[64], num[32], parent[OS_PATH_MAX];
    normalize_path(parent, target, "..");
    copy_text(name, sizeof(name), ".modernos-save-");
    format_uint(num, call0(SYS_getpid));
    append_text(name, sizeof(name), num);
    append_text(name, sizeof(name), "-");
    format_uint(num, w->id);
    append_text(name, sizeof(name), num);
    error = join_path(temporary, parent, name);
    if (error) {
        return error;
    }
    append_text(name, sizeof(name), ".bak");
    error = join_path(backup, parent, name);
    if (error) {
        return error;
    }
    unsigned save_mode = same ? e->mode : (e->mode | 0600);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, save_mode);
    if (fd < 0) {
        return fd;
    }
    error = write_all(fd, e->text, e->length);
    if (!error) {
        error = fsync(fd);
    }
    close(fd);
    if (error) {
        call1(SYS_unlink, temporary);
        return error;
    }
    bool moved = false;
    if (!exists) {
        if (!stat(backup, &st)) {
            error = -E_EXIST;
        } else {
            error = rename_noreplace(target, backup);
            moved = !error;
        }
    }

    if (!error) {
        error = rename_noreplace(temporary, target);
    }
    if (error) {
        if (moved && rename_noreplace(backup, target)) {
            dialog_notice(w, "Save recovery required",
                          "The replacement failed and rollback also failed. Your original document "
                          "remains at the .modernos-save-*.bak path beside the file. The edited "
                          "copy remains in the temporary file.");
            return -E_IO;
        }

        call1(SYS_unlink, temporary);
        return error;
    }

    int cleanup = moved ? call1(SYS_unlink, backup) : 0;
    copy_text(e->path, sizeof(e->path), target);
    e->exists = true;
    e->mode = save_mode;
    e->readonly = false;
    e->dirty = false;
    e->original_length = e->length;
    e->original_hash = hash(14695981039346656037ULL, e->text, e->length);
    copy_text(e->status, sizeof(e->status),
              cleanup ? "Saved. Original backup could not be removed; inspect the file folder."
                      : "Saved to the filesystem.");
    struct os_stat saved_stat;
    if (!cleanup && !stat(target, &saved_stat)) {
        copy_text(e->status, sizeof(e->status),
                  saved_stat.dev ? "Saved to disk."
                                 : "Saved in RAM storage. Reboot clears this file.");
    }
    title(w);
    recent_add(target);
    files_refresh_all();
    invalidate_window(w);
    trace("SAVE", w, target);
    if (cleanup) {
        dialog_error(w, "Saved; backup cleanup failed", cleanup);
    }
    return 0;
}

static void save_as(Window *w, const char *path) {
    int e = save_to(w, path);
    if (e && !desk.dialog.open) {
        dialog_error(w, "Save document", e);
    }
}

static void save(Window *w, bool as) {
    Editor *e = w->state;
    if (as || !e->path[0] || e->readonly) {
        dialog_input(w, "Save As",
                     "Choose an absolute path or a name relative to Home. Existing destinations "
                     "are preserved.",
                     e->path[0] ? e->path : "", save_as);
    } else {
        int error = save_to(w, e->path);
        if (error && !desk.dialog.open) {
            dialog_error(w, "Save document", error);
        }
    }
}

static int cursor_line(Editor *e) {
    int n = 0;
    for (size_t i = 0; i < e->cursor; i++) {
        if (e->text[i] == '\n') {
            n++;
        }
    }
    return n;
}

static void ensure_cursor(Window *w) {
    Editor *e = w->state;
    Rect b = window_body(w);
    int rows = MAX(1, (b.h - S(99)) / S(20)), line = cursor_line(e);
    int first = e->top / S(20);
    if (line < first) {
        e->top = line * S(20);
    }
    if (line >= first + rows) {
        e->top = (line - rows + 1) * S(20);
    }
    smooth_jump(&e->animation, e->top, now_ms());
    size_t start = start_line(e, e->cursor);
    int column = visual_columns(e, start, e->cursor) * text_width("M", FONT_EDIT);
    int available = MAX(1, b.w - S(90));
    if (column < e->horizontal) {
        e->horizontal = column;
    } else if (column > e->horizontal + available) {
        e->horizontal = column - available;
    }
}

void editor_draw(Window *w) {
    Editor *e = w->state;
    Rect b = window_body(w);
    fill(b, THEME_WINDOW);
    button((Rect){b.x + S(17), b.y + S(12), S(78), S(28)}, "Save", I_SAVE, true, true, false);
    button((Rect){b.x + S(104), b.y + S(12), S(93), S(28)}, "Save As", I_DOC, false, true, false);
    label(b.x + S(216), b.y + S(19), e->path[0] ? e->path : "New document", FONT_NORMAL,
          THEME_SECONDARY_TEXT, b.w - S(232));
    fill((Rect){b.x, b.y + S(51), b.w, 1}, THEME_DIVIDER);
    Rect saved = desk.clip, viewport = {b.x + S(15), b.y + S(64), b.w - S(30), b.h - S(101)};
    clip_set(intersect(saved, viewport));
    int rows = MAX(1, viewport.h / S(20));
    e->top = clampi(e->top, 0, MAX(0, line_count(e) * S(20) - viewport.h));
    int first_line = e->top / S(20), offset = e->top % S(20);
    size_t p = 0;
    int lineno = 0;
    while (p < e->length && lineno < first_line) {
        if (e->text[p++] == '\n') {
            lineno++;
        }
    }
    size_t a = MIN(e->cursor, e->anchor), end = MAX(e->cursor, e->anchor);
    int advance = MAX(1, text_width("M", FONT_EDIT));
    for (int row = 0; row <= rows + 1 && p <= e->length; row++) {
        size_t first = p, last = end_line(e, p);
        int y = viewport.y + row * S(20) - offset, x = viewport.x + S(38) - e->horizontal;
        char number[32];
        format_uint(number, lineno + 1);
        label(viewport.x, y, number, FONT_SMALL, THEME_MUTED_TEXT, S(28));
        /* Clip the text separately from its fixed line-number gutter. Walk
           only as far as the visible end; long lines have no display cap. */
        Rect text_view = {viewport.x + S(38), viewport.y, viewport.w - S(38), viewport.h};
        clip_set(intersect(saved, text_view));
        if (a <= last && end > first) {
            int left = visual_columns(e, first, MAX(a, first)) * advance;
            int right = visual_columns(e, first, MIN(end, last)) * advance;
            if (end > last) {
                right += advance;
            }
            fill((Rect){x + left, y, right - left, S(19)}, desk.selection);
        }

        int column = 0;
        for (size_t pos = first; pos < last;) {
            size_t next = next_char(e, pos);
            int xx = x + column * advance;
            if (xx >= text_view.x + text_view.w) {
                break;
            }
            if (e->text[pos] == '\t') {
                column += 4 - column % 4;
            } else if (e->text[pos] != '\r') {
                if (xx + advance > text_view.x) {
                    char symbol[5];
                    size_t bytes = next - pos;
                    if (bytes > 4) {
                        symbol[0] = '?';
                        bytes = 1;
                    } else {
                        memcpy(symbol, e->text + pos, bytes);
                    }
                    symbol[bytes] = 0;
                    label(xx, y, symbol, FONT_EDIT, THEME_TEXT, advance + S(2));
                }

                column++;
            }

            pos = next;
        }

        if (e->cursor >= first && e->cursor <= last && wm_focused() == w) {
            int cursor_x = x + visual_columns(e, first, e->cursor) * advance;
            fill((Rect){cursor_x, y + S(2), MAX(1, S(1)), S(15)}, desk.accent);
        }
        clip_set(intersect(saved, viewport));
        if (last == e->length) {
            break;
        }
        p = last + 1;
        lineno++;
    }

    desk.clip = saved;
    label(b.x + S(17), b.y + b.h - S(22), e->status, FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(34));
}

bool editor_mouse(Window *w, int x, int y, unsigned pressed, int wheel) {
    Editor *e = w->state;
    Rect b = window_body(w);
    if (wheel) {
        smooth_by(&e->animation, e->top, -(int64_t)wheel * S(60),
                  MAX(0, line_count(e) * S(20) - (b.h - S(101))), now_ms());
        return true;
    }
    if (!(pressed & 1)) {
        return false;
    }
    if (contains((Rect){b.x + S(17), b.y + S(12), S(78), S(28)}, x, y)) {
        save(w, false);
        return true;
    }
    if (contains((Rect){b.x + S(104), b.y + S(12), S(93), S(28)}, x, y)) {
        save(w, true);
        return true;
    }
    if (contains((Rect){b.x + S(15), b.y + S(64), b.w - S(30), b.h - S(100)}, x, y)) {
        int row = (e->top + y - b.y - S(64)) / S(20);
        size_t p = 0;
        while (row && p < e->length) {
            if (e->text[p++] == '\n') {
                row--;
            }
        }
        int column = MAX(0, x - b.x - S(53) + e->horizontal);
        int advance = MAX(1, text_width("M", FONT_EDIT));
        e->cursor = position_at_column(e, p, (column + advance / 2) / advance);
        if (!(desk.modifiers & MOD_SHIFT)) {
            e->anchor = e->cursor;
        }
        invalidate_window(w);
        return true;
    }

    return false;
}

void editor_tick(Window *w) {
    Editor *e = w->state;
    Rect b = window_body(w);
    Rect viewport = {b.x + S(15), b.y + S(64), b.w - S(30), b.h - S(101)};
    int top =
        smooth_tick(&e->animation, e->top, MAX(0, line_count(e) * S(20) - viewport.h), now_ms());
    if (top != e->top) {
        e->top = top;
        trace_scroll(w, "Editor", top, e->animation.target);
        invalidate(viewport);
    }
}

void editor_key(Window *w, Key k) {
    if (!k.down) {
        return;
    }
    Editor *e = w->state;
    if (k.mods & MOD_CTRL) {
        unsigned key = k.ascii | 32;
        if (key == 's') {
            save(w, k.mods & MOD_SHIFT);
            return;
        }
        if (key == 'a') {
            e->anchor = 0;
            e->cursor = e->length;
            ensure_cursor(w);
            invalidate_window(w);
            return;
        }

        if (key == 'c' || key == 'x') {
            size_t a = MIN(e->cursor, e->anchor), b = MAX(e->cursor, e->anchor);
            text_clipboard_length = b - a;
            memcpy(text_clipboard, e->text + a, b - a);
            if (key == 'x' && b > a) {
                replace_selection(w, "", 0);
            }
            invalidate_window(w);
            return;
        }

        if (key == 'v') {
            replace_selection(w, text_clipboard, text_clipboard_length);
            ensure_cursor(w);
            return;
        }
    }

    bool movement = true;
    if (k.code == KEY_LEFT) {
        e->cursor = previous_char(e, e->cursor);
    } else if (k.code == KEY_RIGHT) {
        e->cursor = next_char(e, e->cursor);
    } else if (k.code == KEY_HOME) {
        e->cursor = (k.mods & MOD_CTRL) ? 0 : start_line(e, e->cursor);
    } else if (k.code == KEY_END) {
        e->cursor = (k.mods & MOD_CTRL) ? e->length : end_line(e, e->cursor);
    } else if (k.code == KEY_UP || k.code == KEY_DOWN || k.code == KEY_PAGEUP ||
               k.code == KEY_PAGEDOWN) {
        int times = (k.code == KEY_PAGEUP || k.code == KEY_PAGEDOWN)
                        ? MAX(1, (w->box.h - S(140)) / S(20))
                        : 1;
        bool down = k.code == KEY_DOWN || k.code == KEY_PAGEDOWN;
        while (times--) {
            size_t first = start_line(e, e->cursor), target;
            int column = visual_columns(e, first, e->cursor);
            if (down) {
                target = end_line(e, e->cursor);
                if (target == e->length) {
                    break;
                }
                target++;
            } else {
                if (!first) {
                    break;
                }
                target = start_line(e, first - 1);
            }

            e->cursor = position_at_column(e, target, column);
        }
    } else {
        movement = false;
    }
    if (movement) {
        if (!(k.mods & MOD_SHIFT)) {
            e->anchor = e->cursor;
        }
        ensure_cursor(w);
        invalidate_window(w);
        return;
    }

    if (k.code == KEY_BACKSPACE || k.code == KEY_DELETE) {
        if (e->cursor == e->anchor) {
            if (k.code == KEY_BACKSPACE) {
                e->anchor = previous_char(e, e->cursor);
            } else {
                e->anchor = next_char(e, e->cursor);
            }
        }

        if (e->cursor != e->anchor) {
            replace_selection(w, "", 0);
        }
    } else if (k.code == KEY_ENTER || k.code == KEY_TAB ||
               (k.ascii >= 32 && k.ascii < 127 && !(k.mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)))) {
        char c = k.code == KEY_ENTER ? '\n' : k.code == KEY_TAB ? '\t' : k.ascii;
        replace_selection(w, &c, 1);
    }
    ensure_cursor(w);
    invalidate_window(w);
}

static void discard(Window *w, const char *unused) {
    (void)unused;
    if (!w || !w->alive) {
        return;
    }
    Editor *e = w->state;
    e->dirty = false;
    wm_close(w);
}

bool editor_confirm_close(Window *w) {
    Editor *e = w->state;
    if (e->dirty) {
        if (wm_focused() != w) {
            bool quitting = desk.quit_pending;
            wm_focus(w);
            desk.quit_pending = quitting;
        }
        dialog_confirm(w, "Discard unsaved changes?",
                       "Your changes have not been saved. Confirm to discard them, or cancel and "
                       "use Ctrl+S to save the document.",
                       discard);
        return false;
    }

    return true;
}

bool editor_close(Window *w) {
    Editor *e = w->state;
    deallocate(e, sizeof(*e));
    w->state = NULL;
    return true;
}
