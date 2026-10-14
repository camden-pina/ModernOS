#include "desktop.h"

void menu_close(void) {
    if (desk.menu.open) {
        invalidate((Rect){desk.menu.box.x - S(10), desk.menu.box.y - S(10), desk.menu.box.w + S(20),
                          desk.menu.box.h + S(20)});
        desk.menu.open = false;
    }
}

void menu_open(Window *w, int x, int y, const MenuItem *items, int count, MenuAction action) {
    menu_close();
    launcher_close();
    Menu *m = &desk.menu;
    m->open = true;
    m->owner = w;
    m->action = action;
    m->count = MIN(count, MAX_MENU_ITEMS);
    m->hover = -1;
    m->focus = 0;
    m->first = 0;
    m->scroll = 0;
    smooth_jump(&m->animation, 0, now_ms());
    memcpy(m->items, items, m->count * sizeof(*items));
    int width = S(202);
    for (int i = 0; i < m->count; i++) {
        width = MAX(width, text_width(m->items[i].label, FONT_BODY) + S(60));
    }
    width = MIN(width, desk.width - S(10));
    m->visible = MIN(m->count, MAX(1, (desk.height - S(64)) / S(29)));
    int height = m->visible * S(29) + S(12);
    m->box = (Rect){clampi(x, S(5), desk.width - width - S(5)),
                    clampi(y, S(5), desk.height - S(47) - height), width, height};
    invalidate((Rect){m->box.x - S(8), m->box.y - S(8), width + S(16), height + S(16)});
}

void menu_draw(void) {
    Menu *m = &desk.menu;
    Rect b = m->box;
    blend((Rect){b.x + S(3), b.y + S(5), b.w, b.h}, THEME_SHADOW, 25);
    rounded(b, S(8), THEME_WINDOW);
    border(b, S(8), THEME_WINDOW_BORDER);
    Rect saved = desk.clip;
    clip_set(intersect(saved, (Rect){b.x + S(5), b.y + S(6), b.w - S(10), b.h - S(12)}));
    for (int i = m->first; i < MIN(m->count, m->first + m->visible + 1); i++) {
        Rect r = {b.x + S(5), b.y + S(6) + i * S(29) - m->scroll, b.w - S(10), S(29)};
        bool hover = contains(r, desk.mouse_x, desk.mouse_y), focus = i == m->focus;
        if (hover || focus) {
            rounded(r, S(4), focus ? theme_color(0xE9F2ED) : theme_color(0xF1F4F2));
        }
        uint32_t color = m->items[i].enabled ? theme_color(0x41525A) : theme_color(0xA9B3B4);
        icon(r.x + S(7), r.y + S(7), m->items[i].icon, S(15), color);
        label(r.x + S(31), r.y + S(7), m->items[i].label, FONT_BODY, color, r.w - S(38));
    }

    clip_set(saved);
    scrollbar((Rect){b.x, b.y + S(6), b.w, b.h - S(12)}, m->count * S(29), m->scroll);
}

bool menu_mouse(int x, int y, unsigned pressed, int wheel) {
    Menu *m = &desk.menu;
    if (!m->open) {
        return false;
    }
    if (!contains(m->box, x, y)) {
        if (pressed) {
            menu_close();
        }
        return pressed != 0;
    }

    if (wheel) {
        smooth_by(&m->animation, m->scroll, -(int64_t)wheel * S(29),
                  (m->count - m->visible) * S(29), now_ms());
        return true;
    }
    if (pressed & 1) {
        int index = (y - m->box.y - S(6) + m->scroll) / S(29);
        if (index >= 0 && index < m->count && y >= m->box.y + S(6) &&
            y < m->box.y + m->box.h - S(6) && m->items[index].enabled) {
            int action = m->items[index].action;
            Window *w = m->owner;
            MenuAction callback = m->action;
            menu_close();
            if (callback) {
                callback(w, action);
            }
        }
    }

    return true;
}

bool menu_key(Key k) {
    Menu *m = &desk.menu;
    if (!m->open || !k.down) {
        return false;
    }
    if (k.code == KEY_ESCAPE) {
        menu_close();
    } else if (k.code == KEY_DOWN || k.code == KEY_UP || k.code == KEY_TAB) {
        int direction = k.code == KEY_UP || (k.code == KEY_TAB && (k.mods & MOD_SHIFT)) ? -1 : 1;
        for (int tries = 0; tries < m->count; tries++) {
            m->focus = (m->focus + direction + m->count) % m->count;
            if (m->items[m->focus].enabled) {
                break;
            }
        }

        if (m->focus < m->first) {
            m->first = m->focus;
        } else if (m->focus >= m->first + m->visible) {
            m->first = m->focus - m->visible + 1;
        }
        m->scroll = m->first * S(29);
        smooth_jump(&m->animation, m->scroll, now_ms());
        invalidate(m->box);
    } else if (k.code == KEY_ENTER && m->items[m->focus].enabled) {
        MenuAction cb = m->action;
        Window *w = m->owner;
        int action = m->items[m->focus].action;
        menu_close();
        if (cb) {
            cb(w, action);
        }
    }

    return true;
}

void menu_tick(void) {
    Menu *m = &desk.menu;
    if (!m->open)
        return;
    int position = smooth_tick(&m->animation, m->scroll, (m->count - m->visible) * S(29), now_ms());
    if (position != m->scroll) {
        m->scroll = position;
        m->first = position / S(29);
        invalidate(m->box);
    }
}

static void dialog_setup(Window *w, const char *title, const char *message) {
    menu_close();
    launcher_close();
    Dialog *d = &desk.dialog;
    if (d->open) {
        invalidate(d->box);
    }
    memset(d, 0, sizeof(*d));
    d->open = true;
    d->focus = 2;
    d->owner = w;
    int width = MIN(S(500), desk.width - S(30)), height = S(228);
    d->box = (Rect){(desk.width - width) / 2, (desk.height - S(42) - height) / 2, width, height};
    copy_text(d->title, sizeof(d->title), title);
    copy_text(d->message, sizeof(d->message), message);
    invalidate((Rect){d->box.x - S(8), d->box.y - S(8), width + S(16), height + S(16)});
}

void dialog_notice(Window *w, const char *title, const char *message) {
    dialog_setup(w, title, message);
}

void dialog_error(Window *w, const char *action, long error) {
    char b[640];
    copy_text(b, sizeof(b), error_text(error));
    if (error == -E_ROFS) {
        append_text(b, sizeof(b), ". This filesystem is mounted read-only.");
    } else if (error == -E_EXIST) {
        append_text(b, sizeof(b), ". Choose another name; existing data is preserved.");
    } else if (error == -E_XDEV) {
        append_text(b, sizeof(b),
                    ". Folders can be cut within one filesystem. Across mounts, copy the folder "
                    "first, then move the original to Trash after verifying the copy.");
    } else if (error == -E_BUSY) {
        append_text(b, sizeof(b), ". Close programs using this file or directory, then try again.");
    } else if (error == -E_NOTSUP) {
        append_text(b, sizeof(b),
                    ". The filesystem or application backend does not support this operation.");
    }
    dialog_notice(w, action, b);
    trace("ERROR", w, action);
}

void dialog_input(Window *w, const char *title, const char *message, const char *value,
                  DialogAction action) {
    dialog_setup(w, title, message);
    desk.dialog.input = true;
    desk.dialog.focus = 0;
    desk.dialog.action = action;
    copy_text(desk.dialog.value, sizeof(desk.dialog.value), value);
    desk.dialog.cursor = strlen(desk.dialog.value);
    desk.dialog.replace = value[0] != 0;
}

void dialog_confirm(Window *w, const char *title, const char *message, DialogAction action) {
    dialog_setup(w, title, message);
    desk.dialog.action = action;
}

static void dialog_accept(void) {
    Dialog snapshot = desk.dialog;
    desk.dialog.open = false;
    invalidate((Rect){snapshot.box.x - S(8), snapshot.box.y - S(8), snapshot.box.w + S(16),
                      snapshot.box.h + S(16)});
    if (snapshot.action) {
        snapshot.action(snapshot.owner, snapshot.value);
    }
}

static void dialog_cancel(void) {
    desk.quit_pending = false;
    Rect b = desk.dialog.box;
    desk.dialog.open = false;
    invalidate((Rect){b.x - S(8), b.y - S(8), b.w + S(16), b.h + S(16)});
}

void dialog_draw(void) {
    Dialog *d = &desk.dialog;
    Rect b = d->box;
    blend((Rect){b.x + S(4), b.y + S(6), b.w, b.h}, THEME_SHADOW, 45);
    rounded(b, S(9), THEME_WINDOW);
    border(b, S(9), THEME_WINDOW_BORDER);
    label(b.x + S(22), b.y + S(18), d->title, FONT_BOLD, THEME_PRIMARY_TEXT, b.w - S(44));
    /* Wrap messages by measured glyph width rather than fixed character cells. */
    const char *p = d->message;
    int yy = b.y + S(48);
    for (int row = 0; *p && row < 5; row++) {
        char line_text[128];
        int used = 0, last_space = -1;
        while (*p && *p != '\n' && used < 126) {
            line_text[used++] = *p++;
            line_text[used] = 0;
            if (line_text[used - 1] == ' ') {
                last_space = used - 1;
            }
            if (text_width(line_text, FONT_BODY) > b.w - S(44)) {
                int keep = last_space > 0 ? last_space : used - 1;
                p -= used - keep;
                used = keep;
                while (*p == ' ') {
                    p++;
                }
                break;
            }
        }

        if (*p == '\n') {
            p++;
        }
        line_text[used] = 0;
        label(b.x + S(22), yy, line_text, FONT_BODY, theme_color(0x617176), b.w - S(44));
        yy += S(17);
    }

    if (d->input) {
        input_draw((Rect){b.x + S(22), b.y + b.h - S(83), b.w - S(44), S(28)}, d->value,
                   "Name or path", d->replace ? -1 : d->cursor, d->focus == 0);
    }
    Rect ok = {b.x + b.w - S(102), b.y + b.h - S(42), S(80), S(27)};
    button(ok, d->action ? (d->input ? "Apply" : "Confirm") : "OK", I_COUNT, true, true,
           d->focus == 2);
    if (d->action) {
        button((Rect){ok.x - S(92), ok.y, S(80), ok.h}, "Cancel", I_COUNT, false, true,
               d->focus == 1);
    }
}

Rect dialog_hover(int x, int y) {
    Dialog *d = &desk.dialog;
    Rect b = d->box;
    Rect ok = {b.x + b.w - S(102), b.y + b.h - S(42), S(80), S(27)};
    if (contains(ok, x, y)) {
        return ok;
    }
    Rect cancel = {ok.x - S(92), ok.y, S(80), ok.h};
    return d->action && contains(cancel, x, y) ? cancel : (Rect){0};
}

bool dialog_mouse(int x, int y, unsigned pressed) {
    if (!desk.dialog.open) {
        return false;
    }
    Dialog *d = &desk.dialog;
    Rect b = d->box;
    if (pressed & 1) {
        Rect ok = {b.x + b.w - S(102), b.y + b.h - S(42), S(80), S(27)};
        if (contains(ok, x, y)) {
            dialog_accept();
        } else if (d->action && contains((Rect){ok.x - S(92), ok.y, S(80), ok.h}, x, y)) {
            dialog_cancel();
        } else if (d->input &&
                   contains((Rect){b.x + S(22), b.y + b.h - S(83), b.w - S(44), S(28)}, x, y)) {
            d->cursor = strlen(d->value);
            d->replace = false;
            d->focus = 0;
            invalidate(b);
        }
    }

    return true;
}

void dialog_key(Key k) {
    Dialog *d = &desk.dialog;
    if (!k.down) {
        return;
    }
    if (k.code == KEY_ESCAPE) {
        dialog_cancel();
        return;
    }
    if (k.code == KEY_ENTER) {
        if (d->focus == 1) {
            dialog_cancel();
        } else {
            dialog_accept();
        }
        return;
    }

    if (k.code == KEY_TAB) {
        int step = k.mods & MOD_SHIFT ? 2 : 1;
        do {
            d->focus = (d->focus + step) % 3;
        } while ((d->focus == 0 && !d->input) || (d->focus == 1 && !d->action));
        invalidate(d->box);
        return;
    }

    if (d->input && d->focus == 0) {
        if ((k.mods & MOD_CTRL) && (k.ascii == 'a' || k.ascii == 'A')) {
            d->replace = true;
            invalidate(d->box);
            return;
        }
        if (d->replace && ((k.ascii >= 32 && !(k.mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))) ||
                           k.code == KEY_BACKSPACE || k.code == KEY_DELETE)) {
            d->value[0] = 0;
            d->cursor = 0;
            d->replace = false;
        }

        if (k.code == KEY_LEFT || k.code == KEY_RIGHT || k.code == KEY_HOME || k.code == KEY_END) {
            d->replace = false;
        }
        if (input_key(d->value, sizeof(d->value), &d->cursor, k)) {
            invalidate(d->box);
        }
    }
}
