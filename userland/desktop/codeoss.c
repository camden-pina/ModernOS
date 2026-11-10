#include "codeoss_view.h"
#include "desktop.h"
#include <uapi/clipboard.h>
#define CODE_QUEUE 64
#define CODE_QUEUE_BYTES (256 * 1024)
#define CODE_SERVICE_BYTES (1024 * 1024)
typedef struct {
  struct code_request packet;
  char *text;
  bool cut;
} CodeInput;
typedef struct {
  int pid, input, output, errors;
  bool ready, failed, pending, allow_close, want_close, selecting, input_error,
      recovering, model_live, closing;
  unsigned first, queued, queue_bytes, sequence;
  CodeInput queue[CODE_QUEUE], active;
  size_t sent, header_read, payload_read;
  struct code_response response, current;
  char *payload;
  char text[CODE_TEXT_LIMIT + 1], path[CODE_PATH_LIMIT],
      message[CODE_MESSAGE_LIMIT + 1], find[256];
  uint64_t started, sent_at, max_tick_ms;
  unsigned top_line, horizontal, font_size, drag_anchor, view_rows, view_cols;
} Code;
/* One ordered channel. Per-window queues/models/caches remain separate. */
static struct {
  int pid, input, output, errors;
  Window *owner;
  unsigned sequence, next_window, bytes, generation;
  bool warm;
  uint64_t max_cold_ms, max_warm_ms;
} service = {.input = -1, .output = -1, .errors = -1};
static struct {
  int pid, errors;
  uint64_t at;
} retired[64];
static unsigned retired_count;
static void code_command(Window *, int);
static bool enqueue(Window *, unsigned, unsigned, unsigned, unsigned, unsigned,
                    unsigned, const char *, size_t, bool, bool);
static void failed(Window *, const char *);
static Font view_font(Code *c) {
  return c->font_size == 11 ? FONT_MONO : FONT_EDIT;
}
static int row_height(Code *c) { return S(c->font_size + 6); }
static unsigned rows(Window *w);
static void message(Window *w, const char *s) {
  Code *c = w->state;
  copy_text(c->message, sizeof(c->message), s);
  invalidate_window(w);
}
static unsigned toolbar(Window *w, int wanted, Rect *out) {
  Rect b = window_body(w);
  int x = S(12), y = S(8), right = b.w - S(12);
  static const int width[] = {82, 64, 62, 84, 62, 62, 62,
                              62, 62, 64, 76, 96, 108};
  for (int id = 0; id < 13; id++) {
    int size = S(width[id]);
    if (id == 7 || x + size > right) {
      x = S(12);
      y += S(32);
    }
    if (id == wanted && out)
      *out = (Rect){b.x + x, b.y + y, size, S(27)};
    x += size + S(5);
  }
  return y + S(51);
}
static Rect viewport(Window *w) {
  Rect b = window_body(w);
  unsigned top = toolbar(w, -1, NULL);
  return (Rect){b.x + S(12), b.y + (int)top + S(10), b.w - S(24),
                MAX(1, b.h - (int)top - S(48))};
}
static unsigned rows(Window *w) {
  return MAX(1, viewport(w).h / row_height(w->state));
}
static bool selected(Code *c) { return c->current.cursor != c->current.anchor; }
static bool enabled(Code *c, int id) {
  if (id <= 1)
    return true;
  if (id == 11)
    return true;
  if (c->failed || !c->ready || c->closing)
    return false;
  if (id == 2)
    return (c->current.flags & CODE_DIRTY) || !(c->current.flags & CODE_EXISTS);
  if (id == 4)
    return c->current.flags & CODE_CAN_UNDO;
  if (id == 5)
    return c->current.flags & CODE_CAN_REDO;
  if (id == 7 || id == 8)
    return selected(c);
  return true;
}
static void free_input(CodeInput *q) {
  if (q->text) {
    service.bytes -= q->packet.bytes;
    deallocate(q->text, MAX(1u, q->packet.bytes));
  }
  memset(q, 0, sizeof(*q));
}
static void clear_queue(Code *c) {
  free_input(&c->active);
  for (unsigned i = 0; i < c->queued; i++)
    free_input(&c->queue[(c->first + i) % CODE_QUEUE]);
  c->first = c->queued = c->queue_bytes = 0;
  c->pending = false;
  c->sent = c->header_read = c->payload_read = 0;
}
static void drain_errors(int fd) {
  char data[1024];
  if (fd < 0)
    return;
  for (int i = 0; i < 4; i++) {
    long n = read(fd, data, sizeof(data));
    if (n <= 0)
      break;
    write_all(2, data, n);
  }
}
static void account_service(uint64_t begin, bool warm) {
  uint64_t elapsed = now_ms() - begin;
  if (warm)
    service.max_warm_ms = MAX(service.max_warm_ms, elapsed);
  else
    service.max_cold_ms = MAX(service.max_cold_ms, elapsed);
  if (desk.trace && elapsed > 50)
    printf("DESKTOP CODE_SERVICE_SLOW generation=%u warm=%u elapsed_ms=%lu\n",
           service.generation, warm, elapsed);
}
void codeoss_reap(void) {
  uint64_t begin = now_ms();
  bool warm = service.warm;
  if (service.pid > 0) {
    drain_errors(service.errors);
    int status = 0, r = waitpid(service.pid, &status, 1);
    if (r > 0 || r == -E_CHILD) {
      service.pid = 0;
      failed(NULL, "Shared core exited. Each window retains its last "
                   "acknowledged text; Recover creates a new copy.");
    }
  }
  for (unsigned i = 0; i < retired_count;) {
    drain_errors(retired[i].errors);
    int status = 0;
    int r = waitpid(retired[i].pid, &status, 1);
    if (r > 0 || r == -E_CHILD) {
      if (retired[i].errors >= 0)
        close(retired[i].errors);
      retired[i] = retired[--retired_count];
      continue;
    }
    if (now_ms() - retired[i].at > 4000)
      call2(SYS_kill, retired[i].pid, 9);
    else if (now_ms() - retired[i].at > 2000)
      call2(SYS_kill, retired[i].pid, 15);
    i++;
  }
  account_service(begin, warm);
}
static void stop_worker(void) {
  if (service.input >= 0)
    close(service.input);
  if (service.output >= 0)
    close(service.output);
  service.input = service.output = -1;
  service.owner = NULL;
  if (service.pid > 0) {
    if (retired_count < 64) {
      retired[retired_count].pid = service.pid;
      retired[retired_count].errors = service.errors;
      retired[retired_count++].at = now_ms();
      service.errors = -1;
    } else
      call2(SYS_kill, service.pid, 15);
  }
  if (service.errors >= 0)
    close(service.errors);
  service.errors = -1;
  service.pid = 0;
  service.warm = false;
}
void codeoss_shutdown(void) {
  stop_worker();
  uint64_t until = now_ms() + 6000;
  while (retired_count && now_ms() < until) {
    codeoss_reap();
    struct os_timespec delay = {0, 5000000};
    call2(SYS_nanosleep, &delay, 0);
  }
  if (desk.trace)
    printf("DESKTOP CODE_SERVICE_STATS generations=%u max_cold_ms=%lu "
           "max_warm_ms=%lu unreaped=%u queued_bytes=%u\n",
           service.generation, service.max_cold_ms, service.max_warm_ms,
           retired_count, service.bytes);
}
static int start_worker(Window *w) {
  Code *c = w->state;
  codeoss_reap();
  c->started = now_ms();
  if (service.pid > 0) {
    c->pid = service.pid;
    c->input = service.input;
    c->output = service.output;
    c->errors = service.errors;
    c->ready = c->failed = c->pending = c->closing = false;
    c->header_read = c->payload_read = c->sent = 0;
    trace("CODE_CORE_REUSE", w, "warm shared native text core");
    return 0;
  }
  if (retired_count >= 64 - MAX_WINDOWS)
    return -E_AGAIN;
  int in[2] = {-1, -1}, out[2] = {-1, -1}, errors[2] = {-1, -1};
  int e = call2(SYS_pipe2, in, O_CLOEXEC);
  if (!e)
    e = call2(SYS_pipe2, out, O_CLOEXEC);
  if (!e)
    e = call2(SYS_pipe2, errors, O_CLOEXEC);
  if (!e) {
    char *args[] = {"/bin/codeoss-launch", NULL};
    struct os_spawn_io spec = {.fd = {in[0], out[1], errors[1]},
                               .cwd = (uintptr_t)desk.home};
    service.pid = call3(SYS_spawn_io, args[0], args, &spec);
    if (service.pid < 0)
      e = service.pid;
  }
  if (in[0] >= 0)
    close(in[0]);
  if (out[1] >= 0)
    close(out[1]);
  if (errors[1] >= 0)
    close(errors[1]);
  if (e) {
    if (in[1] >= 0)
      close(in[1]);
    if (out[0] >= 0)
      close(out[0]);
    if (errors[0] >= 0)
      close(errors[0]);
    service.pid = 0;
    return e;
  }
  service.input = in[1];
  service.output = out[0];
  service.errors = errors[0];
  c->pid = service.pid;
  c->input = service.input;
  c->output = service.output;
  c->errors = service.errors;
  long mode = call3(SYS_fcntl, c->input, F_SETFL, O_WRONLY | O_NONBLOCK);
  if (mode >= 0)
    mode = call3(SYS_fcntl, c->output, F_SETFL, O_NONBLOCK);
  if (mode >= 0)
    mode = call3(SYS_fcntl, c->errors, F_SETFL, O_NONBLOCK);
  if (mode < 0) {
    stop_worker();
    return (int)mode;
  }
  c->ready = false;
  c->failed = false;
  c->pending = false;
  service.sequence = 0;
  service.owner = NULL;
  service.generation++;
  service.warm = false;
  c->closing = false;
  c->started = now_ms();
  c->header_read = c->payload_read = c->sent = 0;
  trace("CODE_CORE_START", w,
        "cold shared JavaScriptCore service; upstream PieceTree");
  return 0;
}
static void failed(Window *w, const char *why) {
  (void)w;
  stop_worker();
  for (int i = 0; i < MAX_WINDOWS; i++) {
    Window *item = &desk.windows[i];
    if (!item->alive || item->app != APP_CODE || !item->state)
      continue;
    Code *c = item->state;
    bool saving = c->pending && (c->active.packet.action == CODE_SAVE ||
                                 c->active.packet.action == CODE_SAVE_AS);
    clear_queue(c);
    c->failed = true;
    c->ready = false;
    c->selecting = false;
    c->want_close = false;
    c->model_live = false;
    c->closing = false;
    c->pid = 0;
    c->input = c->output = c->errors = -1;
    copy_text(
        c->message, sizeof(c->message),
        saving
            ? "Shared core ended during Save; the save outcome is uncertain. "
              "Last acknowledged text is retained. Recover creates a new copy."
            : why);
    invalidate_window(item);
    trace("CODE_CORE_ERROR", item,
          saving ? "save outcome uncertain; snapshot retained" : why);
  }
}
static Window *next_document(void) {
  for (unsigned n = 0; n < MAX_WINDOWS; n++) {
    unsigned at = (service.next_window + n) % MAX_WINDOWS;
    Window *w = &desk.windows[at];
    if (w->alive && w->app == APP_CODE && w->state) {
      Code *c = w->state;
      if (!c->failed && c->queued) {
        service.next_window = (at + 1) % MAX_WINDOWS;
        return w;
      }
    }
  }
  return NULL;
}
static void flush(Window *w) {
  Code *c = w->state;
  if (c->failed || service.input < 0)
    return;
  if (!service.owner)
    service.owner = next_document();
  if (service.owner != w)
    return;
  if (!c->pending && c->queued) {
    c->active = c->queue[c->first];
    memset(&c->queue[c->first], 0, sizeof(CodeInput));
    c->first = (c->first + 1) % CODE_QUEUE;
    c->queued--;
    c->queue_bytes -= c->active.packet.bytes;
    c->active.packet.magic = CODE_MAGIC;
    c->active.packet.version = CODE_VERSION;
    c->active.packet.sequence = ++service.sequence;
    c->active.packet.c = w->id;
    if (!service.sequence) {
      failed(w, "Core sequence exhausted. Recover the last acknowledged text.");
      return;
    }
    c->pending = true;
    c->sent = 0;
    c->sent_at = now_ms();
  }
  if (!c->pending)
    return;
  size_t total = sizeof(struct code_request) + c->active.packet.bytes;
  for (int pass = 0; pass < 2 && c->sent < total; pass++) {
    const void *p;
    size_t n;
    if (c->sent < sizeof(struct code_request)) {
      p = (char *)&c->active.packet + c->sent;
      n = sizeof(struct code_request) - c->sent;
    } else {
      p = c->active.text + c->sent - sizeof(struct code_request);
      n = total - c->sent;
    }
    n = MIN(n, (size_t)16384);
    long sent = write(c->input, p, n);
    if (sent == -E_AGAIN || sent == -E_INTR)
      return;
    if (sent <= 0 || (size_t)sent > n) {
      failed(w, "Core input stopped. Recover the last acknowledged text.");
      return;
    }
    c->sent += sent;
  }
}
static bool enqueue(Window *w, unsigned action, unsigned flags, unsigned a,
                    unsigned b, unsigned d, unsigned local, const char *text,
                    size_t n, bool cut, bool front) {
  (void)local;
  Code *c = w->state;
  if (c->failed || c->closing)
    return false;
  if (!c->ready && action != CODE_INIT && action != CODE_RECOVER) {
    message(
        w,
        "Starting Code OSS Core Preview. Wait for the document before typing.");
    return false;
  }
  if (n > CODE_TEXT_LIMIT || c->queue_bytes + n > CODE_QUEUE_BYTES ||
      c->queued >= CODE_QUEUE || service.bytes + n > CODE_SERVICE_BYTES) {
    c->input_error = true;
    message(w, "Core input queue is full. Accepted actions are retained; wait "
               "and retry the last input.");
    return false;
  }
  // Consecutive unsent typed text is one ordered edit; no model runs in the
  // GUI.
  if (!front && action == CODE_INSERT && !flags && n && c->queued) {
    CodeInput *last = &c->queue[(c->first + c->queued - 1) % CODE_QUEUE];
    if (last->packet.action == CODE_INSERT && !last->packet.flags &&
        last->packet.bytes + n <= CODE_CLIP_LIMIT) {
      size_t count = last->packet.bytes + n;
      char *p = allocate(count);
      if (!p) {
        message(w, "Cannot queue text: out of memory. Document is unchanged.");
        return false;
      }
      memcpy(p, last->text, last->packet.bytes);
      memcpy(p + last->packet.bytes, text, n);
      deallocate(last->text, MAX(1u, last->packet.bytes));
      last->text = p;
      last->packet.bytes = count;
      c->queue_bytes += n;
      service.bytes += n;
      flush(w);
      return true;
    }
  }
  if (!front && action == CODE_SELECT && c->queued) {
    CodeInput *last = &c->queue[(c->first + c->queued - 1) % CODE_QUEUE];
    if (last->packet.action == CODE_SELECT && last->packet.b == b &&
        last->packet.d == d) {
      last->packet.a = a;
      flush(w);
      return true;
    }
  }
  char *payload = NULL;
  if (n) {
    payload = allocate(n);
    if (!payload) {
      message(w, "Cannot queue action: out of memory. Document is unchanged.");
      return false;
    }
    memcpy(payload, text, n);
  }
  unsigned at;
  if (front) {
    c->first = (c->first + CODE_QUEUE - 1) % CODE_QUEUE;
    at = c->first;
  } else
    at = (c->first + c->queued) % CODE_QUEUE;
  c->queue[at] = (CodeInput){.packet = {.action = action,
                                        .flags = flags,
                                        .a = a,
                                        .b = b,
                                        .d = d,
                                        .bytes = n},
                             .text = payload,
                             .cut = cut};
  c->queued++;
  c->queue_bytes += n;
  service.bytes += n;
  flush(w);
  invalidate_window(w);
  return true;
}
static bool send(Window *w, unsigned action, unsigned flags, unsigned a,
                 unsigned b, unsigned d, const char *text, size_t n) {
  return enqueue(w, action, flags, a, b, d, 0, text, n, false, false);
}
static void title(Window *w) {
  Code *c = w->state;
  copy_text(w->title, sizeof(w->title),
            (c->current.flags & CODE_EXISTS) ? basename(c->path)
            : c->recovering                  ? "Recovered copy"
                                             : "Untitled");
  append_text(w->title, sizeof(w->title),
              c->current.flags & CODE_DIRTY ? " * - Code OSS Core Preview"
                                            : " - Code OSS Core Preview");
}
static void ensure_cursor(Window *w) {
  Code *c = w->state;
  unsigned visible = rows(w);
  if (c->current.line < c->top_line)
    c->top_line = c->current.line;
  else if (c->current.line >= c->top_line + visible)
    c->top_line = c->current.line - visible + 1;
  int advance = MAX(1, text_width("M", view_font(c)));
  unsigned available = MAX(1, (viewport(w).w - S(52)) / advance);
  if (c->current.column < c->horizontal)
    c->horizontal = c->current.column;
  else if (c->current.column >= c->horizontal + available)
    c->horizontal = c->current.column - available + 1;
}
static bool receive(Window *w) {
  Code *c = w->state;
  struct code_response *r = &c->response;
  bool cut = c->active.cut;
  unsigned action = c->active.packet.action;
  if (!code_payload_valid(r, c->payload)) {
    failed(w, "Invalid model snapshot. Last acknowledged text is retained for "
              "recovery.");
    return false;
  }
  memcpy(c->path, c->payload, r->path_bytes);
  c->path[r->path_bytes] = 0;
  memcpy(c->text, c->payload + r->path_bytes, r->text_bytes);
  c->text[r->text_bytes] = 0;
  memcpy(c->message,
         c->payload + r->path_bytes + r->text_bytes + r->selection_bytes,
         r->message_bytes);
  c->message[r->message_bytes] = 0;
  c->current = *r;
  if (action == CODE_COPY && !r->status) {
    const char *selected_text = c->payload + r->path_bytes + r->text_bytes;
    int e = clipboard_write(selected_text, r->selection_bytes);
    if (e)
      clipboard_error(w, "Copy from Code OSS Core", e);
    else if (cut && r->cursor != r->anchor) {
      if (!enqueue(w, CODE_DELETE, 0, 0, 0, r->revision, 0, NULL, 0, false,
                   true))
        message(w, "Text copied, but Cut could not be queued. The document is "
                   "unchanged.");
    }
    trace("CODE_CORE_COPY", w,
          e ? "clipboard rejected; no deletion"
            : "copied acknowledged selection");
  }
  bool initial = !c->ready;
  c->ready = true;
  c->header_read = c->payload_read = 0;
  free_input(&c->active);
  c->pending = false;
  c->sent = 0;
  service.owner = NULL;
  if (action == CODE_INIT || action == CODE_RECOVER) {
    c->model_live = true;
    service.warm = true;
  }
  if (action == CODE_RELEASE) {
    c->model_live = false;
    c->want_close = true;
  }
  if (initial) {
    c->top_line = c->horizontal = 0;
    if (desk.trace)
      printf("DESKTOP CODE_CORE_READY window=%d pid=%d latency_ms=%lu "
             "text_bytes=%u\n",
             w->id, c->pid, now_ms() - c->started, r->text_bytes);
  }
  if (action != CODE_COPY && action != CODE_SAVE && action != CODE_SAVE_AS &&
      action != CODE_TABS)
    ensure_cursor(w);
  title(w);
  invalidate_window(w);
  if (desk.trace)
    printf("DESKTOP CODE_CORE_ACK window=%d sequence=%u action=%u error=%u "
           "revision=%u bytes=%u cursor=%u anchor=%u flags=%u latency_ms=%lu\n",
           w->id, r->sequence, r->action, r->status, r->revision, r->text_bytes,
           r->cursor, r->anchor, r->flags, now_ms() - c->sent_at);
  return true;
}
int codeoss_open(Window *w, const char *path) {
  Code *c = allocate(sizeof(*c));
  if (!c)
    return -E_NOMEM;
  memset(c, 0, sizeof(*c));
  c->pid = 0;
  c->input = c->output = c->errors = -1;
  c->font_size = desk.editor_text_size == 11 ? 11 : 13;
  c->current.lines = 1;
  c->current.tab = 4;
  c->current.eol = 1;
  c->current.flags = CODE_SPACES;
  c->payload = allocate(CODE_RESPONSE_LIMIT);
  if (!c->payload) {
    deallocate(c, sizeof(*c));
    return -E_NOMEM;
  }
  w->state = c;
  copy_text(c->message, sizeof(c->message),
            "Starting Code OSS Core Preview...");
  int e = start_worker(w);
  if (e) {
    deallocate(c->payload, CODE_RESPONSE_LIMIT);
    deallocate(c, sizeof(*c));
    w->state = NULL;
    return e;
  }
  send(w, CODE_INIT, 0, 0, 0, 0, path ? path : "", path ? strlen(path) : 0);
  return 0;
}
void codeoss_tick(Window *w) {
  Code *c = w->state;
  uint64_t begin = now_ms();
  uint64_t reaped = now_ms();
  if (c->failed)
    return;
  bool warm = service.warm;
  flush(w);
  uint64_t flushed = now_ms();
  bool stop = false;
  for (int pass = 0; pass < 12 && !c->failed; pass++) {
    if (!c->pending || service.owner != w)
      break;
    if (c->header_read < sizeof(c->response)) {
      long n = read(c->output, (char *)&c->response + c->header_read,
                    sizeof(c->response) - c->header_read);
      if (n == -E_AGAIN || n == -E_INTR)
        break;
      if (n <= 0) {
        stop = true;
        break;
      }
      c->header_read += n;
      if (c->header_read < sizeof(c->response))
        continue;
      if (c->sent != sizeof(struct code_request) + c->active.packet.bytes ||
          !code_header_valid(&c->response, c->active.packet.sequence,
                             c->active.packet.action, w->id)) {
        failed(w,
               "Invalid model response. Last acknowledged text is retained.");
        break;
      }
    }
    size_t total = (size_t)c->response.path_bytes + c->response.text_bytes +
                   c->response.selection_bytes + c->response.message_bytes;
    if (c->payload_read < total) {
      size_t bytes = MIN(total - c->payload_read, (size_t)16384);
      long n = read(c->output, c->payload + c->payload_read, bytes);
      if (n == -E_AGAIN || n == -E_INTR)
        break;
      if (n <= 0) {
        stop = true;
        break;
      }
      c->payload_read += n;
      if (c->payload_read < total)
        continue;
    }
    if (!receive(w))
      break;
    flush(w);
  }
  uint64_t received = now_ms();
  if (stop && !c->failed)
    failed(w, "Core connection ended. Recover keeps the last acknowledged text "
              "as a new unsaved copy.");
  uint64_t waited = now_ms();
  unsigned vr = rows(w),
           vc = MAX(1, viewport(w).w / MAX(1, text_width("M", view_font(c))));
  if (c->ready && (vr != c->view_rows || vc != c->view_cols)) {
    c->view_rows = vr;
    c->view_cols = vc;
    ensure_cursor(w);
    invalidate_window(w);
  }
  if (c->selecting && !(desk.buttons & 1))
    c->selecting = false;
  uint64_t elapsed = now_ms() - begin;
  c->max_tick_ms = MAX(c->max_tick_ms, elapsed);
  account_service(begin, warm);
  if (desk.trace && elapsed > 50)
    printf("DESKTOP CODE_CORE_SLOW window=%d elapsed=%lu reap=%lu flush=%lu "
           "receive=%lu wait=%lu layout=%lu\n",
           w->id, elapsed, reaped - begin, flushed - reaped, received - flushed,
           waited - received, now_ms() - waited);
  if (c->want_close && !c->pending && !c->queued) {
    c->want_close = false;
    wm_close(w);
    return;
  }
}
static size_t line_start(Code *c, unsigned line, unsigned *units) {
  size_t p = 0;
  unsigned row = 0, u = 0;
  while (p < c->current.text_bytes && row < line) {
    uint32_t cp;
    if (!code_scalar(c->text, c->current.text_bytes, &p, &cp))
      break;
    u += cp > 65535 ? 2 : 1;
    if (cp == '\r') {
      if (p < c->current.text_bytes && c->text[p] == '\n') {
        p++;
        u++;
      }
      row++;
    } else if (cp == '\n')
      row++;
  }
  *units = u;
  return p;
}
static size_t line_end(Code *c, size_t p) {
  while (p < c->current.text_bytes && c->text[p] != '\r' && c->text[p] != '\n')
    p++;
  return p;
}
static size_t next_line(Code *c, size_t p, unsigned *units) {
  while (p < c->current.text_bytes) {
    uint32_t cp;
    if (!code_scalar(c->text, c->current.text_bytes, &p, &cp))
      break;
    *units += cp > 65535 ? 2 : 1;
    if (cp == '\r') {
      if (p < c->current.text_bytes && c->text[p] == '\n') {
        p++;
        (*units)++;
      }
      break;
    }
    if (cp == '\n')
      break;
  }
  return p;
}
void codeoss_draw(Window *w) {
  Code *c = w->state;
  Rect b = window_body(w), view = viewport(w), saved = desk.clip;
  fill(b, THEME_WINDOW);
  const char *names[] = {c->failed ? "Recover" : "New",
                         "Open...",
                         "Save",
                         "Save As...",
                         "Undo",
                         "Redo",
                         "Find...",
                         "Copy",
                         "Cut",
                         "Paste",
                         "Select all",
                         c->font_size == 11 ? "Text 11 px" : "Text 13 px",
                         ""};
  char tabs[32], n[24];
  copy_text(tabs, sizeof(tabs),
            c->current.flags & CODE_SPACES ? "Spaces " : "Tabs ");
  format_uint(n, c->current.tab);
  append_text(tabs, sizeof(tabs), n);
  for (int id = 0; id < 13; id++) {
    Rect r;
    toolbar(w, id, &r);
    button(r, id == 12 ? tabs : names[id], I_COUNT, id == 2, enabled(c, id),
           false);
  }
  unsigned bottom = toolbar(w, -1, NULL);
  label(b.x + S(14), b.y + (int)bottom - S(18),
        c->ready ? c->path : "Code OSS Core Preview - starting...", FONT_SMALL,
        THEME_SECONDARY_TEXT, b.w - S(28));
  fill((Rect){b.x, b.y + (int)bottom, b.w, 1}, THEME_DIVIDER);
  if (c->current.lines < 1)
    c->current.lines = 1;
  unsigned visible = rows(w);
  c->top_line = MIN(
      c->top_line, c->current.lines > visible ? c->current.lines - visible : 0);
  clip_set(intersect(saved, view));
  unsigned units = 0, line = c->top_line;
  size_t at = line_start(c, line, &units);
  int cw = MAX(1, text_width("M", view_font(c))), lh = row_height(c);
  unsigned lo = MIN(c->current.cursor, c->current.anchor),
           hi = MAX(c->current.cursor, c->current.anchor);
  for (unsigned row = 0; row <= visible && line < c->current.lines;
       row++, line++) {
    int y = view.y + row * lh;
    char num[32];
    format_uint(num, line + 1);
    label(view.x, y, num, FONT_SMALL, THEME_MUTED_TEXT, S(40));
    Rect text_view = {view.x + S(46), view.y, view.w - S(46), view.h};
    clip_set(intersect(saved, text_view));
    if (line == c->current.line)
      blend((Rect){text_view.x, y, text_view.w, lh}, desk.accent,
            desk.dark_mode ? 20 : 10);
    size_t end = line_end(c, at), p = at;
    unsigned col = 0, u = units;
    while (p < end) {
      size_t first = p;
      uint32_t cp;
      if (!code_scalar(c->text, end, &p, &cp))
        break;
      unsigned width = cp == '\t' ? c->current.tab - col % c->current.tab : 1,
               count = cp > 65535 ? 2 : 1;
      int x = text_view.x + ((int64_t)col - (int64_t)c->horizontal) * cw;
      if (x > text_view.x + text_view.w)
        break;
      if (u < hi && u + count > lo)
        fill((Rect){x, y, (int)width * cw, lh}, desk.selection);
      if (cp != '\t' && x + (int)width * cw > text_view.x) {
        char symbol[5];
        size_t bytes = p - first;
        if (cp < 32) {
          symbol[0] = '?';
          bytes = 1;
        } else
          memcpy(symbol, c->text + first, bytes);
        symbol[bytes] = 0;
        label(x, y, symbol, view_font(c), THEME_TEXT, cw + S(2));
      }
      col += width;
      u += count;
    }
    if (wm_focused() == w && c->ready && !c->failed &&
        line == c->current.line) {
      int x = text_view.x + ((int64_t)c->current.column - c->horizontal) * cw;
      fill((Rect){x, y + S(1), MAX(1, S(1)), lh - S(2)}, desk.accent);
    }
    clip_set(intersect(saved, view));
    at = next_line(c, at, &units);
  }
  desk.clip = saved;
  scrollbar(view, (int64_t)c->current.lines * lh, (int64_t)c->top_line * lh);
  char status[256];
  if (c->input_error)
    copy_text(status, sizeof(status),
              "Input queue was full: wait and retry rejected input. Esc clears "
              "this notice.");
  else if (c->message[0])
    copy_text(status, sizeof(status), c->message);
  else {
    copy_text(status, sizeof(status), "Ln ");
    format_uint(n, c->current.line + 1);
    append_text(status, sizeof(status), n);
    append_text(status, sizeof(status), ", Col ");
    format_uint(n, c->current.column + 1);
    append_text(status, sizeof(status), n);
    append_text(status, sizeof(status),
                c->current.flags & CODE_BOM ? "  |  UTF-8 BOM" : "  |  UTF-8");
    append_text(status, sizeof(status),
                c->current.eol == 2 ? "  |  CRLF" : "  |  LF");
    append_text(status, sizeof(status),
                c->current.flags & CODE_DIRTY ? "  |  Unsaved" : "");
    if (c->pending || c->queued)
      append_text(status, sizeof(status), "  |  Working...");
  }
  label(b.x + S(14), b.y + b.h - S(23), status, FONT_SMALL,
        c->failed ? desk.accent : THEME_SECONDARY_TEXT, b.w - S(28));
}
static unsigned hit_offset(Window *w, int x, int y) {
  Code *c = w->state;
  Rect view = viewport(w);
  unsigned row = c->top_line +
                 clampi((y - view.y) / row_height(c), 0, rows(w) - 1),
           units = 0;
  size_t p = line_start(c, row, &units), end = line_end(c, p);
  int cw = MAX(1, text_width("M", view_font(c)));
  unsigned target = MAX(0, (x - view.x - S(46) + cw / 2) / cw) +
                    (unsigned)c->horizontal,
           col = 0;
  while (p < end) {
    size_t next = p;
    uint32_t cp;
    if (!code_scalar(c->text, end, &next, &cp))
      break;
    unsigned width = cp == '\t' ? c->current.tab - col % c->current.tab : 1;
    if (target * 2 < col * 2 + width)
      break;
    col += width;
    units += cp > 65535 ? 2 : 1;
    p = next;
  }
  return units;
}
static void select_at(Window *w, int x, int y) {
  Code *c = w->state;
  unsigned pos = hit_offset(w, x, y);
  send(w, CODE_SELECT, 0, pos, c->drag_anchor, c->current.revision, NULL, 0);
}
bool codeoss_mouse(Window *w, int x, int y, unsigned pressed, int wheel) {
  Code *c = w->state;
  Rect view = viewport(w);
  if (wheel && contains(view, x, y)) {
    if (desk.modifiers & MOD_SHIFT)
      c->horizontal =
          clampi((int64_t)c->horizontal - wheel * 6, 0, CODE_TEXT_LIMIT * 8);
    else
      c->top_line = clampi((int64_t)c->top_line - wheel * 3, 0,
                           MAX(0, (int64_t)c->current.lines - rows(w)));
    invalidate_window(w);
    return true;
  }
  if (pressed & 1) {
    for (int id = 0; id < 13; id++) {
      Rect r;
      toolbar(w, id, &r);
      if (contains(r, x, y)) {
        if (enabled(c, id))
          code_command(w, id);
        return true;
      }
    }
    if (c->ready && !c->failed && contains(view, x, y)) {
      c->selecting = true;
      c->drag_anchor = (desk.modifiers & MOD_SHIFT) ? c->current.anchor
                                                    : hit_offset(w, x, y);
      select_at(w, x, y);
      return true;
    }
  }
  if (c->selecting) {
    if (desk.buttons & 1)
      select_at(w, x, y);
    else
      c->selecting = false;
    return true;
  }
  return contains(view, x, y);
}
Rect codeoss_hover(Window *w, int x, int y) {
  for (int id = 0; id < 13; id++) {
    Rect r;
    toolbar(w, id, &r);
    if (contains(r, x, y))
      return r;
  }
  return (Rect){0};
}
bool codeoss_text_hit(Window *w, int x, int y) {
  Rect r = viewport(w);
  r.x += S(46); r.w -= S(46);
  return contains(r, x, y);
}
static void open_file(Window *w, const char *input) {
  char path[OS_PATH_MAX];
  int e = normalize_path(path, desk.home, input);
  if (e)
    dialog_error(w, "Open in Code OSS Core", e);
  else
    wm_open(APP_CODE, path);
}
static void save_as(Window *w, const char *input) {
  char path[OS_PATH_MAX];
  int e = normalize_path(path, desk.home, input);
  if (e)
    dialog_error(w, "Save As", e);
  else
    send(w, CODE_SAVE_AS, 0, 0, 0, 0, path, strlen(path));
}
static void save(Window *w, bool as) {
  Code *c = w->state;
  if (!c->ready || c->failed || c->closing)
    return;
  if (as || !(c->current.flags & CODE_EXISTS))
    dialog_input(w, "Save As",
                 "Create a new file. Existing destinations are preserved; "
                 "choose a new name.",
                 c->recovering ? "Recovered.txt" : "", save_as);
  else
    send(w, CODE_SAVE, 0, 0, 0, 0, NULL, 0);
}
static void find(Window *w, const char *query) {
  Code *c = w->state;
  copy_text(c->find, sizeof(c->find), query);
  send(w, CODE_FIND, 0, 0, 0, 0, c->find, strlen(c->find));
}
static void recover(Window *w) {
  Code *c = w->state;
  if (!c->failed)
    return;
  unsigned cursor = c->current.cursor, flags = c->current.flags & CODE_BOM;
  int e = start_worker(w);
  if (e) {
    c->failed = true;
    dialog_error(w, "Recover core", e);
    return;
  }
  c->recovering = true;
  send(w, CODE_RECOVER, flags, cursor, 0, 0, c->text, c->current.text_bytes);
  message(w, "Recovering the last acknowledged text as a new copy...");
}
static void code_command(Window *w, int id) {
  Code *c = w->state;
  if (id == 0) {
    if (c->failed)
      recover(w);
    else
      wm_open(APP_CODE, NULL);
  } else if (id == 1)
    dialog_input(w, "Open in Code OSS Core",
                 "Open a UTF-8 source/text file in a new window (preview limit "
                 "256 KiB).",
                 desk.home, open_file);
  else if (id == 2 || id == 3)
    save(w, id == 3);
  else if (id == 4 || id == 5)
    send(w, id == 4 ? CODE_UNDO : CODE_REDO, 0, 0, 0, 0, NULL, 0);
  else if (id == 6)
    dialog_input(w, "Find in document",
                 "Literal, case-sensitive search. Ctrl+G repeats the search.",
                 c->find, find);
  else if (id == 7 || id == 8)
    enqueue(w, CODE_COPY, 0, 0, 0, 0, 0, NULL, 0, id == 8, false);
  else if (id == 9) {
    char *text;
    size_t n;
    int e = clipboard_read(&text, &n);
    if (e)
      clipboard_error(w, "Paste into Code OSS Core", e);
    else {
      if (n)
        send(w, CODE_INSERT, 0, 0, 0, 0, text, n);
      clipboard_release(text);
    }
  } else if (id == 10)
    send(w, CODE_SELECT_ALL, 0, 0, 0, 0, NULL, 0);
  else if (id == 11) {
    c->font_size = c->font_size == 11 ? 13 : 11;
    ensure_cursor(w);
    invalidate_window(w);
  } else if (id == 12) {
    MenuItem items[] = {{"Spaces: 2", I_COUNT, 102, true},
                        {"Spaces: 4", I_COUNT, 104, true},
                        {"Spaces: 8", I_COUNT, 108, true},
                        {"Tabs: width 2", I_COUNT, 202, true},
                        {"Tabs: width 4", I_COUNT, 204, true},
                        {"Tabs: width 8", I_COUNT, 208, true}};
    Rect r;
    toolbar(w, 12, &r);
    menu_open(w, r.x, r.y + r.h, items, 6, code_command);
  } else if (id == 20)
    wm_open(APP_CODE, NULL);
  else if (id == 21)
    recover(w);
  else if (id == 22)
    wm_close(w);
  else if (id == 23)
    dialog_notice(
        w, "Code OSS Core Preview",
        "Genuine upstream Microsoft Code OSS PieceTree text core (MIT), commit "
        "211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb, in a native JavaScriptCore "
        "worker. This original Coast frontend is an experimental graphical "
        "text-core port, not the full VS Code workbench. UTF-8 files up to 256 "
        "KiB; checked saves retain originals. Shared native model service; no "
        "Electron, Node, "
        "language/extension host or persistent crash recovery is included.");
  else if (id == 24 && c->find[0])
    send(w, CODE_FIND, 0, 0, 0, 0, c->find, strlen(c->find));
  else if ((id >= 102 && id <= 108) || (id >= 202 && id <= 208))
    send(w, CODE_TABS, 0, id % 100, id < 200, 0, NULL, 0);
}
void codeoss_key(Window *w, Key k) {
  if (!k.down)
    return;
  Code *c = w->state;
  if (c->closing)
    return;
  if (k.code == KEY_ESCAPE && c->input_error) {
    c->input_error = false;
    invalidate_window(w);
    return;
  }
  unsigned letter =
      k.code >= 4 && k.code <= 0x1d ? 'a' + k.code - 4 : k.ascii | 32;
  if (k.mods & MOD_CTRL) {
    if (letter == 'n') {
      code_command(w, 20);
      return;
    }
    if (letter == 'o') {
      code_command(w, 1);
      return;
    }
    if (letter == 's') {
      save(w, k.mods & MOD_SHIFT);
      return;
    }
    if (letter == 'z' || letter == 'y') {
      code_command(w, letter == 'y' || (k.mods & MOD_SHIFT) ? 5 : 4);
      return;
    }
    if (letter == 'a') {
      code_command(w, 10);
      return;
    }
    if (letter == 'c' || letter == 'x' || letter == 'v') {
      code_command(w, letter == 'c' ? 7 : letter == 'x' ? 8 : 9);
      return;
    }
    if (letter == 'f' || letter == 'g') {
      code_command(w, letter == 'f' ? 6 : 24);
      return;
    }
  }
  if (!c->ready || c->failed || c->closing)
    return;
  unsigned flags = (k.mods & MOD_SHIFT ? CODE_SHIFT : 0) |
                   (k.mods & MOD_CTRL ? CODE_CONTROL : 0),
           direction = 0;
  bool move = true;
  switch (k.code) {
  case KEY_LEFT:
    direction = CODE_LEFT;
    break;
  case KEY_RIGHT:
    direction = CODE_RIGHT;
    break;
  case KEY_HOME:
    direction = CODE_HOME;
    break;
  case KEY_END:
    direction = CODE_END;
    break;
  case KEY_UP:
    direction = CODE_UP;
    break;
  case KEY_DOWN:
    direction = CODE_DOWN;
    break;
  case KEY_PAGEUP:
    direction = CODE_PAGE_UP;
    break;
  case KEY_PAGEDOWN:
    direction = CODE_PAGE_DOWN;
    break;
  default:
    move = false;
    break;
  }
  if (move) {
    send(w, CODE_MOVE, flags, direction, rows(w), 0, NULL, 0);
    return;
  }
  if (k.code == KEY_BACKSPACE || k.code == KEY_DELETE)
    send(w, CODE_DELETE, flags, k.code == KEY_DELETE, 0, 0, NULL, 0);
  else if (k.code == KEY_ENTER)
    send(w, CODE_INSERT, CODE_ENTER, 0, 0, 0, NULL, 0);
  else if (k.code == KEY_TAB)
    send(w, CODE_INSERT, CODE_TAB, 0, 0, 0, NULL, 0);
  else if ((k.mods & MOD_SHIFT) && k.code == KEY_INSERT)
    code_command(w, 9);
  else if (k.ascii >= 32 && k.ascii < 127 &&
           !(k.mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))) {
    char ch = k.ascii;
    send(w, CODE_INSERT, 0, 0, 0, 0, &ch, 1);
  }
}
static void discard(Window *w, const char *unused) {
  (void)unused;
  if (!w || !w->alive)
    return;
  if (power_review_accept(w))
    return;
  ((Code *)w->state)->allow_close = true;
  wm_close(w);
}
bool codeoss_can_close(Window *w) {
  Code *c = w->state;
  if (c->pending || c->queued) {
    if (!power_review_pending(w))
      c->want_close = true;
    message(w, "Finishing accepted core actions before reviewing Close...");
    return false;
  }
  bool approved = power_review_approved(w) || c->allow_close;
  if (!approved && ((c->current.flags & CODE_DIRTY) || c->failed)) {
    if (wm_focused() != w) {
      bool quitting = desk.quit_pending;
      wm_focus(w);
      desk.quit_pending = quitting;
    }
    dialog_confirm(
        w, "Discard unsaved core text?",
        c->failed
            ? "The core connection ended. The last acknowledged text is "
              "retained here. Cancel and choose Recover to save a new copy, or "
              "confirm to discard it."
            : "This Code OSS Core document has unsaved changes. Cancel and "
              "Save/Save As first, or confirm to discard the changes.",
        discard);
    return false;
  }
  return true;
}
bool codeoss_close(Window *w) {
  Code *c = w->state;
  /* Approval queries (including power review) must not release a model. */
  if (c->model_live && !c->failed) {
    if (!c->closing) {
      if (!send(w, CODE_RELEASE, 0, 0, 0, 0, NULL, 0))
        return false;
      c->closing = true;
    }
    c->want_close = true;
    return false;
  }

  if (desk.trace)
    printf("DESKTOP CODE_CORE_STATS window=%d max_tick_ms=%lu\n", w->id,
           c->max_tick_ms);
  if (service.owner == w || c->pending || c->queued)
    failed(
        w,
        "Document transport closed unexpectedly; recover retained snapshots.");
  clear_queue(c);
  deallocate(c->payload, CODE_RESPONSE_LIMIT);
  deallocate(c, sizeof(*c));
  w->state = NULL;
  return true;
}
void codeoss_menu(Window *w, int group, int x, int y) {
  Code *c = w->state;
  if (group == MENU_FILE) {
    MenuItem items[] = {
        {"New core document\tCtrl+N", I_DOC, 20, true},
        {"Open...\tCtrl+O", I_FOLDER, 1, true},
        {"Save\tCtrl+S", I_SAVE, 2, enabled(c, 2)},
        {"Save As...\tCtrl+Shift+S", I_SAVE, 3, c->ready && !c->failed},
        {"Recover last acknowledged text", I_RESTORE, 21, c->failed},
        {"About Code OSS Core Preview", I_INFO, 23, true},
        {"Close\tAlt+F4", I_CLOSE, 22, true}};
    menu_open(w, x, y, items, 7, code_command);
  } else if (group == MENU_EDIT) {
    MenuItem items[] = {
        {"Undo\tCtrl+Z", I_UP, 4, enabled(c, 4)},
        {"Redo\tCtrl+Shift+Z", I_FORWARD, 5, enabled(c, 5)},
        {"Select all\tCtrl+A", I_CHECK, 10, c->ready && !c->failed},
        {"Copy\tCtrl+C", I_COPY, 7, enabled(c, 7)},
        {"Cut\tCtrl+X", I_CUT, 8, enabled(c, 8)},
        {"Paste\tCtrl+V", I_PASTE, 9, c->ready && !c->failed},
        {"Find...\tCtrl+F", I_SEARCH, 6, c->ready && !c->failed},
        {"Find next\tCtrl+G", I_FORWARD, 24,
         c->ready && !c->failed && c->find[0]}};
    menu_open(w, x, y, items, 8, code_command);
  } else if (group == MENU_VIEW) {
    MenuItem items[] = {
        {"Toggle text size (11/13 px)", I_DOC, 11, true},
        {"Indentation...", I_LAYOUT, 12, c->ready && !c->failed},
        {"About this graphical milestone", I_INFO, 23, true}};
    menu_open(w, x, y, items, 3, code_command);
  }
}
