#include "terminal_preferences.h"
typedef struct {
  TerminalTag tag;
  int palette, foreground, background, opacity, size, shell, ansi;
  int page, focus, drag, scroll;
  unsigned profile;
  bool changed, was_current;
} Preferences;
bool terminal_is_preferences(Window *w) {
  return w && w->app == APP_TERMINAL && w->state &&
         ((TerminalTag *)w->state)->preferences;
}
static bool current(Preferences *p) { return p->profile == profiles_active(); }
static void load(Preferences *p) {
  p->palette = desk.terminal_palette;
  p->foreground = desk.terminal_foreground;
  p->background = desk.terminal_background;
  p->opacity = desk.terminal_opacity;
  p->size = desk.terminal_text_size;
  p->shell = desk.terminal_shell;
  p->ansi = desk.terminal_ansi;
  p->profile = profiles_active();
  p->changed = false;
  p->was_current = true;
  p->drag = 0;
}
int terminal_prefs_init(Window *w) {
  Preferences *p = allocate(sizeof(*p));
  if (!p)
    return -E_NOMEM;
  memset(p, 0, sizeof(*p));
  p->tag.preferences = 1;
  load(p);
  w->state = p;
  copy_text(w->title, sizeof(w->title), "Terminal Preferences");
  return 0;
}
void terminal_preferences_open(Window *w) {
  if (terminal_is_preferences(w)) {
    wm_focus(w);
    return;
  }
  if (!w || !w->alive || w->app != APP_TERMINAL)
    return;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *c = &desk.windows[i];
    if (c->alive && c->owner == w && c->owner_id == w->id &&
        terminal_is_preferences(c)) {
      wm_focus(c);
      trace("TERMINAL_PREF_FOCUS", c, "existing child");
      return;
    }
  }
  Window *c = wm_open(APP_TERMINAL, "@preferences");
  if (!c)
    return;
  damage_window(c);
  c->owner = w;
  c->owner_id = w->id;
  c->workspace = w->workspace;
  c->box.w = MIN(S(620), desk.width - S(24));
  c->box.h = MIN(S(492), desk.height - S(DESKTOP_TOP) - desktop_bottom());
  c->box.x =
      clampi(w->box.x + (w->box.w - c->box.w) / 2, 0, desk.width - c->box.w);
  c->box.y = clampi(w->box.y + S(24), S(DESKTOP_TOP),
                    desk.height - desktop_bottom() - c->box.h);
  c->restore = c->box;
  invalidate_window(c);
  wm_trace_geometry(c);
  trace("TERMINAL_PREF_OPEN", c, "owned child, draft until Apply");
}
static Rect control(Window *w, int id) {
  Rect b = window_body(w);
  Preferences *p = w->state;
  if (id >= 10 && id < 70)
    b.y -= p->scroll;
  int x = b.x + S(18), width = b.w - S(36);
  if (id >= 0 && id < 3)
    return (Rect){x + id * ((width - S(12)) / 3 + S(6)), b.y + S(12),
                  (width - S(12)) / 3, S(28)};
  int group = id / 10, index = id % 10;
  if (group >= 1 && group <= 3) {
    int n = group == 1 ? 5 : 7, gap = S(5), cw = (width - gap * (n - 1)) / n;
    return (Rect){x + index * (cw + gap), b.y + S(78 + (group - 1) * 64), cw,
                  S(27)};
  }
  if (id == 40 || id == 41)
    return (Rect){x + S(8), b.y + S(id == 40 ? 290 : 100), width - S(80),
                  S(26)};
  if (id == 50 || id == 51)
    return (Rect){x + (id - 50) * (width + S(8)) / 2, b.y + S(88),
                  (width - S(8)) / 2, S(30)};
  if (id == 60 || id == 61)
    return (Rect){x + (id - 60) * S(95), b.y + S(198), S(86), S(28)};
  if (id >= 70 && id <= 72)
    return (Rect){b.x + b.w - S(282) + (id - 70) * S(88), b.y + b.h - S(44),
                  S(80), S(28)};
  return (Rect){0};
}
static Rect content_view(Window *w) {
  Rect b = window_body(w);
  return (Rect){b.x, b.y + S(48), b.w, MAX(0, b.h - S(124))};
}
static int max_scroll(Window *w) {
  Preferences *p = w->state;
  return MAX(0, S(p->page == 2 ? 280 : 388) - (window_body(w).h - S(76)));
}
static int ids(Window *w, int *list) {
  Preferences *p = w->state;
  int n = 0;
  for (int i = 0; i < 3; i++)
    list[n++] = i;
  if (current(p)) {
    if (p->page == 0) {
      for (int g = 1; g <= 3; g++)
        for (int c = 0; c < (g == 1 ? 5 : 7); c++)
          list[n++] = g * 10 + c;
      list[n++] = 40;
    }
    if (p->page == 1) {
      list[n++] = 41;
      list[n++] = 60;
      list[n++] = 61;
    }
    if (p->page == 2) {
      list[n++] = 50;
      list[n++] = 51;
    }
    list[n++] = 70;
  }
  list[n++] = 71;
  if (current(p))
    list[n++] = 72;
  return n;
}
static bool apply(Window *w) {
  Preferences *p = w->state;
  if (!current(p))
    return false;
  desk.terminal_palette = p->palette;
  desk.terminal_foreground = p->foreground;
  desk.terminal_background = p->background;
  desk.terminal_opacity = p->opacity;
  desk.terminal_text_size = p->size;
  desk.terminal_shell = p->shell;
  desk.terminal_ansi = p->ansi;
  terminal_preferences_changed();
  int error = preferences_save(w);
  p->changed = error != 0;
  invalidate_window(w);
  trace("TERMINAL_PREF_APPLY", w,
        error ? "session active, persistence failed" : "profile saved");
  return !error;
}
static void activate(Window *w, int id) {
  Preferences *p = w->state;
  p->focus = id;
  if (id < 3) {
    p->page = id;
    p->scroll = 0;
    p->drag = 0;
    invalidate_window(w);
    return;
  }
  if (id == 71) {
    wm_close(w);
    return;
  }
  if (!current(p))
    return;
  if (id == 70 || id == 72) {
    if (!p->changed) {if(id==72)wm_close(w);return;}
    if (apply(w) && id == 72)
      wm_close(w);
    return;
  }
  if (id >= 10 && id < 15)
    p->palette = id - 10;
  if (id >= 20 && id < 27)
    p->foreground = id - 20;
  if (id >= 30 && id < 37)
    p->background = id - 30;
  if (id >= 50 && id < 52)
    p->shell = id - 50;
  if (id >= 60 && id < 62)
    p->ansi = id - 60;
  p->changed = true;
  invalidate_window(w);
}
static void adjust(Window *w, int id, int delta) {
  Preferences *p = w->state;
  if (!current(p))
    return;
  if (id == 40)
    p->opacity = clampi(p->opacity + delta, 40, 100);
  else
    p->size = clampi(p->size + 2 * delta, 9, 19);
  p->changed = true;
  invalidate_window(w);
}
static void slide(Window *w, int id, int x) {
  Preferences *p = w->state;
  Rect r = control(w, id);
  int lo = id == 40 ? 40 : 9, hi = id == 40 ? 100 : 19, step = id == 40 ? 1 : 2;
  int slots = (hi - lo) / step,
      value = (clampi(x - r.x, 0, r.w - 1) * slots + (r.w - 1) / 2) /
              MAX(1, r.w - 1);
  if (id == 40)
    p->opacity = lo + value * step;
  else
    p->size = lo + value * step;
  p->focus = id;
  p->changed = true;
  invalidate_window(w);
}
static void slider_draw(Window *w, int id, int value, int lo, int hi) {
  Preferences *p = w->state;
  Rect r = control(w, id);
  int cy = r.y + r.h / 2, xx = r.x + (r.w - 1) * (value - lo) / (hi - lo);
  rounded((Rect){r.x, cy - S(2), r.w, S(4)}, S(2), THEME_DIVIDER);
  rounded((Rect){r.x, cy - S(2), MAX(1, xx - r.x), S(4)}, S(2), desk.accent);
  rounded((Rect){xx - S(6), cy - S(8), S(13), S(17)}, S(6),
          current(p) ? desk.accent : THEME_MUTED_TEXT);
  if (p->focus == id)
    border((Rect){r.x - S(4), r.y - S(2), r.w + S(8), r.h + S(4)}, S(4),
           desk.accent);
  char text[24];
  format_uint(text, value);
  append_text(text, sizeof(text), id == 40 ? "%" : " px");
  label(r.x + r.w + S(12), r.y + S(7), text, FONT_SMALL, THEME_TEXT, S(60));
}
void terminal_prefs_draw(Window *w) {
  Preferences *p = w->state;
  Rect b = window_body(w);
  fill(b, THEME_WINDOW);
  const char *tabs[] = {"Appearance", "Text", "Shell"};
  for (int i = 0; i < 3; i++)
    button(control(w, i), tabs[i], I_COUNT, p->page == i, true, p->focus == i);
  Rect original = b, saved = desk.clip;
  p->scroll = clampi(p->scroll, 0, max_scroll(w));
  clip_set(intersect(saved, content_view(w)));
  b.y -= p->scroll;
  if (p->page == 0) {
    const char *heads[] = {"Palette", "Text color", "Background color"};
    const char *palettes[] = {"System", "Midnight", "Paper", "Forest", "Ember"};
    const char *colors[] = {"Palette", "Ink",   "Ivory", "Slate",
                            "Mint",    "Amber", "Blue"};
    for (int g = 1; g <= 3; g++) {
      label(b.x + S(18), b.y + S(57 + (g - 1) * 64), heads[g - 1], FONT_BOLD,
            THEME_TEXT, b.w - S(36));
      for (int i = 0; i < (g == 1 ? 5 : 7); i++)
        button(control(w, g * 10 + i), g == 1 ? palettes[i] : colors[i],
               I_COUNT,
               i == (g == 1   ? p->palette
                     : g == 2 ? p->foreground
                              : p->background),
               current(p), p->focus == g * 10 + i);
    }
    label(b.x + S(18), b.y + S(263),
          "Background opacity (lower values reveal the blurred desktop)",
          FONT_NORMAL, THEME_TEXT, b.w - S(36));
    slider_draw(w, 40, p->opacity, 40, 100);
  } else if (p->page == 1) {
    label(b.x + S(18), b.y + S(66), "Text size", FONT_BOLD, THEME_TEXT,
          b.w - S(36));
    slider_draw(w, 41, p->size, 9, 19);
    label(b.x + S(18), b.y + S(165), "Allow application ANSI colors", FONT_BOLD,
          THEME_TEXT, b.w - S(36));
    button(control(w, 60), "Off", I_COUNT, !p->ansi, current(p),
           p->focus == 60);
    button(control(w, 61), "On", I_COUNT, p->ansi, current(p), p->focus == 61);
  } else {
    label(b.x + S(18), b.y + S(60), "Shell for new Terminal windows", FONT_BOLD,
          THEME_TEXT, b.w - S(36));
    button(control(w, 50), "Coast shell", I_COUNT, !p->shell, current(p),
           p->focus == 50);
    button(control(w, 51), "GNU Bash", I_COUNT, p->shell, current(p),
           p->focus == 51);
    label(b.x + S(18), b.y + S(146),
          "Existing sessions keep their running shell.", FONT_NORMAL,
          THEME_SECONDARY_TEXT, b.w - S(36));
    label(b.x + S(18), b.y + S(178),
          "Coast: inline history, completion and editing; 511-byte command "
          "limit.",
          FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(36));
    label(b.x + S(18), b.y + S(205),
          "Bash compatibility remains experimental; job control is not "
          "guaranteed.",
          FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(36));
    label(b.x + S(18), b.y + S(232),
          "zsh, fish and Oh My Zsh are not installed.", FONT_SMALL,
          THEME_SECONDARY_TEXT, b.w - S(36));
  }
  if (p->page != 2) {
    Rect preview = {b.x + S(18), b.y + S(328), b.w - S(36), S(43)};
    rounded(preview, S(5),
            terminal_color_choice(p->palette, p->background, true));
    label(preview.x + S(10), preview.y + S(10), "os:/home/user$ echo hello",
          terminal_font_size(p->size),
          terminal_color_choice(p->palette, p->foreground, false),
          preview.w - S(20));
  }
  b = original;
  desk.clip = saved;
  fill((Rect){b.x, b.y + b.h - S(76), b.w, 1}, THEME_DIVIDER);
  label(
      b.x + S(18), b.y + b.h - S(66),
      !current(p)
          ? "Active profile changed. Close and reopen to edit its preferences."
      : p->changed ? "Unsaved draft. Apply updates this profile; Cancel "
                     "discards unapplied changes."
                   : "Preferences for the current profile. Changes apply to "
                     "all Terminal windows.",
      FONT_SMALL, !current(p) ? desk.accent : THEME_SECONDARY_TEXT,
      b.w - S(36));
  button(control(w, 70), "Apply", I_COUNT, false, current(p) && p->changed,
         p->focus == 70);
  button(control(w, 71), "Cancel", I_COUNT, false, true, p->focus == 71);
  button(control(w, 72), "Done", I_CHECK, true, current(p), p->focus == 72);
}
bool terminal_prefs_mouse(Window *w, int x, int y, unsigned pressed,
                          int wheel) {
  Preferences *p = w->state;
  if (p->drag) {
    if (desk.buttons & 1)
      slide(w, p->drag, x);
    else
      p->drag = 0;
    return true;
  }
  int list[40], n = ids(w, list);
  for (int i = 0; i < n; i++)
    if (contains(control(w, list[i]), x, y) &&
        (list[i] < 10 || list[i] >= 70 || contains(content_view(w), x, y))) {
      int id = list[i];
      if (wheel && (id == 40 || id == 41)) {
        adjust(w, id, wheel);
        return true;
      }
      if (pressed & 1) {
        if (id == 40 || id == 41) {
          p->drag = id;
          slide(w, id, x);
        } else
          activate(w, id);
        return true;
      }
    }
  if (wheel && contains(content_view(w), x, y)) {
    p->scroll = clampi(p->scroll - wheel * S(30), 0, max_scroll(w));
    invalidate_window(w);
  }
  return true;
}
void terminal_prefs_key(Window *w, Key k) {
  if (!k.down)
    return;
  Preferences *p = w->state;
  if (k.code == KEY_ESCAPE) {
    wm_close(w);
    return;
  }
  if (k.code == KEY_TAB) {
    int list[40], n = ids(w, list), at = -1;
    for (int i = 0; i < n; i++)
      if (list[i] == p->focus)
        at = i;
    p->focus = list[(at + (k.mods & MOD_SHIFT ? -1 : 1) + n) % n];
    if (p->focus >= 10 && p->focus < 70) {
      Rect r = control(w, p->focus), view = content_view(w);
      if (r.y < view.y)
        p->scroll -= view.y - r.y;
      else if (r.y + r.h > view.y + view.h)
        p->scroll += r.y + r.h - view.y - view.h;
      p->scroll = clampi(p->scroll, 0, max_scroll(w));
    }
    invalidate_window(w);
    return;
  }
  if ((p->focus == 40 || p->focus == 41) &&
      (k.code == KEY_LEFT || k.code == KEY_RIGHT || k.code == KEY_UP ||
       k.code == KEY_DOWN))
    adjust(w, p->focus, (k.code == KEY_LEFT || k.code == KEY_DOWN) ? -1 : 1);
  else if ((p->focus == 40 || p->focus == 41) &&
           (k.code == KEY_HOME || k.code == KEY_END))
    slide(w, p->focus, k.code == KEY_HOME ? 0 : desk.width);
  else if (k.code == KEY_ENTER || k.ascii == ' ')
    activate(w, p->focus);
}
void terminal_prefs_tick(Window *w) {
  Preferences *p = w->state;
  if (p->drag && !(desk.buttons & 1))
    p->drag = 0;
  if (p->was_current != current(p)) {
    p->was_current = current(p);
    p->drag = 0;
    invalidate_window(w);
  }
}
bool terminal_prefs_close(Window *w) {
  deallocate(w->state, sizeof(Preferences));
  w->state = NULL;
  return true;
}
static void command(Window *w, int id) { activate(w, id); }
void terminal_prefs_menu(Window *w, int group, int x, int y) {
  Preferences *p = w->state;
  if (group == MENU_APP || group == MENU_FILE) {
    MenuItem items[] = {
        {"Apply preferences", I_CHECK, 70, current(p) && p->changed},
        {"Cancel and close\tEscape", I_CLOSE, 71, true}};
    menu_open(w, x, y, items, 2, command);
  }
}
Rect terminal_prefs_hover(Window *w, int x, int y) {
  int list[40], n = ids(w, list);
  for (int i = 0; i < n; i++) {
    Rect r = control(w, list[i]);
    if (list[i] >= 10 && list[i] < 70)
      r = intersect(r, content_view(w));
    if (contains(r, x, y))
      return r;
  }
  return (Rect){0};
}
