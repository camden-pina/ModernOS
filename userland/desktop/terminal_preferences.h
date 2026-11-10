#pragma once
#include "desktop.h"
typedef struct { unsigned preferences; } TerminalTag;
int terminal_prefs_init(Window *);
void terminal_prefs_draw(Window *);
bool terminal_prefs_mouse(Window *, int, int, unsigned, int);
void terminal_prefs_key(Window *, Key);
void terminal_prefs_tick(Window *);
bool terminal_prefs_close(Window *);
void terminal_prefs_menu(Window *, int, int, int);
Rect terminal_prefs_hover(Window *, int, int);
