#ifndef COAST_CODEOSS_VIEW_H
#define COAST_CODEOSS_VIEW_H
#include "../include/codeoss_protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
static inline bool code_scalar(const char *s, size_t n, size_t *at,
                               uint32_t *cp) {
  if (*at >= n)
    return false;
  size_t p = *at;
  unsigned c = (unsigned char)s[p++], need, min;
  if (c < 128) {
    if (!c)
      return false;
    *cp = c;
    *at = p;
    return true;
  }
  if (c >= 0xc2 && c <= 0xdf) {
    need = 1;
    min = 0x80;
    c &= 31;
  } else if (c >= 0xe0 && c <= 0xef) {
    need = 2;
    min = 0x800;
    c &= 15;
  } else if (c >= 0xf0 && c <= 0xf4) {
    need = 3;
    min = 0x10000;
    c &= 7;
  } else
    return false;
  if (n - p < need)
    return false;
  while (need--) {
    unsigned x = (unsigned char)s[p++];
    if ((x & 0xc0) != 0x80)
      return false;
    c = (c << 6) | (x & 63);
  }
  if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
    return false;
  *cp = c;
  *at = p;
  return true;
}
static inline bool code_utf8(const char *s, size_t n) {
  size_t p = 0;
  uint32_t cp;
  while (p < n)
    if (!code_scalar(s, n, &p, &cp))
      return false;
  return true;
}
static inline bool code_header_valid(const struct code_response *r,
                                     unsigned seq, unsigned action,
                                     unsigned document) {
  uint64_t size = (uint64_t)r->path_bytes + r->text_bytes + r->selection_bytes +
                  r->message_bytes;
  return r->magic == CODE_MAGIC && r->version == CODE_VERSION &&
         r->sequence == seq && r->action == action && r->document == document &&
         document && r->status <= 1 && !(r->flags & ~63u) && r->path_bytes &&
         r->path_bytes < CODE_PATH_LIMIT &&
         r->text_bytes + (r->flags & CODE_BOM ? 3u : 0u) <= CODE_TEXT_LIMIT &&
         r->selection_bytes <= CODE_CLIP_LIMIT &&
         r->message_bytes <= CODE_MESSAGE_LIMIT &&
         size <= CODE_RESPONSE_LIMIT && r->lines &&
         r->lines <= CODE_TEXT_LIMIT + 1 && r->line < r->lines &&
         r->column <= CODE_TEXT_LIMIT * 8 && r->cursor <= CODE_TEXT_LIMIT &&
         r->anchor <= CODE_TEXT_LIMIT && r->revision &&
         (r->eol == 1 || r->eol == 2) &&
         (r->tab == 2 || r->tab == 4 || r->tab == 8);
}
static inline bool code_payload_valid(const struct code_response *r,
                                      const char *p) {
  if (p[0] != '/')
    return false;
  const char *text = p + r->path_bytes;
  size_t at = 0;
  uint32_t cp, units = 0, row = 0, col = 0, cursor_row = 0, cursor_col = 0;
  bool cursor = false, anchor = false;
  if (!code_utf8(p, r->path_bytes) || !code_utf8(text, r->text_bytes) ||
      !code_utf8(text + r->text_bytes, r->selection_bytes) ||
      !code_utf8(text + r->text_bytes + r->selection_bytes, r->message_bytes))
    return false;
  while (at < r->text_bytes) {
    if (units == r->cursor) {
      cursor = true;
      cursor_row = row;
      cursor_col = col;
    }
    if (units == r->anchor)
      anchor = true;
    if (!code_scalar(text, r->text_bytes, &at, &cp))
      return false;
    unsigned count = cp > 65535 ? 2 : 1;
    if (cp == '\r') {
      if (at < r->text_bytes && text[at] == '\n') {
        at++;
        count++;
      }
      row++;
      col = 0;
    } else if (cp == '\n') {
      row++;
      col = 0;
    } else if (cp == '\t')
      col += r->tab - col % r->tab;
    else
      col++;
    units += count;
  }
  if (units == r->cursor) {
    cursor = true;
    cursor_row = row;
    cursor_col = col;
  }
  if (units == r->anchor)
    anchor = true;
  return cursor && anchor && row + 1 == r->lines && cursor_row == r->line &&
         cursor_col == r->column;
}
#endif
