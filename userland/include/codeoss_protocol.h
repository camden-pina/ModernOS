#ifndef COAST_CODEOSS_PROTOCOL_H
#define COAST_CODEOSS_PROTOCOL_H
#include <stdint.h>
#define CODE_MAGIC 0x434f5353u
#define CODE_VERSION 2u
#define CODE_TEXT_LIMIT (256u * 1024u)
#define CODE_CLIP_LIMIT (64u * 1024u)
#define CODE_PATH_LIMIT 512u
#define CODE_MESSAGE_LIMIT 1024u
#define CODE_RESPONSE_LIMIT                                                    \
  (CODE_TEXT_LIMIT + CODE_CLIP_LIMIT + CODE_PATH_LIMIT + CODE_MESSAGE_LIMIT)
enum {
  CODE_INIT,
  CODE_INSERT,
  CODE_MOVE,
  CODE_DELETE,
  CODE_UNDO,
  CODE_REDO,
  CODE_SAVE,
  CODE_SAVE_AS,
  CODE_SELECT,
  CODE_SELECT_ALL,
  CODE_COPY,
  CODE_FIND,
  CODE_TABS,
  CODE_RECOVER,
  CODE_QUIT,
  CODE_RELEASE
};
enum { CODE_SHIFT = 1, CODE_CONTROL = 2, CODE_ENTER = 4, CODE_TAB = 8 };
enum {
  CODE_DIRTY = 1,
  CODE_EXISTS = 2,
  CODE_CAN_UNDO = 4,
  CODE_CAN_REDO = 8,
  CODE_BOM = 16,
  CODE_SPACES = 32
};
enum {
  CODE_LEFT,
  CODE_RIGHT,
  CODE_HOME,
  CODE_END,
  CODE_UP,
  CODE_DOWN,
  CODE_PAGE_UP,
  CODE_PAGE_DOWN
};
struct code_request {
  uint32_t magic, version, sequence, action, flags, a, b, c, d, bytes;
};
struct code_response {
  uint32_t magic, version, sequence, action, status, flags, cursor, anchor,
      lines, line, column, revision, path_bytes, text_bytes, selection_bytes,
      message_bytes, eol, tab, document;
};
#if defined(__cplusplus)
static_assert(sizeof(code_request) == 40 && sizeof(code_response) == 76,
              "Code OSS IPC layout");
#else
_Static_assert(sizeof(struct code_request) == 40 &&
                   sizeof(struct code_response) == 76,
               "Code OSS IPC layout");
#endif
#endif
