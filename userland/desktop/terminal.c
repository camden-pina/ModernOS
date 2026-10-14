#include "desktop.h"
#include "terminal_screen.h"
#include "smooth_scroll.h"

#define TERM_LINES 512
#define TERM_COLS 180

typedef struct {
    char lines[TERM_LINES][TERM_COLS + 1], input[512], history[32][512], status[160];
    int row, col, scroll, cursor, history_count, history_position;
    int pid, read_fd, write_fd;
    int escape, param[8], param_count;
    bool ended;
    bool raw_mode, allow_close;
    VtScreen screen;
    SmoothScroll animation;
} Terminal;

static int retired[128], retired_count;

static Rect screen_rect(Window *w) {
    Rect b = window_body(w);

    return (Rect){b.x + S(14), b.y + S(10), b.w - S(28), b.h - S(36)};
}

static void update_size(Window *w) {
    Terminal *t = w->state;
    Rect r = screen_rect(w);
    int cols = clampi(r.w / MAX(1, text_width("M", FONT_MONO)), 1, VT_COLS),
        rows = clampi(r.h / S(17), 1, VT_ROWS);
    if (cols == t->screen.cols && rows == t->screen.rows) {
        return;
    }
    vt_resize(&t->screen, cols, rows);
    struct os_winsize size = {rows, cols, r.w, r.h};
    int e = ioctl(t->read_fd, TIOCSWINSZ, &size);
    if (e) {
        copy_text(t->status, sizeof(t->status), error_text(e));
    }
    invalidate_window(w);
}

static void term_newline(Terminal *t) {
    t->row++;
    t->col = 0;
    if (t->row >= TERM_LINES) {
        memmove(t->lines, t->lines + 1, (TERM_LINES - 1) * (TERM_COLS + 1));
        t->row = TERM_LINES - 1;
    }
    t->lines[t->row][0] = 0;
}

static void term_byte(Terminal *t, unsigned c) {
    if (t->escape == 1) {
        t->escape = c == '[' ? 2 : 0;
        t->param_count = 0;
        memset(t->param, 0, sizeof(t->param));
        return;
    }

    if (t->escape == 2) {
        if (c >= '0' && c <= '9') {
            t->param[t->param_count] = MIN(999, t->param[t->param_count] * 10 + (int)c - '0');
            return;
        }
        if (c == ';') {
            if (t->param_count < 7) {
                t->param_count++;
            }
            return;
        }

        if (c == 'J' && t->param[0] == 2) {
            memset(t->lines, 0, sizeof(t->lines));
            t->row = t->col = 0;
        } else if (c == 'K') {
            if (t->param[0] == 2) {
                t->lines[t->row][0] = 0;
                t->col = 0;
            } else {
                t->lines[t->row][t->col] = 0;
            }
        } else if (c == 'H' || c == 'f') {
            t->col = clampi(t->param[1] - 1, 0, TERM_COLS - 1);
            if (t->param[0] == 1) {
                t->row = 0;
            }
        }

        t->escape = 0;
        return;
    }

    if (c == 27) {
        t->escape = 1;
        return;
    }
    if (c == '\n') {
        term_newline(t);
        return;
    }
    if (c == '\r') {
        t->col = 0;
        return;
    }
    if (c == 8) {
        if (t->col) {
            t->col--;
        }
        return;
    }

    if (c == '\t') {
        int n = 4 - t->col % 4;
        while (n--) {
            term_byte(t, ' ');
        }
        return;
    }

    if (c < 32 || c == 127) {
        return;
    }
    if (t->col == TERM_COLS) {
        term_newline(t);
    }
    int len = strlen(t->lines[t->row]);
    while (len < t->col) {
        t->lines[t->row][len++] = ' ';
    }
    t->lines[t->row][t->col++] = c;
    t->lines[t->row][MAX(t->col, len)] = 0;
}

int terminal_open(Window *w, const char *path) {
    (void)path;
    Terminal *t = allocate(sizeof(*t));
    if (!t) {
        return -E_NOMEM;
    }
    memset(t, 0, sizeof(*t));
    vt_init(&t->screen);
    t->read_fd = t->write_fd = -1;
    w->state = t;
    int pair[2];
    int e = call2(SYS_pty, pair, O_CLOEXEC);
    if (e) {
        deallocate(t, sizeof(*t));
        return e;
    }
    struct os_termios mode;
    e = ioctl(pair[0], TCGETS, &mode);
    if (!e) {
        mode.lflag &= ~ECHO;
        e = ioctl(pair[0], TCSETS, &mode);
    }
    if (e) {
        close(pair[0]);
        close(pair[1]);
        deallocate(t, sizeof(*t));
        return e;
    }

    char *args[] = {"/bin/modernos-sh", "--terminal", NULL};
    struct os_spawn_io spec = {.fd = {pair[1], pair[1], pair[1]}, .cwd = (uintptr_t)desk.home};
    int pid = call3(SYS_spawn_io, "/bin/modernos-sh", args, &spec);
    close(pair[1]);
    if (pid < 0) {
        close(pair[0]);
        deallocate(t, sizeof(*t));
        return pid;
    }
    t->pid = pid;
    t->read_fd = t->write_fd = pair[0];
    call3(SYS_fcntl, t->read_fd, F_SETFL, O_RDWR | O_NONBLOCK);
    update_size(w);
    copy_text(t->status, sizeof(t->status),
              "Native PTY | GNU nano and terminal applications supported");
    trace("TERMINAL_SHELL", w, "/bin/modernos-sh --terminal");
    return 0;
}

void terminal_draw(Window *w) {
    Terminal *t = w->state;
    Rect b = window_body(w);
    fill(b, theme_color(0xFAFCFB));
    if (t->raw_mode) {
        vt_draw(&t->screen, screen_rect(w), wm_focused() == w);
        label(b.x + S(16), b.y + b.h - S(21), "Native PTY | application controls the terminal",
              FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(32));
        return;
    }

    int line_height = S(17), rows = MAX(1, (b.h - S(81)) / line_height);
    int base = MAX(0, (t->row + 1 - rows) * line_height);
    int position = MAX(0, base - t->scroll), first = position / line_height;
    int offset = position % line_height;
    Rect saved = desk.clip;
    clip_set(intersect(saved, (Rect){b.x + S(16), b.y + S(13), b.w - S(32), b.h - S(78)}));
    for (int i = 0; i <= rows && first + i <= t->row; i++) {
        label(b.x + S(16), b.y + S(13) + i * line_height - offset, t->lines[first + i], FONT_MONO,
              THEME_TEXT, b.w - S(32));
    }
    desk.clip = saved;
    Rect input = {b.x + S(14), b.y + b.h - S(58), b.w - S(28), S(29)};
    input_draw(input, t->input,
               t->ended ? "Shell exited - close and reopen Terminal" : "Enter a command", t->cursor,
               wm_focused() == w && !t->ended);
    label(b.x + S(16), b.y + b.h - S(21), t->status, FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(32));
}

bool terminal_mouse(Window *w, int x, int y, unsigned pressed, int wheel) {
    (void)x;
    (void)y;
    Terminal *t = w->state;
    if (wheel && !t->raw_mode) {
        Rect b = window_body(w);
        int rows = MAX(1, (b.h - S(81)) / S(17));
        smooth_by(&t->animation, t->scroll, (int64_t)wheel * S(51),
                  MAX(0, (t->row + 1 - rows) * S(17)), now_ms());
    }

    if (pressed & 1) {
        t->cursor = strlen(t->input);
        invalidate_window(w);
    }
    return true;
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
        if (child[i] && pid[i] > 1 && (close_session || pid[i] != (unsigned)t->pid)) {
            call2(SYS_kill, pid[i], close_session ? 9 : 2);
            killed = true;
        }
    }

    return killed;
}

void terminal_key(Window *w, Key k) {
    if (!k.down) {
        return;
    }
    Terminal *t = w->state;
    if (t->ended) {
        return;
    }
    if (t->raw_mode) {
        char bytes[16];
        int n = vt_key(&t->screen, k, bytes, sizeof(bytes));
        if (n) {
            long result = write(t->write_fd, bytes, n);
            if (result != n) {
                dialog_error(w, "Terminal input", result < 0 ? result : -E_IO);
            }
        }

        return;
    }

    if ((k.mods & MOD_CTRL) && (k.ascii == 'c' || k.ascii == 'C')) {
        t->input[0] = 0;
        t->cursor = 0;
        if (!session_signal(t, false)) {
            write(t->write_fd, "\n", 1);
        }
        copy_text(t->status, sizeof(t->status),
                  "Interrupted child commands. Native shell remains available.");
        invalidate_window(w);
        return;
    }

    if (k.code == KEY_UP || k.code == KEY_DOWN) {
        int pos = clampi(t->history_position + (k.code == KEY_UP ? -1 : 1), 0, t->history_count);
        t->history_position = pos;
        copy_text(t->input, sizeof(t->input), pos < t->history_count ? t->history[pos % 32] : "");
        t->cursor = strlen(t->input);
        invalidate_window(w);
        return;
    }

    if (k.code == KEY_PAGEUP || k.code == KEY_PAGEDOWN) {
        Rect b = window_body(w);
        int rows = MAX(1, (b.h - S(81)) / S(17));
        smooth_by(&t->animation, t->scroll, (k.code == KEY_PAGEUP ? 12 : -12) * S(17),
                  MAX(0, (t->row + 1 - rows) * S(17)), now_ms());
        invalidate_window(w);
        return;
    }

    if (k.code == KEY_ENTER) {
        char command[514];
        copy_text(command, sizeof(command), t->input);
        append_text(command, sizeof(command), "\n");
        long n = write(t->write_fd, command, strlen(command));
        if (n != (long)strlen(command)) {
            copy_text(t->status, sizeof(t->status),
                      n < 0 ? error_text(n)
                            : "Command delivery was incomplete; shell input is unavailable.");
            invalidate_window(w);
            return;
        }

        struct os_termios mode;
        if (ioctl(t->write_fd, TCGETS, &mode) || !(mode.lflag & ECHO)) {
            for (const char *p = t->input; *p; p++) {
                term_byte(t, *p);
            }
            term_byte(t, '\n');
        }

        if (t->input[0]) {
            if (t->history_count == 32) {
                memmove(t->history, t->history + 1, 31 * 512);
                t->history_count = 31;
            }
            copy_text(t->history[t->history_count++], 512, t->input);
        }

        t->history_position = t->history_count;
        t->input[0] = 0;
        t->cursor = 0;
        t->scroll = 0;
        smooth_jump(&t->animation, 0, now_ms());
        invalidate_window(w);
        trace("TERMINAL_COMMAND", w, command);
        return;
    }

    if (input_key(t->input, sizeof(t->input), &t->cursor, k)) {
        invalidate_window(w);
    }
}

void terminal_tick(Window *w) {
    Terminal *t = w->state;
    bool changed = false;
    if (!t->raw_mode) {
        Rect b = window_body(w);
        int rows = MAX(1, (b.h - S(81)) / S(17));
        int scroll =
            smooth_tick(&t->animation, t->scroll, MAX(0, (t->row + 1 - rows) * S(17)), now_ms());
        if (scroll != t->scroll) {
            t->scroll = scroll;
            trace_scroll(w, "Terminal", scroll, t->animation.target);
            invalidate((Rect){b.x + S(16), b.y + S(13), b.w - S(32), b.h - S(78)});
        }
    }

    if (t->read_fd >= 0) {
        update_size(w);
        struct os_termios mode;
        if (!ioctl(t->read_fd, TCGETS, &mode)) {
            bool raw = !(mode.lflag & ICANON);
            if (raw != t->raw_mode) {
                t->raw_mode = raw;
                if (raw) {
                    t->input[0] = 0;
                    t->cursor = 0;
                }
                invalidate_window(w);
            }
        }
    }

    char buffer[4097];
    if (t->read_fd >= 0) {
        for (int chunk = 0; chunk < 4; chunk++) {
            long n = read(t->read_fd, buffer, sizeof(buffer) - 1);
            if (n == -E_AGAIN || n == -E_INTR) {
                break;
            }
            if (n <= 0) {
                close(t->read_fd);
                t->read_fd = t->write_fd = -1;
                if (n < 0) {
                    copy_text(t->status, sizeof(t->status), error_text(n));
                }
                break;
            }

            for (long i = 0; i < n; i++) {
                vt_byte(&t->screen, (unsigned char)buffer[i]);
                if (!t->raw_mode) {
                    term_byte(t, (unsigned char)buffer[i]);
                }
            }

            buffer[n] = 0;
            trace("TERMINAL_OUTPUT", w, buffer);
            changed = true;
        }
    }

    if (t->pid > 0) {
        int status;
        int result = waitpid(t->pid, &status, 1);
        if (result > 0 || result == -E_CHILD) {
            t->ended = true;
            t->pid = 0;
            if (t->write_fd >= 0) {
                close(t->write_fd);
            }
            t->read_fd = t->write_fd = -1;
            copy_text(t->status, sizeof(t->status),
                      "Shell exited. Close and reopen Terminal for a new session.");
            changed = true;
        }
    }

    if (changed) {
        invalidate_window(w);
    }
}

static void confirm_close(Window *w, const char *unused) {
    (void)unused;
    if (w && w->alive) {
        ((Terminal *)w->state)->allow_close = true;
        wm_close(w);
    }
}

bool terminal_can_close(Window *w) {
    Terminal *t = w->state;
    unsigned pid = 0;
    if (!t->allow_close && !t->ended && t->read_fd >= 0 &&
        !ioctl(t->read_fd, OS_PTY_GET_FOREGROUND, &pid) && pid && pid != (unsigned)t->pid) {
        dialog_confirm(w, "Close running program?",
                       "A terminal program is running. Closing this window will stop it. Save your "
                       "work before closing.",
                       confirm_close);
        return false;
    }

    return true;
}

bool terminal_close(Window *w) {
    Terminal *t = w->state;
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
