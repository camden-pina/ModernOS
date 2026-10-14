#include "terminal_screen.h"

static VtCell blank(VtScreen *t) {
    return (VtCell){' ', t->fg, t->bg, t->attrs, 0};
}

static void clear_cells(VtScreen *t, VtCell *p, int n) {
    while (n-- > 0) {
        *p++ = blank(t);
    }
}

void vt_init(VtScreen *t) {
    memset(t, 0, sizeof(*t));
    t->fg = t->bg = 16;
    t->wrap = t->cursor_visible = true;
    vt_resize(t, 80, 24);
}

void vt_resize(VtScreen *t, int cols, int rows) {
    int old = t->rows;
    t->cols = clampi(cols, 1, VT_COLS);
    t->rows = clampi(rows, 1, VT_ROWS);
    t->top = 0;
    t->bottom = t->rows - 1;
    t->x = clampi(t->x, 0, t->cols - 1);
    t->y = clampi(t->y, 0, t->rows - 1);
    for (int y = old; y < t->rows; y++) {
        clear_cells(t, t->cells[y], VT_COLS);
    }
}

static void scroll_region(VtScreen *t, int top, int bottom, int amount) {
    amount = clampi(amount, -(bottom - top + 1), bottom - top + 1);
    if (amount > 0) {
        memmove(t->cells[top], t->cells[top + amount],
                (bottom - top + 1 - amount) * sizeof(t->cells[0]));
        for (int y = bottom - amount + 1; y <= bottom; y++) {
            clear_cells(t, t->cells[y], VT_COLS);
        }
    } else if (amount < 0) {
        int n = -amount;
        memmove(t->cells[top + n], t->cells[top], (bottom - top + 1 - n) * sizeof(t->cells[0]));
        for (int y = top; y < top + n; y++) {
            clear_cells(t, t->cells[y], VT_COLS);
        }
    }
}

static void index_line(VtScreen *t, bool reverse) {
    t->pending_wrap = false;
    if (reverse) {
        if (t->y == t->top) {
            scroll_region(t, t->top, t->bottom, -1);
        } else {
            t->y = MAX(0, t->y - 1);
        }
    } else {
        if (t->y == t->bottom) {
            scroll_region(t, t->top, t->bottom, 1);
        } else {
            t->y = MIN(t->rows - 1, t->y + 1);
        }
    }
}

static int parameter(VtScreen *t, int index, int fallback) {
    return t->param[index] ? t->param[index] : fallback;
}

static void csi(VtScreen *t, unsigned final) {
    int n = parameter(t, 0, 1);
    switch (final) {
    case 'A':
        t->y = MAX(0, t->y - n);
        break;

    case 'B':
    case 'e':
        t->y = MIN(t->rows - 1, t->y + n);
        break;

    case 'C':
    case 'a':
        t->x = MIN(t->cols - 1, t->x + n);
        break;

    case 'D':
        t->x = MAX(0, t->x - n);
        break;

    case 'E':
        t->x = 0;
        t->y = MIN(t->rows - 1, t->y + n);
        break;

    case 'F':
        t->x = 0;
        t->y = MAX(0, t->y - n);
        break;

    case 'G':
    case '`':
        t->x = clampi(n - 1, 0, t->cols - 1);
        break;

    case 'd':
        t->y = clampi(n - 1, 0, t->rows - 1);
        break;

    case 'H':
    case 'f':
        t->y = clampi(n - 1, 0, t->rows - 1);
        t->x = clampi(parameter(t, 1, 1) - 1, 0, t->cols - 1);
        break;

    case 'J':
        if (t->param[0] >= 2) {
            for (int y = 0; y < t->rows; y++) {
                clear_cells(t, t->cells[y], VT_COLS);
            }
        } else if (t->param[0] == 1) {
            for (int y = 0; y < t->y; y++) {
                clear_cells(t, t->cells[y], VT_COLS);
            }
            clear_cells(t, t->cells[t->y], t->x + 1);
        } else {
            clear_cells(t, t->cells[t->y] + t->x, t->cols - t->x);
            for (int y = t->y + 1; y < t->rows; y++) {
                clear_cells(t, t->cells[y], VT_COLS);
            }
        }
        break;

    case 'K':
        if (t->param[0] == 2) {
            clear_cells(t, t->cells[t->y], t->cols);
        } else if (t->param[0] == 1) {
            clear_cells(t, t->cells[t->y], t->x + 1);
        } else {
            clear_cells(t, t->cells[t->y] + t->x, t->cols - t->x);
        }
        break;

    case 'X':
        clear_cells(t, t->cells[t->y] + t->x, MIN(n, t->cols - t->x));
        break;

    case 'P':
        n = MIN(n, t->cols - t->x);
        memmove(t->cells[t->y] + t->x, t->cells[t->y] + t->x + n,
                (t->cols - t->x - n) * sizeof(VtCell));
        clear_cells(t, t->cells[t->y] + t->cols - n, n);
        break;

    case '@':
        n = MIN(n, t->cols - t->x);
        memmove(t->cells[t->y] + t->x + n, t->cells[t->y] + t->x,
                (t->cols - t->x - n) * sizeof(VtCell));
        clear_cells(t, t->cells[t->y] + t->x, n);
        break;

    case 'L':
        if (t->y >= t->top && t->y <= t->bottom) {
            scroll_region(t, t->y, t->bottom, -n);
        }
        break;

    case 'M':
        if (t->y >= t->top && t->y <= t->bottom) {
            scroll_region(t, t->y, t->bottom, n);
        }
        break;

    case 'S':
        scroll_region(t, t->top, t->bottom, n);
        break;

    case 'T':
        scroll_region(t, t->top, t->bottom, -n);
        break;

    case 'r': {
        int top = clampi(n - 1, 0, t->rows - 1),
            bottom = clampi(parameter(t, 1, t->rows) - 1, 0, t->rows - 1);
        if (top < bottom) {
            t->top = top;
            t->bottom = bottom;
            t->x = t->y = 0;
        }
        break;
    }

    case 's':
        t->saved_x = t->x;
        t->saved_y = t->y;
        break;

    case 'u':
        t->x = clampi(t->saved_x, 0, t->cols - 1);
        t->y = clampi(t->saved_y, 0, t->rows - 1);
        break;

    case 'm':
        for (int i = 0; i <= t->parameter; i++) {
            int v = t->param[i];
            if (!v) {
                t->fg = t->bg = 16;
                t->attrs = 0;
            } else if (v == 1) {
                t->attrs |= 2;
            } else if (v == 22) {
                t->attrs &= ~2;
            } else if (v == 4) {
                t->attrs |= 4;
            } else if (v == 24) {
                t->attrs &= ~4;
            } else if (v == 7) {
                t->attrs |= 1;
            } else if (v == 27) {
                t->attrs &= ~1;
            } else if (v >= 30 && v <= 37) {
                t->fg = v - 30;
            } else if (v >= 40 && v <= 47) {
                t->bg = v - 40;
            } else if (v >= 90 && v <= 97) {
                t->fg = v - 90 + 8;
            } else if (v >= 100 && v <= 107) {
                t->bg = v - 100 + 8;
            } else if (v == 39) {
                t->fg = 16;
            } else if (v == 49) {
                t->bg = 16;
            }
        }
        break;

    case 'h':
    case 'l':
        if (t->private_sequence) {
            for (int i = 0; i <= t->parameter; i++) {
                bool enabled = final == 'h';
                int v = t->param[i];
                if (v == 1) {
                    t->application_keys = enabled;
                }
                if (v == 7) {
                    t->wrap = enabled;
                }
                if (v == 25) {
                    t->cursor_visible = enabled;
                }
                if ((v == 1049 || v == 47 || v == 1047) && enabled != t->alternate_screen) {
                    for (int y = 0; y < VT_ROWS; y++) {
                        for (int x = 0; x < VT_COLS; x++) {
                            VtCell c = t->cells[y][x];
                            t->cells[y][x] = t->alternate[y][x];
                            t->alternate[y][x] = c;
                        }
                    }

                    t->alternate_screen = enabled;
                    t->x = t->y = 0;
                    if (enabled) {
                        for (int y = 0; y < t->rows; y++) {
                            clear_cells(t, t->cells[y], VT_COLS);
                        }
                    }
                }
            }
        }
        break;
    default:
        break;
    }
    t->pending_wrap = false;
}

static void character(VtScreen *t, unsigned c) {
    if (t->pending_wrap && t->wrap) {
        t->x = 0;
        index_line(t, false);
    }
    if (c > 255) {
        c = c == 0x25c2 ? '<' : c == 0x25b8 ? '>' : '?';
    }
    t->cells[t->y][t->x] = (VtCell){c, t->fg, t->bg, t->attrs, 0};
    if (t->x < t->cols - 1) {
        t->x++;
    } else {
        t->pending_wrap = t->wrap;
    }
}

void vt_byte(VtScreen *t, unsigned c) {
    if (t->escape == 3) {
        if (c == 7) {
            t->escape = 0;
        } else if (c == 27) {
            t->escape = 4;
        }
        return;
    }

    if (t->escape == 4) {
        t->escape = c == '\\' ? 0 : 3;
        return;
    }
    if (t->escape == 5) {
        t->escape = 0;
        return;
    }
    if (t->escape == 1) {
        t->escape = 0;
        if (c == '[') {
            t->escape = 2;
            t->parameter = 0;
            t->private_sequence = false;
            memset(t->param, 0, sizeof(t->param));
        } else if (c == ']') {
            t->escape = 3;
        } else if (c == '(' || c == ')' || c == '*' || c == '+') {
            t->escape = 5;
        } else if (c == '7') {
            t->saved_x = t->x;
            t->saved_y = t->y;
        } else if (c == '8') {
            t->x = clampi(t->saved_x, 0, t->cols - 1);
            t->y = clampi(t->saved_y, 0, t->rows - 1);
        } else if (c == 'D') {
            index_line(t, false);
        } else if (c == 'M') {
            index_line(t, true);
        } else if (c == 'E') {
            t->x = 0;
            index_line(t, false);
        } else if (c == 'c') {
            int cols = t->cols, rows = t->rows;
            vt_init(t);
            vt_resize(t, cols, rows);
        }
        return;
    }

    if (t->escape == 2) {
        if (c >= '0' && c <= '9') {
            t->param[t->parameter] = MIN(10000, t->param[t->parameter] * 10 + c - '0');
        } else if (c == ';') {
            if (t->parameter < 15) {
                t->parameter++;
            }
        } else if (c == '?') {
            t->private_sequence = true;
        } else if (c >= '@' && c <= '~') {
            csi(t, c);
            t->escape = 0;
        } else if (c == 27) {
            t->escape = 1;
        }
        return;
    }

    if (t->utf_left) {
        if ((c & 0xc0) == 0x80) {
            t->utf = (t->utf << 6) | (c & 63);
            if (!--t->utf_left) {
                character(t, t->utf);
            }
            return;
        }

        t->utf_left = 0;
        character(t, '?');
    }

    if (c == 27) {
        t->escape = 1;
        return;
    }
    if (c == '\n') {
        index_line(t, false);
        return;
    }
    if (c == '\r') {
        t->x = 0;
        t->pending_wrap = false;
        return;
    }
    if (c == 8) {
        t->x = MAX(0, t->x - 1);
        t->pending_wrap = false;
        return;
    }
    if (c == '\t') {
        t->x = MIN(t->cols - 1, (t->x + 8) & ~7);
        return;
    }
    if (c < 32 || c == 127) {
        return;
    }
    if (c >= 0xc2 && c <= 0xdf) {
        t->utf = c & 31;
        t->utf_left = 1;
        return;
    }
    if (c >= 0xe0 && c <= 0xef) {
        t->utf = c & 15;
        t->utf_left = 2;
        return;
    }
    if (c >= 0xf0 && c <= 0xf4) {
        t->utf = c & 7;
        t->utf_left = 3;
        return;
    }
    character(t, c);
}

static uint32_t color(unsigned i, bool background, bool bold) {
    static const uint32_t palette[] = {0x202B32, 0xC86868, 0x70A88C, 0xC3A674, 0x709ABF, 0xAD90BC,
                                       0x72ABAE, 0xCEDADC, 0x6C808A, 0xF09A95, 0xA0D5B1, 0xEFD699,
                                       0x9BC7EE, 0xD7B4EB, 0xA3D9DB, 0xF0F6F7};
    if (i == 16) {
        return background ? theme_color(0xFAFCFB) : THEME_TEXT;
    }
    return palette[MIN(i + (bold && i < 8 ? 8 : 0), 15)];
}

void vt_draw(VtScreen *t, Rect r, bool focused) {
    Rect saved = desk.clip;
    clip_set(intersect(saved, r));
    fill(r, theme_color(0xFAFCFB));
    int cw = MAX(1, text_width("M", FONT_MONO)), lh = S(17);
    for (int y = 0; y < t->rows; y++) {
        if (empty(intersect(desk.clip, (Rect){r.x, r.y + y * lh, r.w, lh}))) {
            continue;
        }
        for (int x = 0; x < t->cols; x++) {
            VtCell c = t->cells[y][x];
            uint32_t fg = color(c.fg, false, c.attrs & 2), bg = color(c.bg, true, false);
            if (c.attrs & 1) {
                uint32_t swap = fg;
                fg = bg;
                bg = swap;
            }
            Rect cell = {r.x + x * cw, r.y + y * lh, cw, lh};
            if (bg != theme_color(0xFAFCFB)) {
                fill(cell, bg);
            }
            char text[3] = {0};
            if (c.ch >= 128 && c.ch < 256) {
                text[0] = 0xc0 | (c.ch >> 6);
                text[1] = 0x80 | (c.ch & 63);
            } else {
                text[0] = c.ch ? c.ch : ' ';
            }
            label(cell.x, cell.y + S(1), text, FONT_MONO, fg, cw + 1);
            if (c.attrs & 4) {
                fill((Rect){cell.x, cell.y + lh - 2, cw, 1}, fg);
            }
        }
    }

    if (focused && t->cursor_visible) {
        border((Rect){r.x + t->x * cw, r.y + t->y * lh, cw, lh}, 0, desk.accent);
    }
    desk.clip = saved;
}

int vt_key(VtScreen *t, Key k, char *out, size_t capacity) {
    if (!k.down || capacity < 16) {
        return 0;
    }
    int n = 0;
    const char *sequence = NULL;
    if (k.code == KEY_ENTER) {
        out[n++] = '\r';
    } else if (k.code == KEY_BACKSPACE) {
        out[n++] = 127;
    } else if (k.code == KEY_TAB) {
        out[n++] = '\t';
    } else if (k.code == KEY_ESCAPE) {
        out[n++] = 27;
    } else if (k.code >= KEY_RIGHT && k.code <= KEY_UP) {
        out[n++] = 27;
        out[n++] = t->application_keys ? 'O' : '[';
        out[n++] = k.code == KEY_RIGHT  ? 'C'
                   : k.code == KEY_LEFT ? 'D'
                   : k.code == KEY_DOWN ? 'B'
                                        : 'A';
    } else if (k.code == KEY_HOME) {
        sequence = "\033[1~";
    } else if (k.code == KEY_END) {
        sequence = "\033[4~";
    } else if (k.code == KEY_DELETE) {
        sequence = "\033[3~";
    } else if (k.code == KEY_PAGEUP) {
        sequence = "\033[5~";
    } else if (k.code == KEY_PAGEDOWN) {
        sequence = "\033[6~";
    } else if (k.code >= 0x3a && k.code <= 0x45) {
        static const char *function_keys[] = {"\033OP",   "\033OQ",   "\033OR",   "\033OS",
                                              "\033[15~", "\033[17~", "\033[18~", "\033[19~",
                                              "\033[20~", "\033[21~", "\033[23~", "\033[24~"};
        sequence = function_keys[k.code - 0x3a];
    } else if (k.ascii) {
        unsigned c = k.ascii;
        if (k.mods & MOD_ALT) {
            out[n++] = 27;
        }
        if (k.mods & MOD_CTRL) {
            if (c >= 'a' && c <= 'z') {
                c -= 32;
            }
            if (c >= '@' && c <= '_') {
                c &= 31;
            } else if (c == ' ') {
                c = 0;
            }
        }

        out[n++] = c;
    }

    if (sequence) {
        copy_text(out, capacity, sequence);
        n = strlen(out);
    }
    return n;
}
