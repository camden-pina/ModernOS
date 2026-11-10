#include "terminal_screen.h"
/* Bounded logical-line reflow. Oldest history is discarded when the finite
   scrollback is exhausted. Alternate screens retain fixed-grid semantics. */
#define CAPACITY (VT_HISTORY + VT_ROWS)
typedef struct {
  VtCell row[CAPACITY][VT_COLS];
  bool wrapped[CAPACITY];
  int total;
} Layout;
static VtCell blank_cell(void) { return (VtCell){' ', 16, 16, 0, 0}; }
static void new_row(Layout *p) {
  int at = p->total++ % CAPACITY;
  p->wrapped[at] = false;
  for (int x = 0; x < VT_COLS; x++)
    p->row[at][x] = blank_cell();
}
const VtCell *vt_row(const VtScreen *t, int row) {
  if (row < 0 || row >= t->history_count + t->rows)
    return NULL;
  if (row < t->history_count)
    return t->history[(t->history_start + row) % VT_HISTORY];
  return t->cells[row - t->history_count];
}
bool vt_wrapped(const VtScreen *t, int row) {
  if (row < 0 || row >= t->history_count + t->rows)
    return false;
  return row < t->history_count
             ? t->history_wrapped[(t->history_start + row) % VT_HISTORY]
             : t->wrapped[row - t->history_count];
}
int vt_view_row(const VtScreen *t, int y) {
  return t->history_count - (t->alternate_screen ? 0 : t->view_offset) + y;
}
void vt_scroll(VtScreen *t, int amount) {
  t->view_offset =
      t->alternate_screen
          ? 0
          : clampi((int64_t)t->view_offset + amount, 0, t->history_count);
}
bool vt_reflow(VtScreen *t, int cols, int rows) {
  Layout *p = allocate(sizeof(*p));
  if (!p)
    return false;
  p->total = 0;
  new_row(p);
  int x = 0, cursor_row = 0, cursor_col = 0;
  bool pending = false;
  int last = t->history_count + t->y;
  for (int row = t->history_count; row < t->history_count + t->rows; row++) {
    const VtCell *source = vt_row(t, row);
    for (int col = 0; col < t->cols; col++)
      if ((source[col].ch && source[col].ch != ' ') || source[col].bg != 16) {
        last = MAX(last, row);
        break;
      }
  }
  for (int row = 0; row <= last; row++) {
    const VtCell *source = vt_row(t, row);
    bool soft = vt_wrapped(t, row);
    int length = soft ? t->cols : 0,
        mark = row == t->history_count + t->y
                   ? (t->pending_wrap ? t->cols : t->x)
                   : -1;
    if (!soft)
      for (int col = 0; col < t->cols; col++)
        if ((source[col].ch && source[col].ch != ' ') || source[col].bg != 16)
          length = col + 1;
    length = MAX(length, mark);
    for (int col = 0; col <= length; col++) {
      if (col == mark) {
        cursor_row = p->total - 1;
        cursor_col = MIN(x, cols - 1);
        pending = x == cols;
      }
      if (col == length)
        break;
      if (x == cols) {
        p->wrapped[(p->total - 1) % CAPACITY] = true;
        new_row(p);
        x = 0;
      }
      p->row[(p->total - 1) % CAPACITY][x++] = source[col];
    }
    if (!soft && row < last) {
      new_row(p);
      x = 0;
    }
  }
  int first = MAX(0, p->total - CAPACITY), start = MAX(first, p->total - rows);
  if (cursor_row >= first && cursor_row < start)
    start = cursor_row;
  int history = MIN(VT_HISTORY, start - first), old_view = t->view_offset;
  for (int i = 0; i < history; i++) {
    int at = (start - history + i) % CAPACITY;
    memcpy(t->history[i], p->row[at], sizeof(t->history[i]));
    t->history_wrapped[i] = p->wrapped[at];
  }
  for (int y = 0; y < rows; y++) {
    if (start + y < p->total) {
      int at = (start + y) % CAPACITY;
      memcpy(t->cells[y], p->row[at], sizeof(t->cells[y]));
      t->wrapped[y] = p->wrapped[at];
    } else {
      for (int col = 0; col < VT_COLS; col++)
        t->cells[y][col] = blank_cell();
      t->wrapped[y] = false;
    }
  }
  t->cols = cols;
  t->rows = rows;
  t->history_start = 0;
  t->history_count = history;
  t->view_offset = MIN(old_view, history);
  t->x = cursor_col;
  t->y = clampi(cursor_row - start, 0, rows - 1);
  t->pending_wrap = pending && cursor_row >= start && cursor_row < start + rows;
  t->top = 0;
  t->bottom = rows - 1;
  t->history_generation++;
  deallocate(p, sizeof(*p));
  return true;
}
