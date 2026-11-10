#include "desktop.h"
#include "text_input_model.h"

void menu_close(void) {
  if (desk.menu.open) {
    transition_begin(desk.menu.box);
    invalidate((Rect){desk.menu.box.x - S(10), desk.menu.box.y - S(10),
                      desk.menu.box.w + S(20), desk.menu.box.h + S(20)});
    desk.menu.open = false;
    invalidate((Rect){0, 0, desk.width, S(DESKTOP_TOP)});
    if (desk.menu.origin_group && desk.menu.owner && desk.menu.owner->alive)
      invalidate_window(desk.menu.owner);
  }
}

void menu_open(Window *w, int x, int y, const MenuItem *items, int count,
               MenuAction action) {
  menu_close();
  launcher_close();
  Menu *m = &desk.menu;
  m->palette = m->color_row = m->color_keyboard = false;
  m->open = true;
  m->owner = w;
  m->action = action;
  m->count = MIN(count, MAX_MENU_ITEMS);
  m->hover = -1;
  m->focus = 0;
  m->origin_group = 0;
  m->first = 0;
  m->scroll = 0;
  smooth_jump(&m->animation, 0, now_ms());
  memcpy(m->items, items, m->count * sizeof(*items));
  while (m->focus + 1 < m->count && !m->items[m->focus].enabled)
    m->focus++;
  int width = S(202);
  for (int i = 0; i < m->count; i++) {
    const char *tab = m->items[i].label;
    while (*tab && *tab != '\t')
      tab++;
    char text[256];
    copy_text(text, sizeof(text), m->items[i].label);
    for (char *p = text; *p; p++)
      if (*p == '\t') {
        *p = 0;
        break;
      }
    width =
        MAX(width, text_width(text, FONT_BODY) + S(60) +
                       (*tab ? text_width(tab + 1, FONT_SMALL) + S(24) : 0));
  }
  width = MIN(width, desk.width - S(10));
  m->visible =
      MIN(m->count,
          MAX(1, (desk.height - S(DESKTOP_TOP + DESKTOP_BOTTOM + 12)) / S(24)));
  int height = m->visible * S(24) + S(12);
  m->box =
      (Rect){clampi(x, S(5), desk.width - width - S(5)),
             clampi(y, S(DESKTOP_TOP), desk.height - desktop_bottom() - height),
             width, height};
  transition_begin(m->box);
  invalidate(
      (Rect){m->box.x - S(8), m->box.y - S(8), width + S(16), height + S(16)});
}

static Rect palette_dot(Menu *m, int i) {
  if(m->color_row)return (Rect){m->box.x+S(10+i*24),m->box.y+m->box.h-S(32),S(22),S(24)};
  return (Rect){m->box.x+S(14+i*32),m->box.y+S(35),S(24),S(24)};
}
void menu_palette(Window *w,int x,int y,const MenuItem *items,const uint32_t *colors,unsigned selected,MenuAction action) {
  menu_open(w,x,y,items,8,action);
  Menu *m=&desk.menu;m->palette=true;m->palette_selected=selected;m->focus=MIN(7,selected);
  memcpy(m->palette_colors,colors,sizeof(m->palette_colors));
  invalidate(m->box);
  m->box=(Rect){clampi(x,S(5),desk.width-S(286)),clampi(y,S(DESKTOP_TOP),desk.height-desktop_bottom()-S(106)),S(280),S(102)};
  transition_begin(m->box);invalidate(m->box);
}
static int menu_inset(Menu *m) {(void)m;return S(6);}
void menu_color_row(const MenuItem *colors,const uint32_t *values,unsigned selected) {
  Menu *m=&desk.menu;invalidate(m->box);
  m->color_row=true;m->palette_selected=selected;
  memcpy(m->color_items,colors,sizeof(m->color_items));
  memcpy(m->palette_colors,values,sizeof(m->palette_colors));
  m->hover=-1;
  m->visible=MIN(m->count,MAX(1,(desk.height-S(DESKTOP_TOP)-desktop_bottom()-S(58))/S(24)));
  m->box.w=MAX(m->box.w,S(214));m->box.h=m->visible*S(24)+S(48);
  m->box.x=clampi(m->box.x,S(5),desk.width-m->box.w-S(5));
  m->box.y=clampi(m->box.y,S(DESKTOP_TOP),desk.height-desktop_bottom()-m->box.h);
  transition_begin(m->box);invalidate(m->box);
  if(desk.trace)printf("DESKTOP MENU_COLORS x=%d y=%d width=%d height=%d selected=%u\n",m->box.x,m->box.y,m->box.w,m->box.h,selected);
}
static void menu_color_choose(Menu *m,int i) {
  MenuAction cb=m->action;Window *w=m->owner;int action=m->color_items[i].action;
  trace("COMMAND",w,m->color_items[i].label);menu_close();if(cb)cb(w,action);
}
void menu_draw(void) {
  Menu *m = &desk.menu;
  Rect b = m->box;
  glass_system_prepare(3);
  blend((Rect){b.x + S(3), b.y + S(5), b.w, b.h}, THEME_SHADOW, 25);
  glass_system_draw(3, b, S(7));
  border(b, S(7), THEME_WINDOW_BORDER);
  if(m->palette) {
    label(b.x+S(16),b.y+S(12),"Folder color",FONT_BOLD,THEME_TEXT,b.w-S(32));
    for(int i=0;i<8;i++) {
      Rect r=palette_dot(m,i);
      if((m->palette?i==m->focus:m->color_keyboard&&i==m->hover) || contains(r,desk.mouse_x,desk.mouse_y)) {
        Rect ring={r.x-S(4),r.y-S(4),r.w+S(8),r.h+S(8)};
        border(ring,S(16),desk.accent);
      }
      rounded(r,S(12),i?m->palette_colors[i]:THEME_TOOLBAR);
      if(!i)border(r,S(12),THEME_SECONDARY_TEXT);
      if((unsigned)i==m->palette_selected)icon(r.x+S(5),r.y+S(5),I_CHECK,S(14),i?0xFFFFFF:THEME_TEXT);
    }
    {
      label(b.x+S(16),b.y+S(78),m->items[m->focus].label,FONT_BODY,THEME_SECONDARY_TEXT,b.w-S(32));
      return;
    }
  }
  Rect saved = desk.clip;
  clip_set(intersect(saved,
                     (Rect){b.x + S(5), b.y + menu_inset(m), b.w - S(10), m->visible*S(24)}));
  for (int i = m->first; i < MIN(m->count, m->first + m->visible + 1); i++) {
    Rect r = {b.x + S(5), b.y + menu_inset(m) + i * S(24) - m->scroll, b.w - S(10),
              S(24)};
    bool hover = contains(r, desk.mouse_x, desk.mouse_y), focus = i == m->focus;
    if (hover || focus) {
      rounded(r, S(4), focus ? theme_color(0xE9F2ED) : theme_color(0xF1F4F2));
    }
    uint32_t color =
        m->items[i].enabled ? theme_color(0x41525A) : theme_color(0xA9B3B4);
    char text[256];
    copy_text(text, sizeof(text), m->items[i].label);
    char *shortcut = NULL;
    for (char *p = text; *p; p++)
      if (*p == '\t') {
        *p = 0;
        shortcut = p + 1;
        break;
      }
    if (!text[0]) {
      fill((Rect){r.x + S(27), r.y + S(14), r.w - S(34), 1}, THEME_DIVIDER);
      continue;
    }
    int right = shortcut ? text_width(shortcut, FONT_SMALL) + S(20) : 0;
    icon(r.x + S(7), r.y + S(7), m->items[i].icon, S(15), color);
    label(r.x + S(31), r.y + S(7), text, FONT_BODY, color, r.w - S(38) - right);
    if (shortcut)
      label(r.x + r.w - right, r.y + S(9), shortcut, FONT_SMALL,
            m->items[i].enabled ? THEME_SECONDARY_TEXT : THEME_MUTED_TEXT,
            right - S(8));
  }

  clip_set(saved);
  if(m->color_row) {
    int top=b.y+b.h-S(38);
    fill((Rect){b.x+S(10),top,b.w-S(20),1},THEME_DIVIDER);
    for(int i=0;i<8;i++) {
      Rect hit=palette_dot(m,i),dot={hit.x+S(4),hit.y+S(5),S(14),S(14)};
      bool focus=m->color_keyboard && i==m->hover;
      if(focus || contains(hit,desk.mouse_x,desk.mouse_y))
        border((Rect){dot.x-S(3),dot.y-S(3),dot.w+S(6),dot.h+S(6)},S(10),THEME_SECONDARY_TEXT);
      rounded(dot,S(7),i?m->palette_colors[i]:THEME_TOOLBAR);
      if(!i) {border(dot,S(7),THEME_MUTED_TEXT);line(dot.x+S(3),dot.y+S(11),dot.x+S(11),dot.y+S(3),THEME_MUTED_TEXT);}
      if(i && (unsigned)i==m->palette_selected)icon(dot.x+S(3),dot.y+S(3),I_CHECK,S(8),0xFFFFFF);
    }
  }
  scrollbar((Rect){b.x, b.y + menu_inset(m), b.w, m->visible*S(24)}, m->count * S(24),
            m->scroll);
}

bool menu_mouse(int x, int y, unsigned pressed, int wheel) {
  Menu *m = &desk.menu;
  if (!m->open) {
    return false;
  }
  if (!contains(m->box, x, y)) {
    Window *bar = wm_focused();
    bool switch_menu = bar && !empty(menubar_hover(bar, x, y));
    /* Keep the origin until the heading handler decides toggle vs switch. */
    if (switch_menu && ((pressed & 1) || (m->origin_group && m->owner == bar)))
      return false;
    if (pressed)
      menu_close();
    return pressed != 0;
  }

  if(m->color_row && y>=m->box.y+m->box.h-S(38)) {
    for(int i=0;i<8;i++)if(contains(palette_dot(m,i),x,y)) {
      if(m->hover!=i){m->hover=i;invalidate(m->box);}
      if(pressed&1)menu_color_choose(m,i);
      break;
    }
    return true;
  }
  if(m->palette) {
    for(int i=0;i<8;i++)if(contains(palette_dot(m,i),x,y)) {
      if(m->focus!=i){m->focus=i;invalidate(m->box);}
      if(pressed&1) {
        int operation=m->items[i].action;Window *w=m->owner;MenuAction cb=m->action;
        trace("COMMAND",w,m->items[i].label);menu_close();if(cb)cb(w,operation);
      }
      break;
    }
    return true;
  }
  if (wheel) {
    smooth_by(&m->animation, m->scroll, -(int64_t)wheel * S(24),
              (m->count - m->visible) * S(24), now_ms());
    return true;
  }
  if (pressed & 1) {
    int index = (y - m->box.y - menu_inset(m) + m->scroll) / S(24);
    if (index >= 0 && index < m->count && y >= m->box.y + menu_inset(m) &&
        y < m->box.y + menu_inset(m) + m->visible*S(24) && m->items[index].enabled) {
      int action = m->items[index].action;
      Window *w = m->owner;
      MenuAction callback = m->action;
      trace("COMMAND", w, m->items[index].label);
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
  if(m->color_row) {
    if(k.code==KEY_LEFT || k.code==KEY_RIGHT) {
      if(!m->color_keyboard)m->hover=m->palette_selected;
      else m->hover=(m->hover+(k.code==KEY_LEFT?7:1))%8;
      m->color_keyboard=true;invalidate(m->box);return true;
    }
    if(m->color_keyboard && k.code==KEY_ENTER) {menu_color_choose(m,m->hover);return true;}
    if(k.code==KEY_DOWN && m->focus==m->count-1 && !m->color_keyboard) {
      m->color_keyboard=true;m->hover=m->palette_selected;invalidate(m->box);return true;
    }
    if(k.code==KEY_UP && m->color_keyboard) {
      m->color_keyboard=false;invalidate(m->box);return true;
    }
    if(k.code==KEY_HOME || k.code==KEY_END || k.code==KEY_TAB)m->color_keyboard=false;
  }
  if(m->palette && (k.code==KEY_LEFT || k.code==KEY_RIGHT)) {
    m->focus=(m->focus+(k.code==KEY_LEFT?7:1))%8;invalidate(m->box);return true;
  }
  if (k.code == KEY_ESCAPE) {
    menu_close();
  } else if (k.code == KEY_DOWN || k.code == KEY_UP || k.code == KEY_TAB ||
             k.code == KEY_HOME || k.code == KEY_END ||
             k.code == KEY_PAGEUP || k.code == KEY_PAGEDOWN) {
    int direction =
        k.code == KEY_UP || (k.code == KEY_TAB && (k.mods & MOD_SHIFT)) ? -1
                                                                        : 1;
    bool jump = k.code == KEY_HOME || k.code == KEY_END ||
                k.code == KEY_PAGEUP || k.code == KEY_PAGEDOWN;
    if (k.code == KEY_HOME) { m->focus=0;direction=1; }
    if (k.code == KEY_END) { m->focus=m->count-1;direction=-1; }
    if (k.code == KEY_PAGEUP || k.code == KEY_PAGEDOWN) {
      direction=k.code==KEY_PAGEUP?-1:1;
      m->focus=clampi(m->focus+direction*m->visible,0,m->count-1);
    }
    for (int tries = 0; tries < m->count; tries++) {
      if (!jump || tries) m->focus = (m->focus + direction + m->count) % m->count;
      if (m->items[m->focus].enabled) {
        break;
      }
    }

    if (m->focus < m->first) {
      m->first = m->focus;
    } else if (m->focus >= m->first + m->visible) {
      m->first = m->focus - m->visible + 1;
    }
    m->scroll = m->first * S(24);
    smooth_jump(&m->animation, m->scroll, now_ms());
    invalidate(m->box);
  } else if (k.code == KEY_ENTER && m->items[m->focus].enabled) {
    MenuAction cb = m->action;
    Window *w = m->owner;
    int action = m->items[m->focus].action;
    trace("COMMAND", w, m->items[m->focus].label);
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
  int position = smooth_tick(&m->animation, m->scroll,
                             (m->count - m->visible) * S(24), now_ms());
  if (position != m->scroll) {
    m->scroll = position;
    m->first = position / S(24);
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
  int width = MIN(S(500), desk.width - S(30)),
      height = MIN(S(228), desk.height - S(DESKTOP_TOP + 16));
  d->box = (Rect){(desk.width - width) / 2,
                  S(DESKTOP_TOP) + (desk.height - S(DESKTOP_TOP) - height) / 2,
                  width, height};
  copy_text(d->title, sizeof(d->title), title);
  copy_text(d->message, sizeof(d->message), message);
  if (desk.trace)
    printf("DESKTOP MODAL x=%d y=%d width=%d height=%d scale=%d\n", d->box.x,
           d->box.y, d->box.w, d->box.h, desk.scale);
  invalidate(
      (Rect){d->box.x - S(8), d->box.y - S(8), width + S(16), height + S(16)});
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
    append_text(b, sizeof(b),
                ". Choose another name; existing data is preserved.");
  } else if (error == -E_XDEV) {
    append_text(
        b, sizeof(b),
        ". Folders can be cut within one filesystem. Across mounts, copy the "
        "folder "
        "first, then move the original to Trash after verifying the copy.");
  } else if (error == -E_BUSY) {
    append_text(
        b, sizeof(b),
        ". Close programs using this file or directory, then try again.");
  } else if (error == -E_NOTSUP) {
    append_text(b, sizeof(b),
                ". The filesystem or application backend does not support this "
                "operation.");
  }
  dialog_notice(w, action, b);
  trace("ERROR", w, action);
}

void dialog_input(Window *w, const char *title, const char *message,
                  const char *value, DialogAction action) {
  dialog_setup(w, title, message);
  desk.dialog.input = true;
  desk.dialog.focus = 0;
  desk.dialog.action = action;
  copy_text(desk.dialog.value, sizeof(desk.dialog.value), value);
  desk.dialog.cursor = strlen(desk.dialog.value);
  desk.dialog.replace = value[0] != 0;
}

void dialog_password(Window *w,const char *title,const char *message,DialogAction action,DialogAction cancel) {
  dialog_input(w,title,message,"",action);
  desk.dialog.secret=true;desk.dialog.cancel_action=cancel;
}
static void secret_clear(void *p,size_t n) {volatile unsigned char *b=p;while(n--)*b++=0;}
void dialog_confirm(Window *w, const char *title, const char *message,
                    DialogAction action) {
  dialog_setup(w, title, message);
  desk.dialog.action = action;
}

static void dialog_accept(void) {
  Dialog snapshot = desk.dialog;
  desk.dialog.open = false;
  secret_clear(desk.dialog.value,sizeof(desk.dialog.value));
  invalidate((Rect){snapshot.box.x - S(8), snapshot.box.y - S(8),
                    snapshot.box.w + S(16), snapshot.box.h + S(16)});
  if (snapshot.action) {
    snapshot.action(snapshot.owner, snapshot.value);
  }
  secret_clear(&snapshot,sizeof(snapshot));
}

static void dialog_cancel(void) {
  power_review_cancel();
  desk.quit_pending = false;
  Rect b = desk.dialog.box;
  if(desk.dialog.cancel_action)desk.dialog.cancel_action(desk.dialog.owner,NULL);
  secret_clear(desk.dialog.value,sizeof(desk.dialog.value));
  desk.dialog.open = false;
  invalidate((Rect){b.x - S(8), b.y - S(8), b.w + S(16), b.h + S(16)});
}

static Rect dialog_message_view(Dialog *d) {
  Rect b = d->box;
  return (Rect){b.x + S(22), b.y + S(48), b.w - S(44),
                MAX(S(17), b.h - S(d->input ? 142 : 106))};
}
static int dialog_message_layout(Dialog *d, bool paint) {
  /* The same bounded UTF-8 wrapping measures and paints scrollable messages. */
  const char *p = d->message;
  int height = 0;
  Rect v = dialog_message_view(d), saved = desk.clip;
  if (paint)
    clip_set(intersect(saved, v));
  while (*p) {
    char line_text[128];
    int used = 0, last_space = -1;
    while (*p && *p != '\n' && used < 126) {
      int before = used, bytes = input_next(p, 0);
      if (used + bytes > 127)
        break;
      memcpy(line_text + used, p, bytes);
      used += bytes;
      p += bytes;
      line_text[used] = 0;
      if (bytes == 1 && line_text[used - 1] == ' ')
        last_space = used - 1;
      if (text_width(line_text, FONT_BODY) > v.w - S(12)) {
        int keep = last_space > 0 ? last_space : before;
        if (keep > 0) {
          p -= used - keep;
          used = keep;
        }
        while (*p == ' ')
          p++;
        break;
      }
    }

    if (*p == '\n') {
      p++;
    }
    line_text[used] = 0;
    int y = v.y + height - d->message_scroll;
    if (paint && y + S(17) > v.y && y < v.y + v.h)
      label(v.x, y, line_text, FONT_BODY, THEME_TEXT, v.w - S(12));
    height += S(17);
  }

  if (paint)
    clip_set(saved);
  return height;
}
static void dialog_scroll(Dialog *d, int delta) {
  d->message_height = dialog_message_layout(d, false);
  d->message_scroll =
      clampi(d->message_scroll + delta, 0,
             MAX(0, d->message_height - dialog_message_view(d).h));
  invalidate(d->box);
  if (desk.trace)
    printf("DESKTOP MODAL_SCROLL offset=%d height=%d viewport=%d\n",
           d->message_scroll, d->message_height, dialog_message_view(d).h);
}

void dialog_draw(void) {
  Dialog *d = &desk.dialog;
  Rect b = d->box;
  glass_system_prepare(4);
  blend((Rect){b.x + S(4), b.y + S(6), b.w, b.h}, THEME_SHADOW, 45);
  glass_system_draw(4, b, S(9));
  border(b, S(9), THEME_WINDOW_BORDER);
  label(b.x + S(22), b.y + S(18), d->title, FONT_BOLD, THEME_PRIMARY_TEXT,
        b.w - S(44));
  d->message_height = dialog_message_layout(d, false);
  d->message_scroll =
      clampi(d->message_scroll, 0,
             MAX(0, d->message_height - dialog_message_view(d).h));
  dialog_message_layout(d, true);
  scrollbar(dialog_message_view(d), d->message_height, d->message_scroll);

  if (d->input) {
    char masked[OS_PATH_MAX];
    size_t n=strlen(d->value);memset(masked,'*',n);masked[n]=0;
    input_draw((Rect){b.x + S(22), b.y + b.h - S(83), b.w - S(44), S(28)},
               d->secret?masked:d->value, d->secret?"Password":"Name or path", d->replace ? -1 : d->cursor, d->anchor,
               d->focus == 0);
  }
  Rect ok = {b.x + b.w - S(102), b.y + b.h - S(42), S(80), S(27)};
  button(ok, d->action ? (d->input ? "Apply" : "Confirm") : "OK", I_COUNT, true,
         true, d->focus == 2);
  if (d->action) {
    button((Rect){ok.x - S(92), ok.y, S(80), ok.h}, "Cancel", I_COUNT, false,
           true, d->focus == 1);
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

bool dialog_mouse(int x, int y, unsigned pressed, int wheel) {
  if (!desk.dialog.open) {
    return false;
  }
  Dialog *d = &desk.dialog;
  Rect b = d->box;
  if (wheel) {
    dialog_scroll(d, -wheel * S(51));
    return true;
  }
  if (pressed & 1) {
    Rect ok = {b.x + b.w - S(102), b.y + b.h - S(42), S(80), S(27)};
    if (contains(ok, x, y)) {
      dialog_accept();
    } else if (d->action &&
               contains((Rect){ok.x - S(92), ok.y, S(80), ok.h}, x, y)) {
      dialog_cancel();
    } else if (d->input && contains((Rect){b.x + S(22), b.y + b.h - S(83),
                                           b.w - S(44), S(28)},
                                    x, y)) {
      d->cursor = strlen(d->value);
      d->anchor = d->cursor;
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
  if (k.code == KEY_PAGEUP || k.code == KEY_PAGEDOWN) {
    dialog_scroll(d,
                  (k.code == KEY_PAGEUP ? -1 : 1) * dialog_message_view(d).h);
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
    unsigned letter=k.code>=4 && k.code<=0x1d?'a'+k.code-4:k.ascii|32;
    if(d->secret && (k.mods&MOD_CTRL) && (letter=='c' || letter=='x'))return;
    if ((k.mods & MOD_CTRL) && (k.ascii == 'a' || k.ascii == 'A')) {
      d->replace = true;
      invalidate(d->box);
      return;
    }
    if (d->replace) {
      d->cursor = strlen(d->value);
      d->anchor = 0;
      d->replace = false;
    }
    if (input_key(d->value, sizeof(d->value), &d->cursor, &d->anchor, k)) {
      invalidate(d->box);
    }
  }
}
