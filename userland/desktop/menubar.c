#include "desktop.h"

/* Menus dispatch the same application actions as buttons and keys. Terminal
   and games retain their Alt-letter/function keys; Ctrl+Alt+M opens their bar.
 */
static int groups(Window *w, int *out) {
  int n = 0;
  if (terminal_is_preferences(w)) {
    out[n++] = MENU_APP;
    out[n++] = MENU_FILE;
    out[n++] = MENU_WINDOW;
    out[n++] = MENU_HELP;
    return n;
  }
  if (w->app == APP_TERMINAL)
    out[n++] = MENU_APP;
  out[n++] = MENU_FILE;
  if (w->app == APP_FILES || w->app == APP_EDITOR || w->app == APP_CODE || w->app == APP_TERMINAL)
    out[n++] = MENU_EDIT;
  if (!native_window_app(w->app))
    out[n++] = MENU_VIEW;
  if (w->app == APP_FILES)
    out[n++] = MENU_GO;
  out[n++] = MENU_WINDOW;
  out[n++] = MENU_HELP;
  return n;
}
static const char *name(Window *w, int group) {
  static const char *names[] = {"File", "Edit", "View", "Go", "Window", "Help"};
  if (group == MENU_FILE) {
    if (w->app == APP_SETTINGS)
      return "Preferences";
    if (w->app == APP_POWER)
      return "System";
    if (w->app == APP_GAME)
      return "Game";
  }
  if (group == MENU_APP)
    return apps[w->app].name;
  return names[group];
}
static Rect group_rect(Window *w, int group) {
  int list[6], count = groups(w, list),
               x = S(41) + text_width(apps[w->app].name, FONT_BOLD) + S(16);
  for (int i = 0; i < count; i++) {
    if (list[i] == MENU_APP) {
      if (group == MENU_APP)
        return (Rect){S(32), 0,
                      text_width(apps[w->app].name, FONT_BOLD) + S(16),
                      S(DESKTOP_TOP)};
      continue;
    }
    int width = text_width(name(w, list[i]), FONT_NORMAL) + S(16);
    Rect r = {x, 0, width, S(DESKTOP_TOP)};
    if (list[i] == group)
      return r;
    x += width;
  }
  return (Rect){0};
}
void menubar_draw(Window *w) {
  int list[6], count = groups(w, list);
  for (int i = 0; i < count; i++) {
    Rect r = group_rect(w, list[i]);
    if (r.x + r.w > desk.width - S(190))
      continue;
    bool open = desk.menu.open && desk.menu.owner == w &&
                desk.menu.origin_group == list[i] + 1;
    if (open || contains(r, desk.mouse_x, desk.mouse_y))
      blend((Rect){r.x, r.y + 1, r.w, r.h - 2},
            open ? desk.selection : THEME_TEXT, open ? 160 : 22);
    label(r.x + S(8), r.y + S(4), name(w, list[i]),
          list[i] == MENU_APP ? FONT_BOLD : FONT_NORMAL,
          open ? desk.accent_text : THEME_TEXT, r.w - S(16));
  }
}
Rect menubar_hover(Window *w, int x, int y) {
  if (!w || !w->alive)
    return (Rect){0};
  int list[6], count = groups(w, list);
  for (int i = 0; i < count; i++) {
    Rect r = group_rect(w, list[i]);
    if (r.x + r.w <= desk.width - S(190) && contains(r, x, y))
      return r;
  }
  return (Rect){0};
}
void menu_send_key(Window *w, unsigned ascii, unsigned code, unsigned mods) {
  if (w && w->alive && apps[w->app].key) {
    apps[w->app].key(
        w, (Key){.ascii = ascii, .code = code, .mods = mods, .down = true});
    if (w->alive)
      apps[w->app].key(
          w, (Key){.ascii = ascii, .code = code, .mods = 0, .down = false});
  }
}
void desktop_help(Window *w) {
  const char *detail = "";
  if (w)
    switch (w->app) {
    case APP_FILES:
      detail = "Files: Ctrl+L path; Ctrl+F filter this folder; Ctrl+N file; "
               "Ctrl+Shift+N folder; F2 rename; Delete trash; Ctrl+C/X/V "
               "copy/cut/paste; F5 refresh.\n";
      break;
    case APP_EDITOR:
      detail = "Editor: Ctrl+S save; Ctrl+Shift+S Save As; Ctrl+A/C/X/V "
               "selection and clipboard; Shift+movement selects; Ctrl+Home/End "
               "document bounds.\n";
      break;
    case APP_TERMINAL:
      detail = "Terminal: Tab completes at the shell; Ctrl+Shift+C/V "
               "copy/paste; Shift+Insert paste; Ctrl+C interrupts the "
               "foreground command; arrows browse command history. Full-screen "
               "programs receive their own keys.\n";
      break;
    case APP_SETTINGS:
      detail = "Settings: Tab/Shift+Tab select a control; arrows change "
               "choices; Ctrl+S saves preferences. The visible configuration "
               "path identifies disk or volatile RAM storage.\n";
      break;
    case APP_POWER:
      detail = "System: View selects actual diagnostics; F5 refreshes cached "
               "data; Tab selects power controls; arrows/Page Up/Down scroll. "
               "Shutdown/restart always require application review.\n";
      break;
    case APP_DISKS:
      detail = "Disks: arrows select devices/partitions; F5 refreshes actual "
               "inventory; Page Up/Down and Home/End scroll details. This "
               "app is read-only; unsupported operations are labeled.\n";
      break;
    case APP_GAME:
      detail = "Doom: click the game to capture the pointer; F10 or Ctrl+Alt "
               "releases it. Focus changes release held keys. WASD moves; Ctrl "
               "fires; E/Space uses; Escape opens the game menu.\n";
      break;
    case APP_WORKBENCH:
      detail = "Code OSS: Ctrl+P opens a file; Ctrl+S saves; F1 opens the "
               "command palette. Use the editor's own File, Edit and View "
               "menus. The desktop entry restores the running editor.\n";
      break;
    case APP_CHROMIUM:
      detail = "Chromium Browser: Ctrl+L opens the address bar; Ctrl+T opens a "
               "tab; Ctrl+W closes a tab; Alt+Left/Right navigates history. "
               "F11 toggles fullscreen. Downloads go to your Downloads folder.\n";
      break;
    default:
      break;
    }
  char text[1200];
  copy_text(text, sizeof(text), detail);
  append_text(
      text, sizeof(text),
      "\nDesktop: Ctrl+Alt+M application menus; Left/Right switches open "
      "menus. Alt+Space window actions; Alt+Tab switches windows; Alt+F4 "
      "closes. Super+W lists all windows; Super+E/T/B opens "
      "Files/Terminal/Browser. Super+1/2/3 switches workspace; "
      "Super+Shift+1/2/3 moves a window. Super+Left/Right snaps; Super+Up/Down "
      "maximizes/restores/minimizes. Ctrl+Alt+Escape returns to console.");
  dialog_notice(w, "Keyboard reference", text);
}
static void help_action(Window *w, int action) {
  if (!action)
    desktop_help(w);
  else
    wm_open(APP_POWER, NULL);
}
static void game_action(Window *w, int action) {
  if (!action)
    wm_open(APP_GAME, NULL);
  else if (action == 1)
    game_focus_changed();
  else
    wm_close(w);
}
static void workbench_action(Window *w, int action) {
  if (!action)
    wm_focus(w);
  else
    wm_close(w);
}
static void open_group(Window *w, int group) {
  if (!w || !w->alive)
    return;
  Rect r = group_rect(w, group);
  if (empty(r))
    return;
  if (group == MENU_WINDOW)
    wm_actions(w, r.x, r.y + r.h);
  else if (group == MENU_HELP) {
    MenuItem items[] = {{"Keyboard reference", I_INFO, 0, true},
                        {"System information", I_INFO, 1, true}};
    menu_open(w, r.x, r.y + r.h, items, 2, help_action);
  } else
    switch (w->app) {
    case APP_FILES:
      files_menu(w, group, r.x, r.y + r.h);
      break;
    case APP_CODE:
      codeoss_menu(w,group,r.x,r.y+r.h);
      break;
    case APP_EDITOR:
      editor_menu(w, group, r.x, r.y + r.h);
      break;
    case APP_TERMINAL:
      terminal_menu(w, group, r.x, r.y + r.h);
      break;
    case APP_SETTINGS:
      settings_menu(w, group, r.x, r.y + r.h);
      break;
    case APP_POWER:
      power_menu(w, group, r.x, r.y + r.h);
      break;
    case APP_DISKS:
      disks_menu(w, group, r.x, r.y + r.h);
      break;
    case APP_GAME: {
      MenuItem items[] = {{"New Doom window", I_GRID, 0, true},
                          {"Close game window\tAlt+F4", I_CLOSE, 2, true}};
      menu_open(w, r.x, r.y + r.h, items, 2, game_action);
      break;
    }
    case APP_WORKBENCH:
    case APP_CHROMIUM: {
      MenuItem items[] = {{w->app==APP_CHROMIUM ? "Show Chromium Browser" : "Show Code OSS", apps[w->app].icon, 0, true},
                          {"Close window\tAlt+F4", I_CLOSE, 1, true}};
      menu_open(w, r.x, r.y + r.h, items, 2, workbench_action);
      break;
    }
    default:
      return;
    }
  if (desk.menu.open) {
    desk.menu.origin_group = group + 1;
    trace("MENU", w, name(w, group));
    if (desk.trace)
      printf("DESKTOP MENUBAR_GEOMETRY group=%d x=%d y=%d width=%d height=%d\n",
             group, r.x, r.y, r.w, r.h);
    invalidate_window(w);
    invalidate((Rect){0, 0, desk.width, S(DESKTOP_TOP)});
  }
}
bool menubar_mouse(Window *w, int x, int y, unsigned pressed) {
  int list[6], count = groups(w, list);
  for (int i = 0; i < count; i++)
    if (group_rect(w, list[i]).x + group_rect(w, list[i]).w <=
            desk.width - S(190) &&
        contains(group_rect(w, list[i]), x, y)) {
      if (!pressed && desk.menu.open && desk.menu.owner == w &&
          desk.menu.origin_group && desk.menu.origin_group != list[i] + 1 &&
          !desk.buttons) {
        open_group(w, list[i]);
        trace("MENU_HOVER_SWITCH", w, name(w, list[i]));
      }
      if (pressed & 1) {
        if (desk.menu.open && desk.menu.owner == w &&
            desk.menu.origin_group == list[i] + 1) {
          menu_close();
          trace("MENU_TOGGLE_CLOSED", w, name(w, list[i]));
        } else
          open_group(w, list[i]);
      }
      return true;
    }
  return false;
}
bool menubar_key(Key k) {
  if (!k.down)
    return false;
  Window *w = wm_focused();
  unsigned letter =
      k.code >= 4 && k.code <= 0x1d ? 'a' + k.code - 4 : k.ascii | 32;
  if (k.mods & MOD_SUPER) {
    if (letter == 'w') {
      desktop_windows_menu(S(64), desk.height - S(46));
      return true;
    }
    if (letter == 'e' || letter == 't' || letter == 'b') {
      wm_open(letter == 'e'   ? APP_FILES
              : letter == 't' ? APP_TERMINAL
                              : APP_CHROMIUM,
              NULL);
      return true;
    }
  }
  if (w && (k.mods & MOD_ALT) && k.code == KEY_SPACE) {
    wm_actions(w, w->box.x + S(8), w->box.y + S(40));
    return true;
  }
  if (w && (k.mods & MOD_CTRL) && (k.mods & MOD_ALT) && letter == 'm') {
    open_group(w, MENU_FILE);
    return true;
  }
  if (desk.menu.open && desk.menu.origin_group &&
      (k.code == KEY_LEFT || k.code == KEY_RIGHT)) {
    w = desk.menu.owner;
    if (!w || !w->alive) {
      menu_close();
      return true;
    }
    int list[6], count = groups(w, list), current = desk.menu.origin_group - 1;
    for (int i = 0; i < count; i++)
      if (list[i] == current) {
        open_group(w,
                   list[(i + (k.code == KEY_RIGHT ? 1 : count - 1)) % count]);
        break;
      }
    return true;
  }
  if (w && w->app != APP_TERMINAL && !native_window_app(w->app)) {
    if (k.code == KEY_F1 && !(k.mods & (MOD_ALT | MOD_CTRL | MOD_SUPER))) {
      desktop_help(w);
      return true;
    }
    if ((k.mods & MOD_ALT) && !(k.mods & (MOD_CTRL | MOD_SUPER))) {
      const char keys[] = "fevgwh";
      int list[6], count = groups(w, list);
      for (int i = 0; i < count; i++)
        if (letter == (unsigned)keys[list[i]]) {
          open_group(w, list[i]);
          return true;
        }
    }
  }
  return false;
}
static char window_labels[MAX_WINDOWS][160];
static void windows_action(Window *unused, int action) {
  (void)unused;
  if (action >= 1000) {
    for (int i = 0; i < MAX_WINDOWS; i++)
      if (desk.windows[i].alive && desk.windows[i].id == action - 1000) {
        wm_focus(&desk.windows[i]);
        return;
      }
  } else if (action < 2)
    wm_arrange(action == 0);
  else
    for (int i = 0; i < MAX_WINDOWS; i++) {
      Window *w = &desk.windows[i];
      if (w->alive && w->workspace == desk.workspace) {
        if (action == 2 && !w->minimized)
          wm_minimize(w);
        if (action == 3 && w->minimized)
          wm_focus(w);
      }
    }
}
void desktop_windows_menu(int x, int y) {
  MenuItem items[MAX_MENU_ITEMS];
  int n = 0, visible = 0, minimized = 0;
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < MAX_WINDOWS; i++) {
      Window *w = &desk.windows[i];
      if (!w->alive || (w->workspace == desk.workspace) != (pass == 0))
        continue;
      char *text = window_labels[n], number[24];
      copy_text(text, sizeof(window_labels[0]), w->title);
      append_text(text, sizeof(window_labels[0]),
                  w->minimized ? " (minimized)\tWS " : "\tWS ");
      format_uint(number, w->workspace + 1);
      append_text(text, sizeof(window_labels[0]), number);
      items[n++] = (MenuItem){text, apps[w->app].icon, 1000 + w->id, true};
      if (w->workspace == desk.workspace) {
        if (w->minimized)
          minimized++;
        else
          visible++;
      }
    }
  items[n++] = (MenuItem){"Tile two front windows", I_LAYOUT, 0,
                          visible >= 2 && desk.width >= S(1060)};
  items[n++] = (MenuItem){"Cascade workspace", I_WORKSPACE, 1, visible > 0};
  items[n++] = (MenuItem){"Minimize workspace", I_MINUS, 2, visible > 0};
  items[n++] =
      (MenuItem){"Restore workspace windows", I_RESTORE, 3, minimized > 0};
  menu_open(NULL, x,
            y <= S(DESKTOP_TOP) ? S(DESKTOP_TOP) : y - n * S(24) - S(12), items,
            n, windows_action);
  trace("WINDOW_LIST", NULL, "all workspaces");
}
static void status_action(Window *unused, int action) {
  (void)unused;
  static const char *sections[] = {NULL, "processes", "network", "audio",
                                   "storage"};
  if (action >= 0 && action < 5)
    wm_open(APP_POWER, sections[action]);
  else if (action >= 10 && action < 13)
    workspace_switch(action - 10);
  else if (action == 6)
    wm_open(APP_DISKS, NULL);
  else
    wm_open(APP_SETTINGS, NULL);
}
void desktop_status_menu(int x, int y) {
  MenuItem items[10] = {{"System & Power", I_INFO, 0, true},
                        {"Processes", I_TERMINAL, 1, true},
                        {"Network diagnostics", I_GLOBE, 2, true},
                        {"Audio device", I_SOUND, 3, true},
                        {"Storage mounts", I_DRIVE, 4, true},
                        {"Desktop preferences", I_GEAR, 5, true},
                        {"Disks", I_DRIVE, 6, true}};
  const char *workspaces[] = {"Workspace 1\tSuper+1", "Workspace 2\tSuper+2",
                              "Workspace 3\tSuper+3"};
  int count = 7;
  for (int i = 0; i < desk.workspace_count; i++)
    items[count++] =
        (MenuItem){workspaces[i], i == desk.workspace ? I_CHECK : I_WORKSPACE,
                   10 + i, true};
  menu_open(NULL, x,
            y <= S(DESKTOP_TOP) ? S(DESKTOP_TOP) : y - count * S(24) - S(12),
            items, count, status_action);
  trace("STATUS_MENU", NULL, "system diagnostics");
}
