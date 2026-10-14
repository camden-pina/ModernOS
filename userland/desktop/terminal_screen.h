#ifndef MODERNOS_TERMINAL_SCREEN_H
#define MODERNOS_TERMINAL_SCREEN_H
#include "desktop.h"

#define VT_COLS 240
#define VT_ROWS 100

typedef struct {
    uint16_t ch;
    uint8_t fg, bg, attrs, pad;
} VtCell;

typedef struct {
    VtCell cells[VT_ROWS][VT_COLS], alternate[VT_ROWS][VT_COLS];
    int cols, rows, x, y, saved_x, saved_y, top, bottom, escape, param[16], parameter, utf_left;
    unsigned utf;
    uint8_t fg, bg, attrs;
    bool private_sequence, alternate_screen, wrap, pending_wrap, cursor_visible, application_keys;
} VtScreen;

void vt_init(VtScreen *);
void vt_resize(VtScreen *, int, int);
void vt_byte(VtScreen *, unsigned);
void vt_draw(VtScreen *, Rect, bool focused);
int vt_key(VtScreen *, Key, char *, size_t);
#endif
