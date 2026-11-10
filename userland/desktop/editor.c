#include "desktop.h"
#include "smooth_scroll.h"

#define DOCUMENT_LIMIT (256 * 1024)

typedef struct {
  size_t at, removed, inserted, cursor, anchor;
  char *bytes;
} EditRecord;
#define EDIT_HISTORY 64
#define EDIT_HISTORY_BYTES (1024 * 1024)
typedef struct {
  char text[DOCUMENT_LIMIT + 1], path[OS_PATH_MAX], status[192];
  size_t length, cursor, anchor, original_length;
  uint64_t original_hash;
  int top, horizontal;
  SmoothScroll animation;
  unsigned mode;
  bool dirty, exists, readonly, preferences, crlf;
  int pref_top, pref_focus;
  EditRecord history[EDIT_HISTORY];
  unsigned history_count, history_at;
  size_t history_bytes;
  char find[128];
} Editor;

/* Preferences live in the existing editor window; the document remains intact.
 */
static void editor_command(Window *, int);
static int toolbar_layout(Window *, int, Rect *);
static int toolbar_bottom(Window *);
static bool toolbar_enabled(Window *, int);
static const char *toolbar_label(Window *, int);
static int editor_font(void) {
  return desk.editor_text_size == 11 ? FONT_MONO : FONT_EDIT;
}
static int editor_row(void) { return S(desk.editor_text_size == 11 ? 18 : 20); }
static int editor_gutter(void) {
  return desk.editor_line_numbers ? S(38) : S(4);
}
static const char *preference_names[] = {"Text size",
                                         "Tab width",
                                         "Insert spaces",
                                         "Auto indent",
                                         "Line numbers",
                                         "Show whitespace",
                                         "Highlight current line",
                                         "New line endings"};
static const char *preference_values[][3] = {{"Compact 11", "Standard 13"},
                                             {"2", "4", "8"},
                                             {"Off", "On"},
                                             {"Off", "On"},
                                             {"Off", "On"},
                                             {"Off", "On"},
                                             {"Off", "On"},
                                             {"Detect", "LF", "CRLF"}};
static int preference_count(int row) { return row == 1 || row == 7 ? 3 : 2; }
static int preference_value(int row) {
  switch (row) {
  case 0:
    return desk.editor_text_size == 13;
  case 1:
    return desk.editor_tab_width == 2 ? 0 : desk.editor_tab_width == 4 ? 1 : 2;
  case 2:
    return desk.editor_spaces;
  case 3:
    return desk.editor_autoindent;
  case 4:
    return desk.editor_line_numbers;
  case 5:
    return desk.editor_whitespace;
  case 6:
    return desk.editor_highlight;
  default:
    return desk.editor_newline;
  }
}
static void preference_choose(Window *w, int row, int value) {
  Editor *e = w->state;
  switch (row) {
  case 0:
    desk.editor_text_size = value ? 13 : 11;
    break;
  case 1:
    desk.editor_tab_width = value == 0 ? 2 : value == 1 ? 4 : 8;
    break;
  case 2:
    desk.editor_spaces = value;
    break;
  case 3:
    desk.editor_autoindent = value;
    break;
  case 4:
    desk.editor_line_numbers = value;
    break;
  case 5:
    desk.editor_whitespace = value;
    break;
  case 6:
    desk.editor_highlight = value;
    break;
  case 7:
    desk.editor_newline = value;
    break;
  }
  e->pref_focus = row;
  preferences_save(w);
  for (int i = 0; i < MAX_WINDOWS; i++)
    if (desk.windows[i].alive && desk.windows[i].app == APP_EDITOR)
      invalidate_window(&desk.windows[i]);
  if (desk.trace)
    printf("DESKTOP EDITOR_PREF row=%d value=%d tab=%d text=%d spaces=%d "
           "indent=%d numbers=%d whitespace=%d highlight=%d newline=%d\n",
           row, value, desk.editor_tab_width, desk.editor_text_size,
           desk.editor_spaces, desk.editor_autoindent, desk.editor_line_numbers,
           desk.editor_whitespace, desk.editor_highlight, desk.editor_newline);
}
void editor_preferences_open(Window *w) {
  if (!w || w->app != APP_EDITOR)
    return;
  Editor *e = w->state;
  e->preferences = true;
  e->pref_focus = e->pref_top = 0;
  invalidate_window(w);
  trace("EDITOR_PREFERENCES", w, "open");
}
static Rect preference_button(Rect b, int row, int value, int top) {
  int width = MAX(S(46), MIN(S(100), (b.w - S(225)) / 3));
  return (Rect){b.x + S(204) + value * (width + S(6)),
                b.y + S(65) + row * S(38) - top, width, S(27)};
}
static void preference_draw(Window *w) {
  Editor *e = w->state;
  Rect b = window_body(w), saved = desk.clip;
  fill(b, THEME_WINDOW);
  label(b.x + S(20), b.y + S(18), "Text Editor preferences", FONT_TITLE,
        THEME_TEXT, b.w - S(130));
  button((Rect){b.x + b.w - S(94), b.y + S(13), S(74), S(28)}, "Done", I_CHECK,
         true, true, false);
  Rect view = {b.x + S(16), b.y + S(58), b.w - S(32), b.h - S(112)};
  e->pref_top = clampi(e->pref_top, 0, MAX(0, S(310) - view.h));
  clip_set(intersect(saved, view));
  for (int row = 0; row < 8; row++) {
    Rect first = preference_button(b, row, 0, e->pref_top);
    label(b.x + S(20), first.y + S(7), preference_names[row], FONT_NORMAL,
          THEME_TEXT, S(175));
    for (int v = 0; v < preference_count(row); v++) {
      Rect r = preference_button(b, row, v, e->pref_top);
      button(r, preference_values[row][v], I_COUNT, preference_value(row) == v,
             true, e->pref_focus == row && preference_value(row) == v);
    }
  }
  desk.clip = saved;
  label(b.x + S(20), b.y + b.h - S(43),
        "Detect preserves a file's CRLF/LF style. No existing bytes are "
        "converted.",
        FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(40));
  label(b.x + S(20), b.y + b.h - S(25),
        desk.config_on_disk ? "Preferences save automatically to disk. Tab "
                              "selects a row; arrows change it."
                            : "Preferences save in RAM. Reboot clears them "
                              "unless Home is on a writable disk.",
        FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(40));
}
static bool preference_mouse(Window *w, int x, int y, unsigned pressed,
                             int wheel) {
  Editor *e = w->state;
  Rect b = window_body(w),
       view = {b.x + S(16), b.y + S(58), b.w - S(32), b.h - S(112)};
  if (wheel) {
    e->pref_top =
        clampi(e->pref_top - wheel * S(38), 0, MAX(0, S(310) - view.h));
    invalidate_window(w);
  }
  if (!(pressed & 1))
    return true;
  if (contains((Rect){b.x + b.w - S(94), b.y + S(13), S(74), S(28)}, x, y)) {
    e->preferences = false;
    invalidate_window(w);
    return true;
  }
  if (contains(view, x, y))
    for (int row = 0; row < 8; row++)
      for (int v = 0; v < preference_count(row); v++)
        if (contains(preference_button(b, row, v, e->pref_top), x, y)) {
          preference_choose(w, row, v);
          return true;
        }
  return true;
}
static void preference_key(Window *w, Key k) {
  Editor *e = w->state;
  if (k.code == KEY_ESCAPE)
    e->preferences = false;
  else if (k.code == KEY_TAB || k.code == KEY_UP || k.code == KEY_DOWN) {
    int delta = k.code == KEY_UP || (k.code == KEY_TAB && (k.mods & MOD_SHIFT))
                    ? -1
                    : 1;
    e->pref_focus = (e->pref_focus + delta + 8) % 8;
    int height = window_body(w).h - S(112), row = e->pref_focus * S(38);
    e->pref_top = clampi(e->pref_top, MAX(0, row + S(34) - height), row);
  } else if (k.code == KEY_LEFT || k.code == KEY_RIGHT || k.code == KEY_ENTER ||
             k.ascii == ' ') {
    int row = e->pref_focus, count = preference_count(row);
    int delta = k.code == KEY_LEFT ? -1 : 1;
    preference_choose(w, row, (preference_value(row) + delta + count) % count);
  } else if ((k.mods & MOD_CTRL) && k.ascii == 's')
    preferences_save(w);
  invalidate_window(w);
}

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
  if (p >= 2 && e->text[p - 2] == '\r' && e->text[p - 1] == '\n')
    return p - 2;
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
  if (p + 1 < e->length && e->text[p] == '\r' && e->text[p + 1] == '\n')
    return p + 2;
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
      column += desk.editor_tab_width - column % desk.editor_tab_width;
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
    if (e->text[first] == '\r' && first + 1 < e->length &&
        e->text[first + 1] == '\n')
      break;
    int width = e->text[first] == '\t'
                    ? desk.editor_tab_width - column % desk.editor_tab_width
                : e->text[first] == '\r' ? 0
                                         : 1;
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
  copy_text(w->title, sizeof(w->title),
            e->path[0] ? basename(e->path) : "Untitled document");
  append_text(w->title, sizeof(w->title),
              e->dirty ? " * - Text Editor" : " - Text Editor");
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
    for (size_t i = 0; i < e->length; i++) {
      if (e->text[i] == '\n') {
        e->crlf = i && e->text[i - 1] == '\r';
        break;
      }
    }
    e->mode = st.mode & 0777;
    e->original_length = e->length;
    e->original_hash = hash(14695981039346656037ULL, e->text, e->length);
    e->readonly = call2(SYS_access, path, 2) != 0;
  }

  copy_text(
      e->status, sizeof(e->status),
      e->readonly
          ? "Read-only filesystem or file. Use Save As for a writable copy."
          : "Ctrl+S saves  |  Ctrl+Shift+S saves as  |  256 KiB document "
            "limit");
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
    dialog_notice(w, "Document limit",
                  "Text Editor supports documents up to 256 KiB. Existing "
                  "content is preserved.");
    return false;
  }

  if (a == b && !size)
    return true;
  size_t amount = b - a + size;
  char *record = allocate(MAX((size_t)1, amount));
  if (!record) {
    dialog_error(w, "Record undo", -E_NOMEM);
    return false;
  }
  memcpy(record, e->text + a, b - a);
  if (size)
    memcpy(record + b - a, bytes, size);
  while (e->history_count > e->history_at) {
    EditRecord *r = &e->history[--e->history_count];
    size_t n = r->removed + r->inserted;
    e->history_bytes -= n;
    deallocate(r->bytes, MAX((size_t)1, n));
  }
  while (e->history_count && (e->history_count == EDIT_HISTORY ||
                              e->history_bytes + amount > EDIT_HISTORY_BYTES)) {
    EditRecord *r = &e->history[0];
    size_t n = r->removed + r->inserted;
    e->history_bytes -= n;
    deallocate(r->bytes, MAX((size_t)1, n));
    memmove(e->history, e->history + 1,
            (--e->history_count) * sizeof(EditRecord));
    e->history_at--;
  }
  e->history[e->history_count++] =
      (EditRecord){a, b - a, size, e->cursor, e->anchor, record};
  e->history_at = e->history_count;
  e->history_bytes += amount;
  memmove(e->text + a + size, e->text + b, e->length - b + 1);
  if (size)
    memcpy(e->text + a, record + (b - a), size);
  e->length = e->length - (b - a) + size;
  e->cursor = e->anchor = a + size;
  edit_changed(w);
  return true;
}

static void undo_edit(Window *w, bool redo) {
  Editor *e = w->state;
  if (e->readonly ||
      (redo ? e->history_at == e->history_count : !e->history_at))
    return;
  EditRecord *r = &e->history[redo ? e->history_at : e->history_at - 1];
  size_t remove = redo ? r->removed : r->inserted,
         insert = redo ? r->inserted : r->removed;
  memmove(e->text + r->at + insert, e->text + r->at + remove,
          e->length - r->at - remove + 1);
  if (insert)
    memcpy(e->text + r->at, r->bytes + (redo ? r->removed : 0), insert);
  e->length = e->length - remove + insert;
  e->cursor = redo ? r->at + r->inserted : r->cursor;
  e->anchor = redo ? e->cursor : r->anchor;
  if (redo)
    e->history_at++;
  else
    e->history_at--;
  e->dirty =
      !e->exists || e->length != e->original_length ||
      hash(14695981039346656037ULL, e->text, e->length) != e->original_hash;
  title(w);
  invalidate_window(w);
  trace(redo ? "EDITOR_REDO" : "EDITOR_UNDO", w, "byte-exact edit");
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
                    "Another application modified this file. Use Save As to "
                    "preserve your "
                    "edits under a new name, or reopen the file.");
      return -E_AGAIN;
    }
  }

  char temporary[OS_PATH_MAX], backup[OS_PATH_MAX], name[64], num[32],
      parent[OS_PATH_MAX];
  normalize_path(parent, target, "..");
  copy_text(name, sizeof(name), ".coast-save-");
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
  int fd = desktop_create_file(temporary, save_mode);
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
                    "The replacement failed and rollback also failed. Your "
                    "original document "
                    "remains at the .coast-save-*.bak path beside the file. "
                    "The edited "
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
            cleanup ? "Saved. Original backup could not be removed; inspect "
                      "the file folder."
                    : "Saved to the filesystem.");
  struct os_stat saved_stat;
  if (!cleanup && !stat(target, &saved_stat)) {
    copy_text(e->status, sizeof(e->status),
              saved_stat.dev
                  ? "Saved to disk."
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
                 "Choose an absolute path or a name relative to Home. Existing "
                 "destinations "
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
  int rows = MAX(1, (b.h - toolbar_bottom(w) - S(35)) / editor_row()),
      line = cursor_line(e);
  int first = e->top / editor_row();
  if (line < first) {
    e->top = line * editor_row();
  }
  if (line >= first + rows) {
    e->top = (line - rows + 1) * editor_row();
  }
  smooth_jump(&e->animation, e->top, now_ms());
  size_t start = start_line(e, e->cursor);
  int column =
      visual_columns(e, start, e->cursor) * text_width("M", editor_font());
  int available = MAX(1, b.w - S(52) - editor_gutter());
  if (column < e->horizontal) {
    e->horizontal = column;
  } else if (column > e->horizontal + available) {
    e->horizontal = column - available;
  }
}

void editor_draw(Window *w) {
  if (((Editor *)w->state)->preferences) {
    preference_draw(w);
    return;
  }
  Editor *e = w->state;
  Rect b = window_body(w);
  fill(b, THEME_WINDOW);
  for (int id = 0; id < 15; id++) {
    Rect r;
    toolbar_layout(w, id, &r);
    button(r, toolbar_label(w, id), I_COUNT, id == 2, toolbar_enabled(w, id),
           false);
  }
  int bottom = toolbar_bottom(w);
  label(b.x + S(15), b.y + bottom - S(17),
        e->path[0] ? e->path : "Untitled — plain text", FONT_SMALL,
        THEME_SECONDARY_TEXT, b.w - S(30));
  fill((Rect){b.x, b.y + bottom, b.w, 1}, THEME_DIVIDER);
  Rect saved = desk.clip,
       viewport = {b.x + S(15), b.y + toolbar_bottom(w) + S(12), b.w - S(30),
                   b.h - toolbar_bottom(w) - S(49)};
  clip_set(intersect(saved, viewport));
  int rows = MAX(1, viewport.h / editor_row());
  e->top = clampi(e->top, 0, MAX(0, line_count(e) * editor_row() - viewport.h));
  int first_line = e->top / editor_row(), offset = e->top % editor_row();
  size_t p = 0;
  int lineno = 0;
  while (p < e->length && lineno < first_line) {
    if (e->text[p++] == '\n') {
      lineno++;
    }
  }
  size_t a = MIN(e->cursor, e->anchor), end = MAX(e->cursor, e->anchor);
  int advance = MAX(1, text_width("M", editor_font()));
  for (int row = 0; row <= rows + 1 && p <= e->length; row++) {
    size_t first = p, last = end_line(e, p);
    int y = viewport.y + row * editor_row() - offset,
        x = viewport.x + editor_gutter() - e->horizontal;
    char number[32];
    format_uint(number, lineno + 1);
    if (desk.editor_line_numbers)
      label(viewport.x, y, number, FONT_SMALL, THEME_MUTED_TEXT, S(28));
    /* Clip the text separately from its fixed line-number gutter. Walk
       only as far as the visible end; long lines have no display cap. */
    Rect text_view = {viewport.x + editor_gutter(), viewport.y,
                      viewport.w - editor_gutter(), viewport.h};
    clip_set(intersect(saved, text_view));
    if (desk.editor_highlight && e->cursor >= first && e->cursor <= last)
      blend((Rect){text_view.x, y, text_view.w, editor_row() - 1}, desk.accent,
            desk.dark_mode ? 24 : 12);
    if (a <= last && end > first) {
      int left = visual_columns(e, first, MAX(a, first)) * advance;
      int right = visual_columns(e, first, MIN(end, last)) * advance;
      if (end > last) {
        right += advance;
      }
      fill((Rect){x + left, y, right - left, editor_row() - 1}, desk.selection);
    }

    int column = 0;
    for (size_t pos = first; pos < last;) {
      size_t next = next_char(e, pos);
      int xx = x + column * advance;
      if (xx >= text_view.x + text_view.w) {
        break;
      }
      if (e->text[pos] == '\t') {
        if (desk.editor_whitespace)
          label(xx, y, "→", editor_font(), THEME_MUTED_TEXT,
                advance *
                    (desk.editor_tab_width - column % desk.editor_tab_width));
        column += desk.editor_tab_width - column % desk.editor_tab_width;
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
          if (desk.editor_whitespace && symbol[0] == ' ')
            copy_text(symbol, sizeof(symbol), "·");
          label(xx, y, symbol, editor_font(),
                e->text[pos] == ' ' ? THEME_MUTED_TEXT : THEME_TEXT,
                advance + S(2));
        }

        column++;
      }

      pos = next;
    }

    if (e->cursor >= first && e->cursor <= last && wm_focused() == w) {
      int cursor_x = x + visual_columns(e, first, e->cursor) * advance;
      fill((Rect){cursor_x, y + S(2), MAX(1, S(1)), editor_row() - S(5)},
           desk.accent);
    }
    clip_set(intersect(saved, viewport));
    if (last == e->length) {
      break;
    }
    p = last + 1;
    lineno++;
  }

  desk.clip = saved;
  label(b.x + S(17), b.y + b.h - S(22), e->status, FONT_SMALL,
        THEME_SECONDARY_TEXT, b.w - S(34));
}

bool editor_mouse(Window *w, int x, int y, unsigned pressed, int wheel) {
  if (((Editor *)w->state)->preferences)
    return preference_mouse(w, x, y, pressed, wheel);
  Editor *e = w->state;
  Rect b = window_body(w);
  if (wheel) {
    smooth_by(&e->animation, e->top, -(int64_t)wheel * S(60),
              MAX(0, line_count(e) * editor_row() -
                         (b.h - toolbar_bottom(w) - S(49))),
              now_ms());
    return true;
  }
  if (!(pressed & 1)) {
    return false;
  }
  for (int id = 0; id < 15; id++) {
    Rect r;
    toolbar_layout(w, id, &r);
    if (contains(r, x, y)) {
      if (toolbar_enabled(w, id))
        editor_command(w, id < 4    ? id
                          : id == 4 ? 30
                          : id == 5 ? 31
                          : id == 6 ? 32
                          : id == 7 ? 33
                          : id == 8 ? 22
                                    : 40 + id - 9);
      return true;
    }
  }
  if (contains((Rect){b.x + S(15), b.y + toolbar_bottom(w) + S(12), b.w - S(30),
                      b.h - toolbar_bottom(w) - S(48)},
               x, y)) {
    int row = (e->top + y - b.y - toolbar_bottom(w) - S(12)) / editor_row();
    size_t p = 0;
    while (row && p < e->length) {
      if (e->text[p++] == '\n') {
        row--;
      }
    }
    int column = MAX(0, x - b.x - S(15) - editor_gutter() + e->horizontal);
    int advance = MAX(1, text_width("M", editor_font()));
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
  if (((Editor *)w->state)->preferences)
    return;
  Editor *e = w->state;
  Rect b = window_body(w);
  Rect viewport = {b.x + S(15), b.y + toolbar_bottom(w) + S(12), b.w - S(30),
                   b.h - toolbar_bottom(w) - S(49)};
  int top =
      smooth_tick(&e->animation, e->top,
                  MAX(0, line_count(e) * editor_row() - viewport.h), now_ms());
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
  if ((k.mods & MOD_CTRL) && (k.ascii == ',' || k.code == 0x36)) {
    if (!e->preferences)
      editor_preferences_open(w);
    else {
      e->preferences = false;
      invalidate_window(w);
    }
    return;
  }
  if (e->preferences) {
    preference_key(w, k);
    return;
  }
  if ((k.mods & MOD_SHIFT) && k.code == KEY_INSERT) {
    k.mods = MOD_CTRL;
    k.ascii = 'v';
    k.code = 0x19;
  }
  if (k.mods & MOD_CTRL) {
    unsigned key =
        k.code >= 4 && k.code <= 0x1d ? 'a' + k.code - 4 : k.ascii | 32;
    if (key == 'n' || key == 'o') {
      editor_command(w, key == 'n' ? 0 : 1);
      return;
    }
    if (key == 'z' || key == 'y') {
      undo_edit(w, key == 'y' || (k.mods & MOD_SHIFT));
      ensure_cursor(w);
      return;
    }
    if (key == 'f' || key == 'h') {
      editor_command(w, key == 'f' ? 32 : 33);
      return;
    }
    if (key == 'g') {
      editor_command(w, 34);
      return;
    }
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
      if (b == a)
        return;
      int error = clipboard_write(e->text + a, b - a);
      if (error) {
        clipboard_error(w, "Copy text", error);
        return;
      }
      trace("CLIPBOARD_COPY", w, "Editor selection");
      if (key == 'x') {
        replace_selection(w, "", 0);
      }
      invalidate_window(w);
      return;
    }

    if (key == 'v') {
      char *text;
      size_t length;
      int error = clipboard_read(&text, &length);
      if (error) {
        clipboard_error(w, "Paste text", error);
        return;
      }
      if (length)
        replace_selection(w, text, length);
      clipboard_release(text);
      trace("CLIPBOARD_PASTE", w, "Editor");
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
    if (!(k.mods & MOD_CTRL) && e->cursor && e->text[e->cursor - 1] == '\r')
      e->cursor--;
  } else if (k.code == KEY_UP || k.code == KEY_DOWN || k.code == KEY_PAGEUP ||
             k.code == KEY_PAGEDOWN) {
    int times = (k.code == KEY_PAGEUP || k.code == KEY_PAGEDOWN)
                    ? MAX(1, (window_body(w).h - toolbar_bottom(w) - S(49)) /
                                 editor_row())
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
             (k.ascii >= 32 && k.ascii < 127 &&
              !(k.mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)))) {
    if (k.code == KEY_ENTER) {
      size_t at = MIN(e->cursor, e->anchor), first = start_line(e, at),
             indent = 0;
      if (desk.editor_autoindent)
        while (first + indent < at && (e->text[first + indent] == ' ' ||
                                       e->text[first + indent] == '\t'))
          indent++;
      bool crlf = desk.editor_newline == 2 || (!desk.editor_newline && e->crlf);
      size_t newline = crlf ? 2 : 1, bytes = newline + indent;
      char *insert = allocate(bytes);
      if (!insert)
        dialog_error(w, "Insert newline", -E_NOMEM);
      else {
        if (crlf)
          insert[0] = '\r';
        insert[newline - 1] = '\n';
        memcpy(insert + newline, e->text + first, indent);
        replace_selection(w, insert, bytes);
        deallocate(insert, bytes);
      }
    } else if (k.code == KEY_TAB && desk.editor_spaces) {
      size_t at = MIN(e->cursor, e->anchor);
      int column = visual_columns(e, start_line(e, at), at);
      char spaces[8];
      memset(spaces, ' ', sizeof(spaces));
      replace_selection(w, spaces,
                        desk.editor_tab_width - column % desk.editor_tab_width);
    } else {
      char c = k.code == KEY_TAB ? '\t' : k.ascii;
      replace_selection(w, &c, 1);
    }
  }
  ensure_cursor(w);
  invalidate_window(w);
}

static void discard(Window *w, const char *unused) {
  (void)unused;
  if (!w || !w->alive) {
    return;
  }
  if (power_review_accept(w)) {
    return;
  }
  Editor *e = w->state;
  e->dirty = false;
  wm_close(w);
}

bool editor_confirm_close(Window *w) {
  if (power_review_approved(w)) {
    return true;
  }
  Editor *e = w->state;
  if (e->dirty) {
    if (wm_focused() != w) {
      bool quitting = desk.quit_pending;
      wm_focus(w);
      desk.quit_pending = quitting;
    }
    bool power = power_review_pending(w);
    dialog_confirm(w,
                   power ? "Continue with unsaved changes?"
                         : "Discard unsaved changes?",
                   power ? "This document has unsaved changes. Confirm to "
                           "allow the system-power "
                           "request, or cancel and use Ctrl+S to save. The "
                           "document stays open "
                           "during the reversible request; cancellation "
                           "preserves its changes."
                         : "Your changes have not been saved. Confirm to "
                           "discard them, or cancel and "
                           "use Ctrl+S to save the document.",
                   discard);
    return false;
  }

  return true;
}

bool editor_close(Window *w) {
  Editor *e = w->state;
  for (unsigned i = 0; i < e->history_count; i++) {
    EditRecord *r = &e->history[i];
    deallocate(r->bytes, MAX((size_t)1, r->removed + r->inserted));
  }
  deallocate(e, sizeof(*e));
  w->state = NULL;
  return true;
}

static void open_document(Window *w, const char *path) {
  char full[OS_PATH_MAX];
  normalize_path(full, desk.home, path);
  struct os_stat st;
  int error = stat(full, &st);
  if (error) {
    dialog_error(w, "Open document", error);
    return;
  }
  if ((st.mode & 0170000) != S_IFREG) {
    dialog_notice(w, "Open document", "Choose a regular text file.");
    return;
  }
  wm_open(APP_EDITOR, full);
}

static void find_next(Window *w) {
  Editor *e = w->state;
  size_t n = strlen(e->find);
  if (!n)
    return;
  size_t start = MIN(e->cursor, e->length);
  for (int pass = 0; pass < 2; pass++) {
    size_t first = pass ? 0 : start, last = pass ? start : e->length;
    for (size_t i = first; i + n <= e->length && i < last;
         i = next_char(e, i)) {
      if (!memcmp(e->text + i, e->find, n) &&
          (i + n == e->length || ((uint8_t)e->text[i + n] & 0xc0) != 0x80)) {
        e->anchor = i;
        e->cursor = i + n;
        ensure_cursor(w);
        invalidate_window(w);
        trace("EDITOR_FIND", w, "literal case-sensitive match");
        return;
      }
    }
  }
  copy_text(e->status, sizeof(e->status),
            "No match. Find uses literal, case-sensitive text.");
  invalidate_window(w);
}
static void found_query(Window *w, const char *text) {
  Editor *e = w->state;
  copy_text(e->find, sizeof(e->find), text);
  find_next(w);
}
static void replace_query(Window *w, const char *text) {
  Editor *e = w->state;
  size_t a = MIN(e->cursor, e->anchor), b = MAX(e->cursor, e->anchor),
         n = strlen(e->find);
  if (n && b - a == n && !memcmp(e->text + a, e->find, n)) {
    if (replace_selection(w, text, strlen(text)))
      find_next(w);
  } else {
    find_next(w);
    copy_text(e->status, sizeof(e->status),
              "Match selected. Choose Replace again to replace it.");
  }
  ensure_cursor(w);
  invalidate_window(w);
}
static void replacement_find(Window *w, const char *text) {
  Editor *e = w->state;
  copy_text(e->find, sizeof(e->find), text);
  find_next(w);
  if (e->find[0])
    dialog_input(w, "Replace selected match",
                 "Replace one literal match. Existing line endings and other "
                 "bytes are preserved.",
                 "", replace_query);
}
static void editor_command(Window *w, int command) {
  if (command == 0)
    wm_open(APP_EDITOR, NULL);
  else if (command == 1)
    dialog_input(w, "Open document", "Open a text document in a new window.",
                 desk.home, open_document);
  else if (command == 2 || command == 3)
    save(w, command == 3);
  else if (command == 4)
    wm_close(w);
  else if (command >= 10 && command <= 13) {
    const char letters[] = "acxv";
    unsigned letter = letters[command - 10];
    menu_send_key(w, letter, 4 + letter - 'a', MOD_CTRL);
  } else if (command == 22)
    editor_preferences_open(w);
  else if (command == 30 || command == 31) {
    undo_edit(w, command == 31);
    ensure_cursor(w);
  } else if (command == 32)
    dialog_input(w, "Find text",
                 "Literal, case-sensitive search. Ctrl+G finds the next match.",
                 ((Editor *)w->state)->find, found_query);
  else if (command == 33 && !((Editor *)w->state)->readonly)
    dialog_input(w, "Find for replacement",
                 "Choose literal text to replace, one match at a time.",
                 ((Editor *)w->state)->find, replacement_find);
  else if (command == 34)
    find_next(w);
  else if (command >= 40 && command <= 45) {
    int rows[] = {0, 1, 2, 3, 4, 5}, row = rows[command - 40];
    preference_choose(w, row,
                      (preference_value(row) + 1) % preference_count(row));
    ensure_cursor(w);
  } else if (command == 20 || command == 21)
    menu_send_key(w, 0, command == 20 ? KEY_HOME : KEY_END, MOD_CTRL);
}
void editor_menu(Window *w, int group, int x, int y) {
  Editor *e = w->state;
  bool selected = e->cursor != e->anchor;
  if (group == MENU_FILE) {
    MenuItem items[] = {{"New document", I_DOC, 0, true},
                        {"Open document...", I_FOLDER, 1, true},
                        {"Save\tCtrl+S", I_SAVE, 2, !e->readonly},
                        {"Save As...\tCtrl+Shift+S", I_SAVE, 3, true},
                        {"Close document\tAlt+F4", I_CLOSE, 4, true}};
    menu_open(w, x, y, items, sizeof(items) / sizeof(*items), editor_command);
  } else if (group == MENU_EDIT) {
    MenuItem items[] = {
        {"Undo\tCtrl+Z", I_UP, 30, !e->readonly && e->history_at > 0},
        {"Redo\tCtrl+Shift+Z", I_FORWARD, 31,
         !e->readonly && e->history_at < e->history_count},
        {"Find...\tCtrl+F", I_DOC, 32, true},
        {"Find next\tCtrl+G", I_FORWARD, 34, e->find[0] != 0},
        {"Replace...\tCtrl+H", I_DOC, 33, !e->readonly},
        {"Select all\tCtrl+A", I_CHECK, 10, e->length > 0},
        {"Copy\tCtrl+C", I_COPY, 11, selected},
        {"Cut\tCtrl+X", I_CUT, 12, selected && !e->readonly},
        {"Paste\tCtrl+V", I_PASTE, 13, !e->readonly}};
    menu_open(w, x, y, items, sizeof(items) / sizeof(*items), editor_command);
  } else if (group == MENU_VIEW) {
    MenuItem items[] = {{"Document start\tCtrl+Home", I_UP, 20, true},
                        {"Document end\tCtrl+End", I_FORWARD, 21, true},
                        {"Preferences...\tCtrl+,", I_GEAR, 22, true}};
    menu_open(w, x, y, items, 3, editor_command);
  }
}

Rect editor_hover(Window *w, int x, int y) {
  Editor *e = w->state;
  Rect b = window_body(w);
  if (e->preferences) {
    Rect done = {b.x + b.w - S(94), b.y + S(13), S(74), S(28)};
    if (contains(done, x, y))
      return done;
    Rect view = {b.x + S(16), b.y + S(58), b.w - S(32), b.h - S(112)};
    if (contains(view, x, y))
      for (int row = 0; row < 8; row++)
        for (int v = 0; v < preference_count(row); v++) {
          Rect r = intersect(view, preference_button(b, row, v, e->pref_top));
          if (contains(r, x, y))
            return r;
        }
  } else {
    for (int id = 0; id < 15; id++) {
      Rect r;
      toolbar_layout(w, id, &r);
      if (contains(r, x, y))
        return r;
    }
  }
  return (Rect){0};
}

bool editor_text_hit(Window *w, int x, int y) {
  Editor *e = w->state;
  Rect b = window_body(w);
  return !e->preferences && contains((Rect){b.x + S(15) + editor_gutter(),
      b.y + toolbar_bottom(w) + S(12), b.w - S(30) - editor_gutter(),
      b.h - toolbar_bottom(w) - S(49)}, x, y);
}

/* Labeled actions wrap at compact window widths. Drawing, hit-testing and the
   document viewport share this layout; none of these controls adds rich text.
 */
static int toolbar_layout(Window *w, int wanted, Rect *out) {
  Rect b = window_body(w);
  int x = S(12), y = S(8), gap = S(5), right = b.w - S(12);
  static const int widths[] = {58,  68, 64, 84, 68, 68, 68, 92,
                               100, 94, 70, 94, 94, 88, 90};
  for (int id = 0; id < 15; id++) {
    int width = S(widths[id]);
    if (id == 8 || x + width > right) {
      x = S(12);
      y += S(32);
    }
    if (id == wanted && out)
      *out = (Rect){b.x + x, b.y + y, width, S(27)};
    x += width + gap;
  }
  return y + S(51);
}
static int toolbar_bottom(Window *w) { return toolbar_layout(w, -1, NULL); }
static bool toolbar_enabled(Window *w, int id) {
  Editor *e = w->state;
  if (id == 2)
    return !e->readonly && e->dirty;
  if (id == 4)
    return !e->readonly && e->history_at > 0;
  if (id == 5)
    return !e->readonly && e->history_at < e->history_count;
  if (id == 7)
    return !e->readonly;
  return true;
}
static const char *toolbar_label(Window *w, int id) {
  (void)w;
  static const char *names[] = {"New",        "Open...",    "Save",
                                "Save As...", "Undo",       "Redo",
                                "Find...",    "Replace...", "Preferences"};
  if (id < 9)
    return names[id];
  if (id == 9)
    return desk.editor_text_size == 11 ? "Text 11 px" : "Text 13 px";
  if (id == 10)
    return desk.editor_tab_width == 2   ? "Tabs 2"
           : desk.editor_tab_width == 4 ? "Tabs 4"
                                        : "Tabs 8";
  if (id == 11)
    return desk.editor_spaces ? "Spaces on" : "Spaces off";
  if (id == 12)
    return desk.editor_autoindent ? "Indent on" : "Indent off";
  if (id == 13)
    return desk.editor_line_numbers ? "Lines on" : "Lines off";
  return desk.editor_whitespace ? "Marks on" : "Marks off";
}
