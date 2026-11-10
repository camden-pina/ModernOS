#ifndef COAST_TERMINAL_SCREEN_H
#define COAST_TERMINAL_SCREEN_H
#include "desktop.h"
#define VT_COLS 240
#define VT_ROWS 100
#define VT_HISTORY 512
typedef struct {
  uint32_t ch;
  uint8_t fg, bg, attrs, pad;
} VtCell;
typedef struct {
  VtCell cells[VT_ROWS][VT_COLS], alternate[VT_ROWS][VT_COLS],
      history[VT_HISTORY][VT_COLS];
  bool wrapped[VT_ROWS], alt_wrapped[VT_ROWS], history_wrapped[VT_HISTORY];
  int history_start, history_count, view_offset;
  unsigned history_generation;
  int cols, rows, x, y, saved_x, saved_y, top, bottom, escape, param[16],
      parameter, utf_left;
  int normal_x, normal_y, normal_cols, normal_rows;
  uint8_t normal_fg, normal_bg, normal_attrs;
  bool normal_pending;
  unsigned utf;
  uint8_t fg, bg, attrs;
  bool private_sequence, alternate_screen, wrap, pending_wrap, cursor_visible,
      application_keys, bracketed_paste;
  bool resize_failed;
  char response[128];
  unsigned response_size;
} VtScreen;
void vt_init(VtScreen *);
bool vt_resize(VtScreen *, int, int);
bool vt_reflow(VtScreen *, int, int);
void vt_byte(VtScreen *, unsigned);
void vt_draw(VtScreen *, Rect, bool);
size_t vt_response(VtScreen *, char *, size_t);
int vt_key(VtScreen *, Key, char *, size_t);
const VtCell *vt_row(const VtScreen *, int);
bool vt_wrapped(const VtScreen *, int);
int vt_view_row(const VtScreen *, int);
void vt_scroll(VtScreen *, int);
#endif
