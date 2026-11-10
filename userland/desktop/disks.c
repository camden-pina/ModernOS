#include "desktop.h"
#include "disk_inventory.h"

typedef struct {
  DiskInventory inventory;
  int selected, first, scroll, last_visible;
  long error;
} Disks;
static int sidebar_width(Rect b) { return MIN(S(160), b.w * 30 / 100); }
Rect disks_sidebar_rect(Window *w) {
  Rect b = window_body(w);
  return (Rect){b.x, b.y, sidebar_width(b), b.h};
}
static Rect refresh_button(Window *w) {
  Rect b = w->box;
  return (Rect){b.x + b.w - S(112), b.y + S(4), S(90), S(24)};
}
static int sidebar_top(Window *w) {
  return window_body(w).h < S(200) ? S(30) : S(34);
}
static int visible_rows(Window *w) {
  return MAX(1, (window_body(w).h - sidebar_top(w) - S(24)) / S(34));
}
static void reveal(Window *w) {
  Disks *d = w->state;
  int visible = visible_rows(w);
  d->selected = clampi(d->selected, 0, MAX(0, d->inventory.count - 1));
  if (d->selected < d->first)
    d->first = d->selected;
  if (d->selected >= d->first + visible)
    d->first = d->selected - visible + 1;
  d->first = clampi(d->first, 0, MAX(0, d->inventory.count - visible));
}
static void refresh(Window *w) {
  Disks *d = w->state;
  char buffer[8193], selected[32] = {0};
  if (d->inventory.count)
    copy_text(selected, sizeof(selected),
              d->inventory.entries[d->selected].name);
  long n = call3(SYS_sysinfo, INFO_LSBLK, buffer, 8192);
  d->error = n < 0 ? n : 0;
  d->inventory = (DiskInventory){0};
  if (n >= 0 && n <= 8192) {
    buffer[n] = 0;
    disk_parse(&d->inventory, buffer, n);
    if (n >= 8191)
      d->inventory.incomplete = true;
  } else if (n > 8192)
    d->error = -E_IO;
  d->selected = 0;
  d->scroll = 0;
  for (int i = 0; i < d->inventory.count; i++)
    if (disk_equal(selected, d->inventory.entries[i].name))
      d->selected = i;
  reveal(w);
  invalidate_window(w);
  if (desk.trace)
    printf("DESKTOP DISKS_INVENTORY window=%d count=%d incomplete=%d error=%ld "
           "read_only=1\n",
           w->id, d->inventory.count, d->inventory.incomplete, d->error);
}
int disks_open(Window *w, const char *path) {
  (void)path;
  w->state = allocate(sizeof(Disks));
  if (!w->state)
    return -E_NOMEM;
  memset(w->state, 0, sizeof(Disks));
  refresh(w);
  return 0;
}
static void property(Rect b, int *y, const char *name, const char *value,
                     bool narrow) {
  label(b.x, *y, name, FONT_NORMAL, THEME_SECONDARY_TEXT, b.w);
  label(b.x + (narrow ? 0 : S(130)), *y + (narrow ? S(21) : 0), value,
        FONT_BODY, THEME_PRIMARY_TEXT, MAX(1, b.w - (narrow ? 0 : S(130))));
  *y += S(narrow ? 42 : 28);
}
void disks_draw(Window *w) {
  Disks *d = w->state;
  Rect b = window_body(w), saved = desk.clip;
  int side = sidebar_width(b);
  if (d->last_visible != visible_rows(w)) {
    d->last_visible = visible_rows(w);
    reveal(w);
  }
  fill(b, THEME_WINDOW);
  window_material(w, disks_sidebar_rect(w), 1);
  fill((Rect){b.x + side, b.y, 1, b.h}, THEME_BORDER);
  label(b.x + S(16), b.y + S(12), "STORAGE", FONT_SMALL, THEME_SECONDARY_TEXT,
        side - S(26));
  int visible = visible_rows(w);
  for (int i = d->first; i < MIN(d->inventory.count, d->first + visible); i++) {
    DiskEntry *e = &d->inventory.entries[i];
    Rect row = {b.x + S(7), b.y + sidebar_top(w) + (i - d->first) * S(34),
                side - S(14), S(32)};
    bool selected = i == d->selected;
    if (selected)
      rounded(row, S(5), desk.accent);
    int indent = e->partition ? S(12) : 0;
    icon(row.x + S(9) + indent, row.y + S(9), I_DRIVE, MIN(S(18), ICON_MAX),
         selected ? THEME_ON_ACCENT : THEME_SECONDARY_TEXT);
    label(row.x + S(34) + indent, row.y + S(5), e->name, FONT_MEDIUM,
          selected ? THEME_ON_ACCENT : THEME_PRIMARY_TEXT,
          row.w - S(45) - indent);
    label(row.x + S(34) + indent, row.y + S(19),
          e->partition ? "Partition" : "Device", FONT_SMALL,
          selected ? THEME_ON_ACCENT : THEME_SECONDARY_TEXT,
          row.w - S(45) - indent);
  }
  if (!d->inventory.count)
    label(b.x + S(16), b.y + S(58),
          d->error ? "Unavailable" : "No block devices", FONT_NORMAL,
          THEME_SECONDARY_TEXT, side - S(24));
  if (b.h >= S(200))
    label(b.x + S(12), b.y + b.h - S(19), "Read-only inventory", FONT_SMALL,
          THEME_SECONDARY_TEXT, side - S(18));
  Rect panel = {b.x + side + S(18), b.y + S(14), b.w - side - S(36),
                b.h - S(28)};
  Rect viewport = panel;
  clip_set(intersect(saved, viewport));
  bool narrow = panel.w < S(440);
  int content =
      S((d->inventory.count ? 108 + 9 * (narrow ? 42 : 28) : 64) + 10 + 18 +
        24 + (d->inventory.incomplete ? 4 * 25 + 8 + 17 : 3 * 25 + 17));
  d->scroll = clampi(d->scroll, 0, MAX(0, content - viewport.h));
  int y = viewport.y - d->scroll;
  if (d->inventory.count) {
    DiskEntry *e = &d->inventory.entries[d->selected];
    icon(panel.x, y + S(7), I_DRIVE, MIN(S(28), ICON_MAX), desk.accent);
    label(panel.x + S(48), y + S(4), e->name, narrow ? FONT_BOLD : FONT_TITLE,
          THEME_PRIMARY_TEXT, panel.w - S(50));
    char size[32];
    format_size(size, e->bytes);
    label(panel.x + S(48), y + S(28), size, FONT_MEDIUM, THEME_SECONDARY_TEXT,
          panel.w - S(50));
    y += S(64);
    rounded((Rect){panel.x, y, panel.w, S(28)}, S(6), desk.selection);
    label(panel.x + S(12), y + S(9),
          e->partition ? "PARTITION CAPACITY" : "DEVICE CAPACITY", FONT_SMALL,
          desk.accent_text, panel.w - S(24));
    y += S(44);
    char exact[40], sector[40];
    format_uint(exact, e->bytes);
    append_text(exact, sizeof(exact), " bytes");
    format_uint(sector, e->sector);
    append_text(sector, sizeof(sector), " bytes");
    property(panel, &y, "Capacity", exact, narrow);
    property(panel, &y, "Type", e->partition ? "Partition" : "Block device",
             narrow);
    property(panel, &y, "Filesystem",
             disk_equal(e->fs, "-") ? "Not reported" : e->fs, narrow);
    property(panel, &y, "Mount point",
             disk_equal(e->mount, "-") ? "Not mounted" : e->mount, narrow);
    property(panel, &y, "OS access", e->readonly ? "Read-only" : "Read / write",
             narrow);
    property(panel, &y, "Sector size", sector, narrow);
    property(panel, &y, "Parent",
             e->parent >= 0 ? d->inventory.entries[e->parent].name
             : e->partition ? "Not reported"
                            : "None (whole device)",
             narrow);
    property(panel, &y, "Space used / free", "Unavailable from OS", narrow);
    property(panel, &y, "Partition map / UUID", "Unavailable from OS", narrow);
  } else {
    label(panel.x, y + S(14),
          d->error ? error_text(d->error) : "No devices reported", FONT_BODY,
          THEME_PRIMARY_TEXT, panel.w);
    y += S(64);
  }
  y += S(10);
  fill((Rect){panel.x, y, panel.w, 1}, THEME_DIVIDER);
  y += S(18);
  label(panel.x, y, "OPERATIONS", FONT_SMALL, THEME_SECONDARY_TEXT, panel.w);
  y += S(24);
  const char *limits[] = {
      "Erase / format: unavailable", "Partition / resize: unavailable",
      "Mount / unmount: unavailable", "First Aid / repair: unavailable"};
  for (unsigned i = 0; i < 4; i++) {
    label(panel.x, y, limits[i], FONT_NORMAL, THEME_SECONDARY_TEXT, panel.w);
    y += S(25);
  }
  if (d->inventory.incomplete)
    label(panel.x, y + S(8), "Snapshot incomplete; refresh to retry.",
          FONT_NORMAL, 0xB86B35, panel.w);
  clip_set(saved);
  scrollbar(viewport, content, d->scroll);
}
bool disks_mouse(Window *w, int x, int y, unsigned pressed, int wheel) {
  Disks *d = w->state;
  Rect b = window_body(w);
  int side = sidebar_width(b);
  if (wheel) {
    if (x < b.x + side)
      d->first = clampi(d->first - wheel, 0,
                        MAX(0, d->inventory.count - visible_rows(w)));
    else
      d->scroll = MAX(0, d->scroll - wheel * S(46));
    invalidate_window(w);
    return true;
  }
  if (!(pressed & 1))
    return true;
  if (contains(refresh_button(w), x, y)) {
    refresh(w);
    return true;
  }
  if (x < b.x + side && y >= b.y + sidebar_top(w) &&
      y < b.y + sidebar_top(w) + visible_rows(w) * S(34)) {
    int row = d->first + (y - b.y - sidebar_top(w)) / S(34);
    if (row < d->inventory.count) {
      d->selected = row;
      d->scroll = 0;
      trace("DISKS_SELECT", w, d->inventory.entries[row].name);
      invalidate_window(w);
    }
  }
  return true;
}
void disks_key(Window *w, Key k) {
  if (!k.down)
    return;
  Disks *d = w->state;
  if (k.code == KEY_F5 ||
      ((k.mods & MOD_CTRL) && (k.ascii == 'r' || k.code == 0x15)))
    refresh(w);
  else if (k.code == KEY_UP || k.code == KEY_DOWN) {
    d->selected = clampi(d->selected + (k.code == KEY_DOWN ? 1 : -1), 0,
                         MAX(0, d->inventory.count - 1));
    d->scroll = 0;
    reveal(w);
    if (d->inventory.count)
      trace("DISKS_SELECT", w, d->inventory.entries[d->selected].name);
    invalidate_window(w);
  } else if (k.code == KEY_PAGEUP || k.code == KEY_PAGEDOWN ||
             k.code == KEY_HOME || k.code == KEY_END) {
    d->scroll =
        k.code == KEY_HOME ? 0
        : k.code == KEY_END
            ? S(1000)
            : MAX(0, d->scroll + (k.code == KEY_PAGEDOWN ? 1 : -1) * S(160));
    invalidate_window(w);
  }
}
void disks_toolbar(Window *w) {
  Rect b = w->box;
  int width = text_width("Disks", FONT_BOLD);
  label(b.x + MAX(S(82), (b.w - width) / 2), b.y + S(6), "Disks", FONT_BOLD,
        THEME_PRIMARY_TEXT, MAX(0, b.w - S(210)));
  button(refresh_button(w), "Refresh", I_RELOAD, false, true, false);
}
bool disks_toolbar_mouse(Window *w, int x, int y, unsigned pressed) {
  if ((pressed & 1) && contains(refresh_button(w), x, y)) {
    refresh(w);
    return true;
  }
  return false;
}
Rect disks_toolbar_hover(Window *w, int x, int y) {
  return contains(refresh_button(w), x, y) ? refresh_button(w) : (Rect){0};
}
bool disks_close(Window *w) {
  deallocate(w->state, sizeof(Disks));
  w->state = NULL;
  return true;
}
static void action(Window *w, int item) {
  if (item == 0)
    refresh(w);
  else if (item == 1)
    wm_close(w);
  else
    dialog_notice(
        w, "About Disks",
        "Disks reads the kernel's device and mount inventory. Capacity is the "
        "reported device/partition size, not free filesystem space. No erase, "
        "format, repair, repartition or mount operation is implemented. Arrows "
        "select devices; Page Up/Down scroll details; F5 refreshes.");
}
void disks_menu(Window *w, int group, int x, int y) {
  if (group == MENU_FILE) {
    MenuItem items[] = {{"Refresh inventory\tF5", I_RELOAD, 0, true},
                        {"Close window\tAlt+F4", I_CLOSE, 1, true}};
    menu_open(w, x, y, items, 2, action);
  } else {
    MenuItem items[] = {{"Refresh inventory\tF5", I_RELOAD, 0, true},
                        {"About disk inventory", I_INFO, 2, true}};
    menu_open(w, x, y, items, 2, action);
  }
}
