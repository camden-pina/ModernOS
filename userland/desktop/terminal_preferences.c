#include "desktop.h"
Font terminal_font_size(int size) {
  switch (size) {
  case 9:
    return FONT_TERM9;
  case 13:
    return FONT_EDIT;
  case 15:
    return FONT_TERM15;
  case 17:
    return FONT_TERM17;
  case 19:
    return FONT_TERM19;
  default:
    return FONT_MONO;
  }
}
Font terminal_font(void) { return terminal_font_size(desk.terminal_text_size); }
int terminal_line_height(void) { return S(desk.terminal_text_size + 6); }
uint32_t terminal_color_choice(int palette, int override, bool background) {
  static const uint32_t colors[] = {0,        0x111C29, 0xF1EFE6, 0x637384,
                                    0xA8DBC1, 0xF0C77D, 0xA9CCE9};
  static const uint32_t fg[] = {0, 0xE4EAF4, 0x252E3B, 0xD4E8D7, 0xF2E2CC};
  static const uint32_t bg[] = {0, 0x141C2C, 0xF3EFE4, 0x142922, 0x30201F};
  if (override >= 1 && override < 7)
    return colors[override];
  int at = clampi(palette, 0, 4);
  if (!at)
    return background ? theme_color(0xFAFCFB) : THEME_TEXT;
  return background ? bg[at] : fg[at];
}
uint32_t terminal_foreground(void) {
  return terminal_color_choice(desk.terminal_palette, desk.terminal_foreground,
                               false);
}
uint32_t terminal_background(void) {
  return terminal_color_choice(desk.terminal_palette, desk.terminal_background,
                               true);
}
void terminal_preferences_changed(void) {
  desk.material_epoch++;
  if (desk.trace)
    printf("DESKTOP TERMINAL_PREF palette=%d foreground=%d background=%d "
           "text_size=%d opacity=%d shell=%d ansi=%d\n",
           desk.terminal_palette, desk.terminal_foreground,
           desk.terminal_background, desk.terminal_text_size,
           desk.terminal_opacity, desk.terminal_shell, desk.terminal_ansi);
  for (int i = 0; i < MAX_WINDOWS; i++)
    if (desk.windows[i].alive && desk.windows[i].app == APP_TERMINAL)
      invalidate_window(&desk.windows[i]);
}
