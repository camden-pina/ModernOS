#include "desktop.h"

static Rect status_box(void) {
  return (Rect){desk.width - S(180), 0, S(40), S(DESKTOP_TOP)};
}
static Rect windows_box(void) {
  return (Rect){desk.width - S(216), 0, S(40), S(DESKTOP_TOP)};
}

Rect taskbar_hover(int x, int y) {
  if (y < S(DESKTOP_TOP)) {
    Window *w = wm_focused();
    if (contains((Rect){0, 0, S(40), S(DESKTOP_TOP)}, x, y))
      return (Rect){0, 0, S(40), S(DESKTOP_TOP)};
    if (contains(status_box(), x, y))
      return status_box();
    if (contains(windows_box(), x, y))
      return windows_box();
    return w ? menubar_hover(w, x, y) : (Rect){0};
  }
  return dock_hover(x, y);
}
void taskbar_draw(void) {
  uint32_t ink = desk.dark_mode ? 0xE9ECF4 : 0x26324A;
  glass_system_prepare(0);
  glass_system_draw(0, (Rect){0, 0, desk.width, S(DESKTOP_TOP)}, 0);
  blend((Rect){0, S(DESKTOP_TOP) - 1, desk.width, 1}, 0xFFFFFF, 28);
  icon(S(11), S(4), I_GRID, S(13), ink);
  Window *active = wm_focused();
  if(!active || active->app!=APP_TERMINAL)
  label(S(40), S(4), active ? apps[active->app].name : "Coast", FONT_BOLD,
        ink, MAX(S(50), desk.width - S(248)));
  if (active)
    menubar_draw(active);
  if (desk.width >= S(760))
    icon(desk.width - S(208), S(4), I_WORKSPACE, S(13), ink);
  icon(desk.width - S(172), S(4), I_INFO, S(13), ink);
  label(desk.width - S(142), S(4), desk.date, FONT_NORMAL, ink, S(74));
  label(desk.width - S(68), S(4), desk.clock, FONT_MEDIUM, ink, S(65));

  dock_draw();
}

static Window launcher_shadow;
static Rect launcher_bounds(Rect b) {return (Rect){b.x-S(34),b.y-S(14),b.w+S(68),b.h+S(62)};}
static bool matches(const char *name);
static int launcher_indices(int *indices) {
  int count = 0;
  for (int i = 0; i < APP_COUNT; i++) {
    if (!apps[i].name) continue;
    if (!app_available((App)i)) continue;
    if (i != APP_CODE && matches(apps[i].name))
      indices[count++] = i;
  }
  /* Keep the earlier text-core preview behind the complete editor in search. */
  if (app_available(APP_CODE) && matches(apps[APP_CODE].name))
    indices[count++] = APP_CODE;
  if (matches("Return to console"))
    indices[count++] = APP_COUNT;
  return count;
}
static Rect launcher_disclosure(void) {
  Rect b=desk.launcher_box;
  return (Rect){b.x+b.w-S(42),b.y+S(16),S(30),S(32)};
}
static void launcher_layout(void) {
  Rect old=desk.launcher_box;
  int indices[APP_COUNT+1],count=launcher_indices(indices);
  int width=MIN(S(640),desk.width-S(40));
  int limit=clampi(desk.launcher_rows,5,9);
  bool results=desk.launcher_query[0] || desk.launcher_expanded;
  desk.launcher_visible=results?MIN(MAX(1,count),MIN(limit,
    MAX(1,(desk.height-S(DESKTOP_TOP+96)-desktop_bottom())/S(40)))):0;
  int height=desk.launcher_visible?S(96)+desk.launcher_visible*S(40):S(64);
  desk.launcher_box=(Rect){(desk.width-width)/2,MAX(S(DESKTOP_TOP+8),
    MIN(desk.height/4,desk.height-desktop_bottom()-S(8)-height)),width,height};
  if(memcmp(&old,&desk.launcher_box,sizeof(old))) {
    invalidate(launcher_bounds(old));
    invalidate(launcher_bounds(desk.launcher_box));
    if(desk.trace)printf("DESKTOP LAUNCHER x=%d y=%d width=%d height=%d visible=%d scale=%d\n",
      desk.launcher_box.x,desk.launcher_box.y,width,height,desk.launcher_visible,desk.scale);
  }
}
static void launcher_reveal(int count) {
  if (!count || !desk.launcher_visible) {
    desk.launcher_focus = desk.launcher_first = 0;
    return;
  }
  desk.launcher_focus = clampi(desk.launcher_focus, 0, count - 1);
  if (desk.launcher_focus < desk.launcher_first)
    desk.launcher_first = desk.launcher_focus;
  if (desk.launcher_focus >= desk.launcher_first + desk.launcher_visible)
    desk.launcher_first = desk.launcher_focus - desk.launcher_visible + 1;
  desk.launcher_first =
      clampi(desk.launcher_first, 0, MAX(0, count - desk.launcher_visible));
}
/* One highlighted app shared by pointer and keyboard navigation. Damage both
   old and new rows: cursor rectangles alone cannot repair a translucent list.
 */
Rect launcher_hover(int x, int y) {
  if (!desk.launcher)
    return (Rect){0};
  Rect b = desk.launcher_box;
  if(contains(launcher_disclosure(),x,y))return launcher_disclosure();
  int indices[APP_COUNT + 1], count = launcher_indices(indices);
  for (int row = 0;
       row < desk.launcher_visible && desk.launcher_first + row < count;
       row++) {
    Rect r = {b.x + S(8), b.y + S(88) + row * S(40), b.w - S(16), S(38)};
    if (contains(r, x, y))
      return r;
  }
  return (Rect){0};
}
void launcher_pointer(int x, int y, bool moved) {
  if (!moved || !desk.launcher || desk.buttons)
    return;
  Rect row = launcher_hover(x, y), b = desk.launcher_box;
  if (empty(row) || row.y<b.y+S(88))
    return;
  int next = desk.launcher_first + (row.y - b.y - S(88)) / S(40);
  if (next != desk.launcher_focus) {
    int old = desk.launcher_focus - desk.launcher_first;
    if (old >= 0 && old < desk.launcher_visible)
      compositor_damage(
          (Rect){b.x + S(8), b.y + S(88) + old * S(40), b.w - S(16), S(38)});
    desk.launcher_focus = next;
    compositor_damage(row);
    if (desk.trace)
      printf("DESKTOP LAUNCHER_FOCUS index=%d source=pointer\n", next);
  }
}
bool taskbar_mouse(int x, int y, unsigned pressed, int wheel) {
  if (desk.launcher && contains(desk.launcher_box, x, y)) {
    int indices[APP_COUNT + 1], count = launcher_indices(indices);
    if((pressed&1) && contains(launcher_disclosure(),x,y)) {
      desk.launcher_expanded=!desk.launcher_expanded;launcher_layout();invalidate(desk.launcher_box);return true;
    }
    if (wheel && desk.launcher_visible) {
      desk.launcher_first = clampi(desk.launcher_first - wheel, 0,
                                   MAX(0, count - desk.launcher_visible));
      desk.launcher_focus =
          count ? clampi(desk.launcher_focus, desk.launcher_first,
                         MIN(count - 1,
                             desk.launcher_first + desk.launcher_visible - 1))
                : 0;
      invalidate(desk.launcher_box);
    }
    if ((pressed & 1) && y >= desk.launcher_box.y + S(88)) {
      int row = (y - desk.launcher_box.y - S(88)) / S(40);
      int at = desk.launcher_first + row;
      if (row < desk.launcher_visible && at < count) {
        int app = indices[at];
        launcher_close();
        if (app == APP_COUNT)
          desktop_quit();
        else
          wm_open(app, NULL);
      }
    }
    return true;
  }

  if (y < S(DESKTOP_TOP)) {
    if (!pressed && wm_focused() && x >= S(40) && x < status_box().x)
      menubar_mouse(wm_focused(), x, y, 0);
    if (pressed) {
      if (x < S(40))
        launcher_toggle();
      else if (contains(windows_box(), x, y) && desk.width >= S(760))
        desktop_windows_menu(x, S(DESKTOP_TOP));
      else if (x >= status_box().x)
        desktop_status_menu(x, S(DESKTOP_TOP));
      else if (wm_focused())
        menubar_mouse(wm_focused(), x, y, pressed);
    }
    return true;
  }
  if (dock_mouse(x, y, pressed, wheel))
    return true;
  if (desk.launcher && pressed)
    launcher_close();
  return false;
}

void launcher_close(void) {
  if (!desk.launcher) {
    return;
  }
  Rect b = desk.launcher_box;
  transition_begin(launcher_bounds(b));
  desk.launcher = false;
  /* Shadows extend beyond the popup. Clear their complete bounds as well. */
  invalidate(launcher_bounds(b));
  invalidate(taskbar_dock_rect());
}

void launcher_toggle(void) {
  menu_close();
  if (desk.dialog.open) {
    return;
  }
  if (desk.launcher) {
    launcher_close();
    return;
  }
  desk.launcher = true;
  desk.launcher_expanded=false;
  desk.launcher_query[0]=0;desk.launcher_cursor=desk.launcher_anchor=0;
  desk.launcher_focus=desk.launcher_first=0;
  launcher_layout();transition_begin(launcher_bounds(desk.launcher_box));
  invalidate(taskbar_dock_rect());
}

static bool matches(const char *name) {
  if (!desk.launcher_query[0]) {
    return true;
  }
  for (int a = 0; name[a]; a++) {
    int b = 0;
    while (desk.launcher_query[b] && name[a + b]) {
      char c = name[a + b], d = desk.launcher_query[b];
      if (c >= 'A' && c <= 'Z') {
        c += 32;
      }
      if (d >= 'A' && d <= 'Z') {
        d += 32;
      }
      if (c != d) {
        break;
      }
      b++;
    }

    if (!desk.launcher_query[b]) {
      return true;
    }
  }

  return false;
}

void launcher_draw(void) {
  Rect b = desk.launcher_box, saved = desk.clip;
  glass_system_prepare(2);
  launcher_shadow.box=b;window_shadow(&launcher_shadow,S(16));
  glass_system_draw(2, b, S(16));
  border(b, S(16), THEME_WINDOW_BORDER);
  icon(b.x+S(20),b.y+S(20),I_SEARCH,S(24),THEME_SECONDARY_TEXT);
  input_material_draw((Rect){b.x + S(54), b.y + S(12), b.w - S(108), S(40)},
             desk.launcher_query, "Spotlight Search", desk.launcher_cursor,
             desk.launcher_anchor, true);
  Rect toggle=launcher_disclosure();
  if(contains(toggle,desk.mouse_x,desk.mouse_y))rounded(toggle,S(6),THEME_TOOLBAR);
  int cx=toggle.x+toggle.w/2,cy=toggle.y+toggle.h/2;
  int tip=desk.launcher_expanded?-S(3):S(3);
  line(cx-S(5),cy-tip,cx,cy+tip,THEME_SECONDARY_TEXT);
  line(cx,cy+tip,cx+S(5),cy-tip,THEME_SECONDARY_TEXT);
  if(!desk.launcher_visible)return;
  label(b.x + S(18), b.y + S(68), "Applications", FONT_SMALL,
        THEME_SECONDARY_TEXT, b.w - S(30));
  int indices[APP_COUNT + 1], count = launcher_indices(indices);
  launcher_reveal(count);
  clip_set(intersect(saved, (Rect){b.x + S(6), b.y + S(88), b.w - S(12),
                                   desk.launcher_visible * S(40)}));
  for (int row = 0;
       row < desk.launcher_visible && desk.launcher_first + row < count;
       row++) {
    int at = desk.launcher_first + row, app = indices[at];
    Rect r = {b.x + S(8), b.y + S(88) + row * S(40), b.w - S(16), S(38)};
    if (at == desk.launcher_focus)
      rounded(r, S(7), desk.selection);
    icon(r.x + S(10), r.y + S(7), app < APP_COUNT ? apps[app].icon : I_CLOSE,
         S(24), THEME_TEXT);
    const char *hint = app == APP_FILES      ? "Super+E"
                       : app == APP_TERMINAL ? "Super+T"
                       : app == APP_CHROMIUM ? "Super+B"
                                             : "";
    int hint_width = hint[0] ? text_width(hint, FONT_SMALL) + S(14) : 0;
    label(r.x + S(40), r.y + S(6),
          app < APP_COUNT ? apps[app].name : "Return to console", FONT_BODY,
          THEME_TEXT, r.w - S(44) - hint_width);
    if (hint_width)
      label(r.x + r.w - hint_width, r.y + S(13), hint, FONT_SMALL,
            THEME_SECONDARY_TEXT, hint_width - S(5));
  }
  if (!count)
    label(b.x + S(20), b.y + S(100), "No matching application", FONT_BODY,
          THEME_SECONDARY_TEXT, b.w - S(40));
  clip_set(saved);

}
bool taskbar_key(Key k) {
  if ((k.mods & MOD_SUPER) && k.down && k.code == KEY_SPACE) {
    desk.super_used = true;
    launcher_toggle();
    return true;
  }
  if (!desk.launcher || !k.down)
    return false;
  int indices[APP_COUNT + 1], count = launcher_indices(indices);
  if(!desk.launcher_visible && (k.code==KEY_DOWN || k.code==KEY_TAB)) {
    desk.launcher_expanded=true;launcher_layout();invalidate(desk.launcher_box);return true;
  }
  if (k.code == KEY_ESCAPE)
    launcher_close();
  else if (k.code == KEY_UP || k.code == KEY_DOWN || k.code == KEY_TAB) {
    int step = k.code == KEY_UP || (k.code == KEY_TAB && (k.mods & MOD_SHIFT))
                   ? -1
                   : 1;
    if (count)
      desk.launcher_focus = (desk.launcher_focus + step + count) % count;
    launcher_reveal(count);
    invalidate(desk.launcher_box);
  } else if (k.code == KEY_HOME || k.code == KEY_END || k.code == KEY_PAGEUP ||
             k.code == KEY_PAGEDOWN) {
    if (k.code == KEY_HOME)
      desk.launcher_focus = 0;
    else if (k.code == KEY_END)
      desk.launcher_focus = MAX(0, count - 1);
    else
      desk.launcher_focus =
          clampi(desk.launcher_focus +
                     (k.code == KEY_PAGEUP ? -1 : 1) * desk.launcher_visible,
                 0, MAX(0, count - 1));
    launcher_reveal(count);
    invalidate(desk.launcher_box);
  } else if (k.code == KEY_ENTER && count && desk.launcher_visible) {
    int app = indices[clampi(desk.launcher_focus, 0, count - 1)];
    launcher_close();
    if (app == APP_COUNT)
      desktop_quit();
    else
      wm_open(app, NULL);
  } else if (input_key(desk.launcher_query, sizeof(desk.launcher_query),
                       &desk.launcher_cursor, &desk.launcher_anchor, k)) {
    desk.launcher_focus = desk.launcher_first = 0;
    launcher_layout();
    invalidate(desk.launcher_box);
  }
  return true;
}
