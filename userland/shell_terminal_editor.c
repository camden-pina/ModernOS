#include <libc.h>

#include "desktop/text_input_model.h"
#include "include/shell_completion.h"
static struct sc_result completion;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
static unsigned cells(const char *p, int first, int last) {
  unsigned n = 0;
  while (first < last) {
    first = input_next(p, first);
    n++;
  }
  return n;
}
/* Shell-owned ANSI editing on the actual PTY. Never read ahead merely to
   query display state: typeahead must remain available to a child program. */
static int previous_cursor_row, previous_rows = 1, previous_columns;
static unsigned previous_cursor_cells, previous_total_cells;
static void append_number(char *out, size_t cap, unsigned n, char final) {
  char number[16];
  unsigned used = 0;
  do {
    number[used++] = '0' + n % 10;
    n /= 10;
  } while (n);
  size_t pos = strlen(out);
  if (pos + used + 4 >= cap)
    return;
  out[pos++] = 27;
  out[pos++] = '[';
  while (used)
    out[pos++] = number[--used];
  out[pos++] = final;
  out[pos] = 0;
}
static void redraw(const char *cwd, const char *line, int cursor) {
  struct os_winsize ws;
  unsigned columns = 80, rows = 24;
  if (!ioctl(0, TIOCGWINSZ, &ws)) {
    columns = MAX(20, MIN(512, ws.cols));
    rows = MAX(3, MIN(100, ws.rows));
  }
  char prompt[OS_PATH_MAX + 8];
  strcpy(prompt, "os:");
  strcpy(prompt + 3, cwd);
  strcpy(prompt + strlen(prompt), "$ ");
  unsigned prompt_cells = cells(prompt, 0, strlen(prompt)),
           length = strlen(line), total = prompt_cells + cells(line, 0, length),
           at = prompt_cells + cells(line, 0, cursor);
  unsigned old_row = previous_cursor_row, old_rows = previous_rows;
  if (previous_columns && previous_columns != (int)columns) {
    old_row = previous_cursor_cells &&
                      previous_cursor_cells == previous_total_cells &&
                      !(previous_cursor_cells % columns)
                  ? (previous_cursor_cells - 1) / columns
                  : previous_cursor_cells / columns;
    old_rows = MAX(1, (previous_total_cells + columns - 1) / columns);
  }
  old_rows = MIN(old_rows, rows);
  old_row = MIN(old_row, rows - 1);
  char output[4096] = "";
  strcpy(output, "\r");
  if (old_row)
    append_number(output, sizeof(output), old_row, 'A');
  for (unsigned y = 0; y < old_rows; y++) {
    strcpy(output + strlen(output), "\033[2K");
    if (y + 1 < old_rows)
      append_number(output, sizeof(output), 1, 'B');
  }
  if (old_rows > 1)
    append_number(output, sizeof(output), old_rows - 1, 'A');
  /* Ordinary bounded commands wrap across real terminal rows. If an input is
     taller than the entire viewport, show a bounded tail without dropping data.
   */
  unsigned first = 0, last = length, visible_prompt = prompt_cells;
  const char *shown_prompt = prompt;
  unsigned max_cells = columns * (rows - 1);
  if (total > max_cells) {
    shown_prompt = ".. ";
    visible_prompt = 3;
    unsigned keep = max_cells - 3;
    int scan = cursor;
    unsigned n = 0;
    while (scan && n < keep / 2) {
      scan = input_previous(line, scan);
      n++;
    }
    first = scan;
    last = first;
    unsigned shown = 0;
    while (last < (unsigned)length && shown < keep) {
      last = input_next(line, last);
      shown++;
    }
    at = visible_prompt + cells(line, first, cursor);
    total = visible_prompt + cells(line, first, last);
  }
  strcpy(output + strlen(output), shown_prompt);
  size_t used = strlen(output), bytes = last - first;
  if (used + bytes + 64 < sizeof(output)) {
    memcpy(output + used, line + first, bytes);
    output[used + bytes] = 0;
  }
  unsigned end_row = total ? (total - 1) / columns : 0,
           cursor_row = at / columns, cursor_col = at % columns;
  if (at == total && at && !cursor_col) {
    cursor_row = end_row;
    cursor_col = columns - 1;
  }
  end_row = MIN(end_row, rows - 1);
  cursor_row = MIN(cursor_row, rows - 1);
  if (end_row > cursor_row)
    append_number(output, sizeof(output), end_row - cursor_row, 'A');
  else if (cursor_row > end_row)
    append_number(output, sizeof(output), cursor_row - end_row, 'B');
  append_number(output, sizeof(output), cursor_col + 1, 'G');
  write_all(1, output, strlen(output));
  previous_cursor_row = cursor_row;
  previous_rows = MIN(rows, MAX(1, (total + columns - 1) / columns));
  previous_columns = columns;
  previous_cursor_cells = at;
  previous_total_cells = total;
}

static int read_key(void) {
  unsigned char c;
  long n = read(0, &c, 1);
  if (n <= 0)
    return n < 0 ? (int)n : -E_PIPE;
  return c;
}
static int escape_key(void) {
  struct os_pollfd p = {.fd = 0, .events = POLLIN};
  if (call3(SYS_poll, &p, 1, 35) <= 0)
    return 0;
  int c = read_key();
  if (c != '[' && c != 'O')
    return 0;
  char sequence[8];
  unsigned n = 0;
  do {
    p.revents = 0;
    if (call3(SYS_poll, &p, 1, 35) <= 0)
      return 0;
    c = read_key();
    if (c < 0)
      return c;
    sequence[n++] = c;
  } while (n < sizeof(sequence) - 1 && !((c >= 'A' && c <= 'Z') || c == '~'));
  sequence[n] = 0;
  if (!strcmp(sequence, "A"))
    return 1001;
  if (!strcmp(sequence, "B"))
    return 1002;
  if (!strcmp(sequence, "C"))
    return 1003;
  if (!strcmp(sequence, "D"))
    return 1004;
  if (!strcmp(sequence, "H") || !strcmp(sequence, "1~") ||
      !strcmp(sequence, "7~"))
    return 1005;
  if (!strcmp(sequence, "F") || !strcmp(sequence, "4~") ||
      !strcmp(sequence, "8~"))
    return 1006;
  if (!strcmp(sequence, "3~"))
    return 1007;
  return 0;
}
int shell_read_terminal(char *line, size_t capacity, const char *cwd,
                        char history[32][512], unsigned history_count) {
  struct os_termios saved, raw;
  previous_columns = 0;
  char initial[OS_PATH_MAX + 8];
  strcpy(initial, "os:");
  strcpy(initial + 3, cwd);
  strcpy(initial + strlen(initial), "$ ");
  previous_cursor_cells = previous_total_cells =
      cells(initial, 0, strlen(initial));
  struct os_winsize start_size;
  unsigned width = 80;
  if (!ioctl(0, TIOCGWINSZ, &start_size))
    width = MAX(20, start_size.cols);
  previous_cursor_row = previous_cursor_cells / width;
  previous_rows = previous_cursor_row + 1;
  write_all(1, initial, strlen(initial));
  if (ioctl(0, TCGETS, &saved))
    return read_line(0, line, capacity);
  raw = saved;
  raw.lflag &= ~(ICANON | ECHO | ISIG);
  raw.cc[VMIN] = 1;
  raw.cc[VTIME] = 0;
  int error = ioctl(0, TCSETS, &raw);
  if (error)
    return read_line(0, line, capacity);
  line[0] = 0;
  int cursor = 0, result = 0;
  bool overflow = false;
  unsigned available = history_count < 32 ? history_count : 32,
           position = available;
  char draft[512] = "";
  for (;;) {
    int key = read_key();
    if (key < 0) {
      result = key == -E_PIPE ? 0 : key;
      break;
    }
    if (key == '\n' || key == '\r') {
      redraw(cwd, line, strlen(line));
      write_all(1, "\n", 1);
      result = overflow ? -E_2BIG : (int)strlen(line) + 1;
      break;
    }
    if (key == 3) {
      redraw(cwd, line, strlen(line));
      line[0] = 0;
      write_all(1, "^C\n", 3);
      result = 1;
      break;
    }
    if (overflow)
      continue;
    if (key == 27) {
      key = escape_key();
      if (key < 0) {
        result = key;
        break;
      }
    }
    if (key == 4 && !line[0]) {
      write_all(1, "\n", 1);
      result = 0;
      break;
    }
    if (key == 12) {
      write_all(1, "\033[2J\033[H", 7);
      previous_cursor_row = 0;
      previous_rows = 1;
      previous_columns = 0;
      redraw(cwd, line, cursor);
      continue;
    }
    if (key == 9) {
      int n = shell_complete_native(&completion, line, capacity, &cursor, cwd,
                                    shell_search_path());
      if (n < 0) {
        printf("\nCompletion: %s\n", error_text(n));
      } else if (completion.count > 1 || completion.truncated) {
        write_all(1, "\n", 1);
        for (unsigned i = 0; i < completion.count; i++)
          puts(completion.matches[i]);
        if (completion.truncated)
          puts("More matches exist; type a longer prefix.");
      } else if (!completion.count)
        write_all(1, "\007", 1);
      if (n < 0 || completion.count > 1 || completion.truncated) {
        previous_cursor_row = 0;
        previous_rows = 1;
        previous_columns = 0;
      }
      redraw(cwd, line, cursor);
      continue;
    }
    if (key == 1001 || key == 1002) {
      if (position == available)
        strcpy(draft, line);
      if (key == 1001 && position)
        position--;
      else if (key == 1002 && position < available)
        position++;
      if (position == available)
        strcpy(line, draft);
      else
        strcpy(line, history[(history_count - available + position) % 32]);
      cursor = strlen(line);
    } else if (key == 1003) {
      cursor = input_next(line, cursor);
    } else if (key == 1004) {
      cursor = input_previous(line, cursor);
    } else if (key == 1005 || key == 1) {
      cursor = 0;
    } else if (key == 1006 || key == 5) {
      cursor = strlen(line);
    } else if (key == 127 || key == 8 || key == 1007 || key == 4) {
      int anchor = cursor;
      if (key == 127 || key == 8)
        anchor = input_previous(line, cursor);
      else
        anchor = input_next(line, cursor);
      input_replace(line, capacity, &cursor, &anchor, "", 0);
    } else if (key == 21) {
      memmove(line, line + cursor, strlen(line + cursor) + 1);
      cursor = 0;
    } else if (key == 11) {
      line[cursor] = 0;
    } else if (key == 23) {
      int anchor = cursor;
      while (anchor && (line[anchor - 1] == ' ' || line[anchor - 1] == '\t'))
        anchor--;
      while (anchor && line[anchor - 1] != ' ' && line[anchor - 1] != '\t')
        anchor = input_previous(line, anchor);
      input_replace(line, capacity, &cursor, &anchor, "", 0);
    } else if (key >= 32 && key <= 255) {
      size_t n = strlen(line);
      if (n + 1 >= capacity) {
        overflow = true;
        continue;
      }
      memmove(line + cursor + 1, line + cursor, n - cursor + 1);
      line[cursor++] = key;
    } else
      continue;
    struct os_pollfd ready = {.fd = 0, .events = POLLIN};
    if (call3(SYS_poll, &ready, 1, 0) > 0)
      continue;
    redraw(cwd, line, cursor);
  }
  error = ioctl(0, TCSETS, &saved);
  return error ? error : result;
}
