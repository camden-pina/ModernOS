#include <libc.h>
#include "desktop/text_input_model.h"
#include "include/shell_completion.h"
static struct sc_result completion;
static unsigned cells(const char *p, int first, int last) {
    unsigned n = 0;
    while (first < last) {
        first = input_next(p, first);
        n++;
    }
    return n;
}
static void redraw(const char *cwd, const char *line, int cursor) {
    struct os_winsize ws;
    unsigned columns = 80;
    if (!ioctl(0, TIOCGWINSZ, &ws) && ws.cols >= 20)
        columns = ws.cols;
    if (columns > 512)
        columns = 512;
    char prompt[OS_PATH_MAX + 8];
    strcpy(prompt, "os:");
    strcpy(prompt + 3, cwd);
    size_t p = strlen(prompt);
    strcpy(prompt + p, "$ ");
    unsigned prompt_cells = cells(prompt, 0, strlen(prompt));
    const char *shown = prompt;
    if (prompt_cells > columns - 10) {
        int start = strlen(prompt);
        unsigned keep = columns - 16;
        while (start && keep--) {
            start = input_previous(prompt, start);
        }
        shown = prompt + start;
    }
    unsigned width = columns - cells(shown, 0, strlen(shown)) - 1;
    if (width < 4)
        width = 4;
    int first = 0, length = strlen(line);
    while (first < cursor && cells(line, first, cursor) >= width)
        first = input_next(line, first);
    int end = first;
    unsigned n = 0;
    while (end < length && n < width) {
        end = input_next(line, end);
        n++;
    }
    /* One write keeps a redraw together and avoids a separate empty-prompt
       fragment between erasing the old line and printing its current text. */
    char rendered[OS_PATH_MAX + SC_LINE + 64];
    size_t used = 0, prompt_bytes = strlen(shown);
    memcpy(rendered, "\r\033[2K", 5);
    used = 5;
    memcpy(rendered + used, shown, prompt_bytes);
    used += prompt_bytes;
    memcpy(rendered + used, line + first, end - first);
    used += end - first;
    unsigned back = cells(line, cursor, end);
    if (back) {
        rendered[used++] = 27;
        rendered[used++] = '[';
        char digits[16];
        unsigned count = 0;
        do {
            digits[count++] = '0' + back % 10;
            back /= 10;
        } while (back);
        while (count)
            rendered[used++] = digits[--count];
        rendered[used++] = 'D';
    }
    write_all(1, rendered, used);
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
    if (!strcmp(sequence, "H") || !strcmp(sequence, "1~") || !strcmp(sequence, "7~"))
        return 1005;
    if (!strcmp(sequence, "F") || !strcmp(sequence, "4~") || !strcmp(sequence, "8~"))
        return 1006;
    if (!strcmp(sequence, "3~"))
        return 1007;
    return 0;
}
int shell_read_interactive(char *line, size_t capacity, const char *cwd, char history[32][512],
                           unsigned history_count) {
    struct os_termios saved, raw;
    printf("os:%s$ ", cwd);
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
    unsigned available = history_count < 32 ? history_count : 32, position = available;
    char draft[512] = "";
    for (;;) {
        int key = read_key();
        if (key < 0) {
            result = key == -E_PIPE ? 0 : key;
            break;
        }
        if (key == '\n' || key == '\r') {
            write_all(1, "\n", 1);
            result = overflow ? -E_2BIG : (int)strlen(line) + 1;
            break;
        }
        if (key == 3) {
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
        redraw(cwd, line, cursor);
    }
    error = ioctl(0, TCSETS, &saved);
    return error ? error : result;
}
