#include "desktop.h"
#include "terminal_preferences.h"
#include "terminal_screen.h"
#include "text_input_model.h"
#include <uapi/clipboard.h>
#define PASTE_CAPACITY (OS_CLIPBOARD_LIMIT + 13)
typedef struct {
  TerminalTag tag;
  int pid, read_fd, write_fd;
  bool ended, raw_mode, allow_close, selecting, selection_valid;
  int selection_start_row, selection_start_col, selection_end_row,
      selection_end_col;
  unsigned selection_generation;
  VtScreen screen;
  char input_queue[16384];
  size_t input_length, input_offset;
  char *paste;
  size_t paste_length, paste_offset;
  bool paste_ready, paste_raw, paste_bracketed;
  unsigned paste_foreground;
  uint64_t paste_progress;
  char status[160];
  uint64_t status_until;
} Terminal;
static int retired[128], retired_count;
static void notice(Terminal *t, const char *message) {
  copy_text(t->status, sizeof(t->status), message);
  t->status_until = now_ms() + 4000;
}
static Rect screen_rect(Window *w) {
  Rect b = window_body(w);
  return (Rect){b.x + S(10), b.y + S(8), b.w - S(20), b.h - S(16)};
}
static void update_size(Window *w) {
  Terminal *t = w->state;
  Rect r = screen_rect(w);
  int cols = clampi(r.w / MAX(1, text_width("M", terminal_font())), 1, VT_COLS),
      rows = clampi(r.h / terminal_line_height(), 1, VT_ROWS);
  if (cols == t->screen.cols && rows == t->screen.rows)
    return;
  if (!vt_resize(&t->screen, cols, rows)) {
    notice(t, "Terminal resize could not allocate reflow storage.");
    return;
  }
  struct os_winsize size = {rows, cols, r.w, r.h};
  int e = ioctl(t->read_fd, TIOCSWINSZ, &size);
  if (e)
    notice(t, error_text(e));
  t->selection_valid = false;
  invalidate_window(w);
  if (desk.trace)
    printf("DESKTOP TERMINAL_SIZE window=%d cols=%d rows=%d history=%d\n",
           w->id, cols, rows, t->screen.history_count);
}
static void input_flush(Window *w) {
  Terminal *t = w->state;
  if (t->write_fd < 0 || !t->input_length)
    return;
  size_t left = t->input_length - t->input_offset;
  long n = write(t->write_fd, t->input_queue + t->input_offset, left);
  if (n > 0 && (size_t)n <= left) {
    t->input_offset += n;
    if (t->input_offset == t->input_length)
      t->input_offset = t->input_length = 0;
  } else if (n != -E_AGAIN && n != -E_INTR) {
    t->input_offset = t->input_length = 0;
    notice(t, "PTY input failed. Review any partial input.");
    trace("TERMINAL_INPUT_ERROR", w, "PTY write");
  }
}
static void input_queue(Window *w, const char *bytes, size_t count) {
  Terminal *t = w->state;
  input_flush(w);
  if (t->input_offset) {
    memmove(t->input_queue, t->input_queue + t->input_offset,
            t->input_length - t->input_offset);
    t->input_length -= t->input_offset;
    t->input_offset = 0;
  }
  if (count > sizeof(t->input_queue) - t->input_length) {
    notice(t, "Terminal input queue is full. Wait before typing more.");
    trace("TERMINAL_INPUT_FULL", w, "16KiB");
    return;
  }
  memcpy(t->input_queue + t->input_length, bytes, count);
  t->input_length += count;
  input_flush(w);
}
static void update_mode(Window *w) {
  Terminal *t = w->state;
  struct os_termios mode;
  if (t->read_fd < 0 || ioctl(t->read_fd, TCGETS, &mode))
    return;
  bool raw = !(mode.lflag & ICANON);
  if (raw != t->raw_mode) {
    t->raw_mode = raw;
    trace("TERMINAL_MODE", w, raw ? "raw" : "canonical");
  }
}
int terminal_open(Window *w, const char *path) {
  if (path && !strcmp(path, "@preferences"))
    return terminal_prefs_init(w);
  Terminal *t = allocate(sizeof(*t));
  if (!t)
    return -E_NOMEM;
  memset(t, 0, sizeof(*t));
  vt_init(&t->screen);
  t->read_fd = t->write_fd = -1;
  int pair[2];
  int e = call2(SYS_pty, pair, O_CLOEXEC);
  if (e) {
    deallocate(t, sizeof(*t));
    return e;
  }
  struct os_termios mode;
  e = ioctl(pair[0], TCGETS, &mode);
  if (!e) {
    mode.lflag |= ECHO | ICANON | ISIG;
    /* The legacy native shell has no fg/bg recovery; do not strand its child.
       GNU Bash uses the POSIX job-control discipline on a fresh PTY. */
    mode.cc[VSUSP]=desk.terminal_shell?26:0;
    e = ioctl(pair[0], TCSETS, &mode);
  }
  if (e) {
    close(pair[0]);
    close(pair[1]);
    deallocate(t, sizeof(*t));
    return e;
  }
  char uid[24];format_uint(uid,profiles_uid(profiles_active()));
  char *args[]={"/bin/profile-session",uid,desk.terminal_shell?"bash":"native",desk.home,NULL};
  const char *program="/bin/profile-session";
  struct os_spawn_io spec = {.fd = {pair[1], pair[1], pair[1]},
                             .cwd = (uintptr_t)(path ? path : desk.home)};
  int pid = call3(SYS_spawn_io, program, args, &spec);
  close(pair[1]);
  if (pid < 0) {
    close(pair[0]);
    deallocate(t, sizeof(*t));
    return pid;
  }
  t->pid = pid;
  t->read_fd = t->write_fd = pair[0];
  w->state = t;
  call3(SYS_fcntl, t->read_fd, F_SETFL, O_RDWR | O_NONBLOCK);
  update_size(w);
  trace("TERMINAL_SHELL", w,
        desk.terminal_shell ? "/usr/bin/linux-bash --noprofile -i"
                            : "/bin/coast-sh --terminal");
  trace("TERMINAL_INLINE", w, "direct PTY input and unified VT output");
  trace("TERMINAL_CWD", w, path ? path : desk.home);
  return 0;
}
static unsigned scalar(char *out, unsigned c) {
  if (c < 128) {
    out[0] = c;
    return 1;
  }
  if (c < 2048) {
    out[0] = 0xc0 | (c >> 6);
    out[1] = 0x80 | (c & 63);
    return 2;
  }
  if (c < 65536) {
    out[0] = 0xe0 | (c >> 12);
    out[1] = 0x80 | ((c >> 6) & 63);
    out[2] = 0x80 | (c & 63);
    return 3;
  }
  out[0] = 0xf0 | (c >> 18);
  out[1] = 0x80 | ((c >> 12) & 63);
  out[2] = 0x80 | ((c >> 6) & 63);
  out[3] = 0x80 | (c & 63);
  return 4;
}
static void selection_range(Terminal *t, int *first, int *last) {
  *first = t->selection_start_row * VT_COLS + t->selection_start_col;
  *last = t->selection_end_row * VT_COLS + t->selection_end_col;
  if (*first > *last) {
    int swap = *first;
    *first = *last;
    *last = swap;
  }
}
static void terminal_copy(Window *w) {
  Terminal *t = w->state;
  if (!t->selection_valid)
    return;
  if (t->selection_generation != t->screen.history_generation) {
    t->selection_valid = false;
    notice(t, "Selection expired after history/reflow changed.");
    return;
  }
  char *text = allocate(OS_CLIPBOARD_LIMIT + 1);
  if (!text) {
    dialog_error(w, "Copy text", -E_NOMEM);
    return;
  }
  int first, last;
  selection_range(t, &first, &last);
  int top = first / VT_COLS, bottom = last / VT_COLS;
  size_t length = 0;
  for (int row = top; row <= bottom; row++) {
    const VtCell *line = vt_row(&t->screen, row);
    if (!line)
      break;
    int start = row == top ? first % VT_COLS : 0,
        end = row == bottom ? last % VT_COLS : t->screen.cols;
    if (!vt_wrapped(&t->screen, row))
      while (end > start && (!line[end - 1].ch || line[end - 1].ch == ' '))
        end--;
    for (int col = start; col < end; col++) {
      char bytes[4];
      unsigned n = scalar(bytes, line[col].ch ? line[col].ch : ' ');
      if (length + n > OS_CLIPBOARD_LIMIT)
        goto too_large;
      memcpy(text + length, bytes, n);
      length += n;
    }
    if (row < bottom && !vt_wrapped(&t->screen, row)) {
      if (length == OS_CLIPBOARD_LIMIT)
        goto too_large;
      text[length++] = '\n';
    }
  }
  {
    int error = clipboard_write(text, length);
    deallocate(text, OS_CLIPBOARD_LIMIT + 1);
    if (error)
      clipboard_error(w, "Copy text", error);
    else
      trace("CLIPBOARD_COPY", w, "Terminal VT selection");
  }
  return;
too_large:
  deallocate(text, OS_CLIPBOARD_LIMIT + 1);
  dialog_notice(w, "Copy text",
                "Select a smaller range; text clipboard limit is64KiB.");
}
static void paste_cancel(Terminal *t, const char *status) {
  if (t->paste)
    deallocate(t->paste, PASTE_CAPACITY);
  t->paste = NULL;
  t->paste_length = t->paste_offset = 0;
  t->paste_ready = false;
  if (status)
    copy_text(t->status, sizeof(t->status), status);
}

static bool paste_target(Terminal *t) {
  unsigned foreground = 0;
  struct os_termios mode;
  return !t->ended && t->write_fd >= 0 &&
         !ioctl(t->write_fd, OS_PTY_GET_FOREGROUND, &foreground) &&
         foreground == t->paste_foreground &&
         !ioctl(t->write_fd, TCGETS, &mode) &&
         (!(mode.lflag & ICANON)) == t->paste_raw &&
         (!t->paste_raw || t->screen.bracketed_paste == t->paste_bracketed);
}

static void paste_accept(Window *w, const char *unused) {
  (void)unused;
  if (!w || !w->alive || !w->state)
    return;
  Terminal *t = w->state;
  if (!t->paste)
    return;
  if (!paste_target(t)) {
    paste_cancel(t, "Paste canceled: the foreground program changed.");
    dialog_notice(w, "Paste canceled",
                  "The terminal program changed while the paste was being "
                  "reviewed. Copy and "
                  "paste again into the intended program.");
  } else {
    t->paste_ready = true;
    t->paste_progress = now_ms();
    copy_text(t->status, sizeof(t->status),
              "Pasting text. Escape cancels the remaining text.");
    trace("CLIPBOARD_PASTE_ACCEPT", w, "Terminal");
  }
  invalidate_window(w);
}

static void terminal_paste(Window *w) {
  update_mode(w);
  Terminal *t = w->state;
  if (t->paste) {
    dialog_notice(w, "Paste in progress",
                  "Wait for this paste to finish, or press Escape to cancel "
                  "its remaining text.");
    return;
  }
  char *copy;
  size_t length;
  int error = clipboard_read(&copy, &length);
  if (error) {
    clipboard_error(w, "Paste text", error);
    return;
  }
  if (!length) {
    clipboard_release(copy);
    return;
  }
  /* Normalize CRLF/CR once; other control characters were rejected when
     copied. The queue is a frozen snapshot, independent of later copies. */
  size_t n = 0;
  bool multiline = false;
  for (size_t i = 0; i < length; i++) {
    char c = copy[i];
    if (c == '\r') {
      if (i + 1 < length && copy[i + 1] == '\n')
        i++;
      c = '\n';
    }
    copy[n++] = c;
    multiline |= c == '\n';
  }
  length = n;
  copy[length] = 0;
  t->paste = allocate(PASTE_CAPACITY);
  if (!t->paste) {
    clipboard_release(copy);
    dialog_error(w, "Paste text", -E_NOMEM);
    return;
  }
  t->paste_raw = t->raw_mode;
  t->paste_bracketed = t->screen.bracketed_paste;
  t->paste_offset = 0;
  t->paste_ready = false;
  if (ioctl(t->write_fd, OS_PTY_GET_FOREGROUND, &t->paste_foreground)) {
    clipboard_release(copy);
    paste_cancel(t, NULL);
    dialog_error(w, "Paste text", -E_IO);
    return;
  }
  size_t offset = 0;
  if (t->paste_bracketed) {
    memcpy(t->paste, "\033[200~", 6);
    offset = 6;
  }
  memcpy(t->paste + offset, copy, length);
  offset += length;
  if (t->paste_bracketed) {
    memcpy(t->paste + offset, "\033[201~", 6);
    offset += 6;
  }
  t->paste_length = offset;
  clipboard_release(copy);
  t->selection_valid = t->selecting = false;
  if (multiline || length > 256) {
    char message[640], num[32];
    copy_text(message, sizeof(message), "Paste ");
    format_uint(num, length);
    append_text(message, sizeof(message), num);
    append_text(message, sizeof(message),
                " bytes into this terminal? Newlines may run commands. Confirm "
                "only when this "
                "is the intended program. Escape cancels.\n\nPreview:\n");
    size_t available = length;
    size_t preview = MIN(available, (size_t)180), used = strlen(message);
    const char *preview_text = t->paste + (t->paste_bracketed ? 6 : 0);
    /* Never end a visible preview in the middle of a UTF-8 character. */
    while (preview && preview < available &&
           ((unsigned char)preview_text[preview] & 0xc0) == 0x80)
      preview--;
    memcpy(message + used, preview_text, preview);
    message[used + preview] = 0;
    if (preview < available)
      append_text(message, sizeof(message), " ...");
    dialog_confirm(w, multiline ? "Paste multiple lines?" : "Paste large text?",
                   message, paste_accept);
    Rect old = desk.dialog.box;
    desk.dialog.box.h = MIN(S(380), desk.height - S(DESKTOP_TOP + 16));
    desk.dialog.box.y =
        S(DESKTOP_TOP) + (desk.height - S(DESKTOP_TOP) - desk.dialog.box.h) / 2;
    invalidate(old);
    invalidate(desk.dialog.box);
    trace("CLIPBOARD_PASTE_REVIEW", w, "Terminal frozen text");
  } else
    paste_accept(w, "");
  return;
}

static void paste_tick(Window *w) {
  Terminal *t = w->state;
  if (!t->paste)
    return;
  if (!t->paste_ready) {
    if (!desk.dialog.open || desk.dialog.owner != w ||
        desk.dialog.action != paste_accept) {
      paste_cancel(t, "Paste canceled.");
      invalidate_window(w);
    }
    return;
  }
  if (!paste_target(t)) {
    paste_cancel(t, "Paste stopped: the foreground program changed. Review "
                    "terminal input.");
    trace("CLIPBOARD_PASTE_STOP", w, "Foreground changed");
    invalidate_window(w);
    return;
  }
  /* Preserve FIFO ordering when an earlier key write only partly fit in the
     PTY. The program may drain it between two writes in this same GUI tick. */
  if (t->input_length) {
    if (now_ms() - t->paste_progress > 30000)
      paste_cancel(t, "Paste stopped while waiting for earlier terminal input.");
    return;
  }
  size_t count = MIN(t->paste_length - t->paste_offset, (size_t)256);
  long written = write(t->write_fd, t->paste + t->paste_offset, count);
  if (written > 0) {
    t->paste_offset += written;
    t->paste_progress = now_ms();
    if (t->paste_offset == t->paste_length) {
      paste_cancel(t, "Paste delivered to the PTY.");
      trace("CLIPBOARD_PASTE_DONE", w, "Terminal");
      invalidate_window(w);
    }
  } else if ((written < 0 && written != -E_AGAIN && written != -E_INTR) ||
             now_ms() - t->paste_progress > 30000) {
    paste_cancel(
        t,
        "Paste stopped. Review any partial terminal input before continuing.");
    trace("CLIPBOARD_PASTE_STOP", w, "Output unavailable");
    invalidate_window(w);
  }
}

void terminal_draw(Window *w) {
  if (terminal_is_preferences(w)) {
    terminal_prefs_draw(w);
    return;
  }
  Terminal *t = w->state;
  Rect b = window_body(w), area = screen_rect(w);
  if (desk.terminal_opacity < 100)
    window_material(w, b, 1);
  else
    fill(b, terminal_background());
  vt_draw(&t->screen, area, wm_focused() == w && !t->ended);
  if (t->selection_valid &&
      t->selection_generation == t->screen.history_generation) {
    int first, last;
    selection_range(t, &first, &last);
    int cw = MAX(1, text_width("M", terminal_font())),
        lh = terminal_line_height();
    for (int y = 0; y < t->screen.rows; y++) {
      int row = vt_view_row(&t->screen, y);
      int start = clampi((int64_t)first - row * VT_COLS, 0, t->screen.cols),
          end = clampi((int64_t)last - row * VT_COLS, 0, t->screen.cols);
      if (end > start)
        blend((Rect){area.x + start * cw, area.y + y * lh, (end - start) * cw,
                     lh},
              desk.selection, 100);
    }
  }
  if (t->status_until > now_ms()) {
    Rect status = {area.x, area.y + area.h - S(22), area.w, S(22)};
    rounded(status, S(4), THEME_TOOLBAR);
    label(status.x + S(6), status.y + S(5), t->status, FONT_SMALL, THEME_TEXT,
          status.w - S(12));
  }
}
bool terminal_mouse(Window *w, int x, int y, unsigned pressed, int wheel) {
  if (terminal_is_preferences(w))
    return terminal_prefs_mouse(w, x, y, pressed, wheel);
  Terminal *t = w->state;
  Rect area = screen_rect(w);
  if (wheel && !t->screen.alternate_screen) {
    vt_scroll(&t->screen, wheel * 3);
    invalidate_window(w);
    return true;
  }
  if ((pressed & 1) && contains(area, x, y)) {
    t->selecting = true;
    t->selection_valid = false;
    t->selection_generation = t->screen.history_generation;
  }
  if (t->selecting) {
    int col = clampi((x - area.x) / MAX(1, text_width("M", terminal_font())), 0,
                     t->screen.cols),
        row = vt_view_row(&t->screen,
                          clampi((y - area.y) / terminal_line_height(), 0,
                                 t->screen.rows - 1));
    if (pressed & 1) {
      t->selection_start_row = row;
      t->selection_start_col = col;
    }
    t->selection_end_row = row;
    t->selection_end_col = col;
    t->selection_valid =
        t->selection_start_row != row || t->selection_start_col != col;
    if (!(desk.buttons & 1))
      t->selecting = false;
    invalidate_window(w);
    return true;
  }
  return contains(area, x, y);
}
void terminal_key(Window *w, Key k) {
  if (terminal_is_preferences(w)) {
    terminal_prefs_key(w, k);
    return;
  }
  if (!k.down)
    return;
  Terminal *t = w->state;
  unsigned letter =
      k.code >= 4 && k.code <= 0x1d ? 'a' + k.code - 4 : k.ascii | 32;
  if ((k.mods & MOD_CTRL) && k.ascii == ',') {
    terminal_preferences_open(w);
    return;
  }
  if ((k.mods & MOD_CTRL) && (k.mods & MOD_SHIFT) && letter == 'c') {
    terminal_copy(w);
    return;
  }
  if (t->ended)
    return;
  if (t->paste &&
      (k.code == KEY_ESCAPE || ((k.mods & MOD_CTRL) && letter == 'c'))) {
    paste_cancel(t, "Paste canceled; review any already delivered text.");
    notice(t, t->status);
    invalidate_window(w);
    if (k.code == KEY_ESCAPE)
      return;
  }
  if (((k.mods & MOD_CTRL) && (k.mods & MOD_SHIFT) && letter == 'v') ||
      ((k.mods & MOD_SHIFT) && k.code == KEY_INSERT)) {
    terminal_paste(w);
    return;
  }
  if (t->paste)
    return;
  if ((k.mods & MOD_SHIFT) &&
      (k.code == KEY_PAGEUP || k.code == KEY_PAGEDOWN) &&
      !t->screen.alternate_screen) {
    vt_scroll(&t->screen,
              (k.code == KEY_PAGEUP ? 1 : -1) * MAX(1, t->screen.rows - 2));
    invalidate_window(w);
    return;
  }
  vt_scroll(&t->screen, -VT_HISTORY);
  t->selection_valid = false;
  char bytes[16];
  int n = vt_key(&t->screen, k, bytes, sizeof(bytes));
  if (n)
    input_queue(w, bytes, n);
  invalidate_window(w);
}
void terminal_tick(Window *w) {
  if (terminal_is_preferences(w)) {
    terminal_prefs_tick(w);
    return;
  }
  Terminal *t = w->state;
  update_size(w);
  update_mode(w);
  input_flush(w);
  paste_tick(w);
  bool changed = false;
  /* Diagnostic mirroring writes synchronously to the serial console. Bound
     each traced tick so large observations cannot postpone input dispatch
     behind a 16-KiB burst. Retain every byte in the PTY for later ticks. */
  int read_passes = desk.trace ? 1 : 8;
  size_t read_bytes = desk.trace ? 256 : 2048;
  for (int pass = 0; pass < read_passes && t->read_fd >= 0; pass++) {
    char buffer[2049];
    long n = read(t->read_fd, buffer, read_bytes);
    if (n <= 0)
      break;
    for (long i = 0; i < n; i++)
      vt_byte(&t->screen, (unsigned char)buffer[i]);
    char response[128];
    size_t count = vt_response(&t->screen, response, sizeof(response));
    if (count)
      input_queue(w, response, count);
    if (t->selection_generation != t->screen.history_generation)
      t->selection_valid = t->selecting = false;
    buffer[n] = 0;
    uint64_t trace_started = now_ms();
    trace("TERMINAL_OUTPUT", w, buffer);
    uint64_t trace_ms = now_ms() - trace_started;
    static unsigned slow_trace_logs;
    if (desk.trace && trace_ms >= 40 && slow_trace_logs++ < 32)
      printf("DESKTOP TERMINAL_TRACE_TIMING bytes=%ld elapsed_ms=%llu\n", n,
             (unsigned long long)trace_ms);
    changed = true;
  }
  update_mode(w);
  input_flush(w);
  if (t->selecting && !(desk.buttons & 1))
    t->selecting = false;
  if (t->pid > 0) {
    int status;
    int result = waitpid(t->pid, &status, 1);
    if (result > 0 || result == -E_CHILD) {
      t->ended = true;
      paste_cancel(t, NULL);
      t->pid = 0;
      if (t->read_fd >= 0)
        close(t->read_fd);
      t->read_fd = t->write_fd = -1;
      notice(t, "Shell exited; open a new Terminal from its menu.");
      trace("TERMINAL_EXIT", w, "process ended");
      changed = true;
    }
  }
  if (t->status_until && now_ms() >= t->status_until) {
    t->status_until = 0;
    changed = true;
  }
  if (changed)
    invalidate_window(w);
}
static unsigned parse_pid(const char **p) {
  unsigned n = 0;
  while (**p == ' ') {
    (*p)++;
  }
  while (**p >= '0' && **p <= '9') {
    n = n * 10 + *(*p)++ - '0';
  }
  return n;
}

static bool session_signal(Terminal *t, bool close_session) {
  char buffer[16384];
  long n = call3(SYS_sysinfo, INFO_PROCESSES, buffer, sizeof(buffer));
  if (n < 0) {
    return false;
  }
  buffer[MIN(n, (long)sizeof(buffer) - 1)] = 0;
  unsigned pid[128], parent[128];
  bool child[128] = {0};
  int count = 0;
  const char *p = buffer;
  while (*p && *p != '\n') {
    p++;
  }
  if (*p) {
    p++;
  }
  while (*p && count < 128) {
    pid[count] = parse_pid(&p);
    parent[count] = parse_pid(&p);
    child[count] = pid[count] == (unsigned)t->pid;
    count++;
    while (*p && *p != '\n') {
      p++;
    }
    if (*p) {
      p++;
    }
  }

  for (int pass = 0; pass < 128; pass++) {
    bool changed = false;
    for (int i = 0; i < count; i++) {
      if (!child[i]) {
        for (int j = 0; j < count; j++) {
          if (child[j] && parent[i] == pid[j]) {
            child[i] = true;
            changed = true;
            break;
          }
        }
      }
    }

    if (!changed) {
      break;
    }
  }

  bool killed = false;
  for (int i = count - 1; i >= 0; i--) {
    if (child[i] && pid[i] > 1 &&
        (close_session || pid[i] != (unsigned)t->pid)) {
      call2(SYS_kill, pid[i], close_session ? 9 : 2);
      killed = true;
    }
  }

  return killed;
}

static void confirm_close(Window *w, const char *unused) {
  (void)unused;
  if (w && w->alive) {
    if (power_review_accept(w)) {
      return;
    }
    ((Terminal *)w->state)->allow_close = true;
    wm_close(w);
  }
}

bool terminal_can_close(Window *w) {
  if (terminal_is_preferences(w))
    return true;
  if (power_review_approved(w)) {
    return true;
  }
  Terminal *t = w->state;
  if (t->allow_close || t->ended) {
    return true;
  }
  unsigned pid = 0;
  int query =
      t->read_fd >= 0 ? ioctl(t->read_fd, OS_PTY_GET_FOREGROUND, &pid) : -E_IO;
  if (query || (pid && pid != (unsigned)t->pid)) {
    bool power = power_review_pending(w);
    dialog_confirm(
        w, power ? "Continue with running program?" : "Close running program?",
        query   ? "Terminal activity could not be checked. Save any terminal "
                  "work first. "
                  "Confirm to allow this operation, or cancel."
        : power ? "A terminal program is running. Confirm to allow the "
                  "system-power "
                  "request, or cancel and save its work. The program stays "
                  "open during "
                  "the reversible request; accepting shutdown can end it "
                  "without saving."
                : "A terminal program is running. Closing this window will "
                  "stop it. Save your "
                  "work before closing.",
        confirm_close);
    return false;
  }

  return true;
}

bool terminal_close(Window *w) {
  if (terminal_is_preferences(w))
    return terminal_prefs_close(w);
  Terminal *t = w->state;
  paste_cancel(t, NULL);
  if (t->pid > 0) {
    session_signal(t, true);
    if (retired_count < 128) {
      retired[retired_count++] = t->pid;
    }
  }

  if (t->read_fd >= 0) {
    close(t->read_fd);
  }
  if (t->write_fd >= 0 && t->write_fd != t->read_fd) {
    close(t->write_fd);
  }
  deallocate(t, sizeof(*t));
  w->state = NULL;
  return true;
}

void terminal_reap(void) {
  for (int i = 0; i < retired_count; i++) {
    int status;
    int result = waitpid(retired[i], &status, 1);
    if (result > 0 || result == -E_CHILD) {
      retired[i--] = retired[--retired_count];
    }
  }
}

static void terminal_command(Window *w, int command) {
  Terminal *t = w->state;
  if (command == 0)
    wm_open(APP_TERMINAL, NULL);
  else if (command == 1)
    wm_close(w);
  else if (command == 2)
    terminal_copy(w);
  else if (command == 3)
    terminal_paste(w);
  else if (command == 8)
    terminal_preferences_open(w);
  else {
    if (command == 4)
      t->screen.view_offset = 0;
    else if (command == 5)
      vt_scroll(&t->screen, VT_HISTORY);
    else
      vt_scroll(&t->screen,
                (command == 6 ? 1 : -1) * MAX(1, t->screen.rows - 2));
    invalidate_window(w);
  }
}
void terminal_menu(Window *w, int group, int x, int y) {
  if (terminal_is_preferences(w)) {
    terminal_prefs_menu(w, group, x, y);
    return;
  }
  Terminal *t = w->state;
  if (group == MENU_APP) {
    MenuItem items[] = {{"Preferences...\tCtrl+,", I_GEAR, 8, true},
                        {"New Terminal\tSuper+T", I_TERMINAL, 0, true},
                        {"Close Terminal\tAlt+F4", I_CLOSE, 1, true}};
    menu_open(w, x, y, items, 3, terminal_command);
  } else if (group == MENU_FILE) {
    MenuItem items[] = {{"New Terminal\tSuper+T", I_TERMINAL, 0, true},
                        {"Preferences...\tCtrl+,", I_GEAR, 8, true},
                        {"Close Terminal\tAlt+F4", I_CLOSE, 1, true}};
    menu_open(w, x, y, items, 3, terminal_command);
  } else if (group == MENU_EDIT) {
    MenuItem items[] = {
        {"Copy selection\tCtrl+Shift+C", I_COPY, 2, t->selection_valid},
        {"Paste text...\tCtrl+Shift+V", I_PASTE, 3, !t->ended && !t->paste}};
    menu_open(w, x, y, items, 2, terminal_command);
  } else if (group == MENU_VIEW) {
    bool scroll = !t->screen.alternate_screen;
    MenuItem items[] = {{"Latest output", I_CLOCK, 4, scroll},
                        {"Oldest output", I_UP, 5, scroll},
                        {"Scroll up\tShift+PageUp", I_UP, 6, scroll},
                        {"Scroll down\tShift+PageDown", I_FORWARD, 7, scroll}};
    menu_open(w, x, y, items, 4, terminal_command);
  }
}

Rect terminal_hover(Window *w, int x, int y) {
  return terminal_is_preferences(w) ? terminal_prefs_hover(w, x, y) : (Rect){0};
}
