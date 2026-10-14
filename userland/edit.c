#include <libc.h>

#define CAPACITY (256 * 1024)

static char document[CAPACITY], screen[65536], message[160];
static size_t length, cursor, top, horizontal;
static unsigned rows = 25, cols = 80;
static int dirty, readonly, exists;
static unsigned mode = 0644;
static uint64_t original_hash;
static size_t original_length;
static const char *filename;
enum {
    KEY_UP = 256,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_PAGEUP,
    KEY_PAGEDOWN
};

static uint64_t hash_bytes(uint64_t hash, const char *p, size_t n) {
    while (n--) {
        hash ^= (uint8_t)*p++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static int file_hash(const char *path, uint64_t *hash, size_t *size) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;

    char buffer[4096];
    long n;
    *hash = 14695981039346656037ULL;
    *size = 0;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        *hash = hash_bytes(*hash, buffer, n);
        *size += n;
    }

    int e = n < 0 ? n : 0;
    close(fd);
    return e;
}

static void status(const char *text) {
    size_t n = strlen(text);
    if (n >= sizeof(message))
        n = sizeof(message) - 1;
    memcpy(message, text, n);
    message[n] = 0;
}

static void status_error(const char *action, int e) {
    status(action);
    size_t n = strlen(message);
    if (n + 2 < sizeof(message)) {
        strcpy(message + n, ": ");
        n += 2;
    }

    const char *reason = error_text(e);
    size_t m = strlen(reason);
    if (m >= sizeof(message) - n)
        m = sizeof(message) - n - 1;

    memcpy(message + n, reason, m);
    message[n + m] = 0;
}

static size_t line_start(size_t p) {
    while (p && document[p - 1] != '\n')
        p--;
    return p;
}

static size_t line_end(size_t p) {
    while (p < length && document[p] != '\n')
        p++;
    return p;
}

static void vertical(int down) {
    size_t start = line_start(cursor), column = cursor - start, target;
    if (down) {
        target = line_end(cursor);
        if (target == length)
            return;
        target++;
    } else {
        if (!start)
            return;
        target = line_start(start - 1);
    }
    size_t end = line_end(target);
    cursor = target + (column < end - target ? column : end - target);
}

static void insert(char c) {
    if (readonly) {
        status("Read-only view. Open a writable copy to edit.");
        return;
    }
    if (length == CAPACITY) {
        status("Document limit reached: 256 KiB.");
        return;
    }

    memmove(document + cursor + 1, document + cursor, length - cursor);
    document[cursor++] = c;
    length++;
    dirty = 1;
}

static void remove_at(size_t p) {
    if (readonly) {
        status("Read-only view.");
        return;
    }
    if (p >= length)
        return;

    memmove(document + p, document + p + 1, length - p - 1);
    length--;
    dirty = 1;
}

static int suffixed_path(const char *suffix, char *out) {
    size_t n = strlen(filename), s = strlen(suffix);
    if (n + s >= OS_PATH_MAX)
        return -E_NAMETOOLONG;
    strcpy(out, filename);
    strcpy(out + n, suffix);
    return 0;
}

static int save(void) {
    if (readonly)
        return -E_ROFS;

    uint64_t hash;
    size_t size;
    int e = file_hash(filename, &hash, &size);
    if (exists) {
        if (e || size != original_length || hash != original_hash)
            return -E_BUSY;
    } else if (e != -E_NOENT)
        return e ? e : -E_EXIST;

    char temporary[OS_PATH_MAX], backup[OS_PATH_MAX];
    e = suffixed_path(".edit-save", temporary);
    if (e)
        return e;
    e = suffixed_path(".bak", backup);
    if (e)
        return e;

    /* Exclusive staging never destroys a recovery file from an earlier save. */
    int fd = open(temporary, O_CREAT | O_EXCL | O_WRONLY, mode);
    if (fd < 0)
        return fd;

    e = write_all(fd, document, length);
    if (!e)
        e = fsync(fd);
    int closed = close(fd);
    if (!e)
        e = closed;
    if (e) {
        status("Save failed; recovery data remains in FILE.edit-save.");
        return e;
    }

    if (exists) {
        /* Keep the previous complete document instead of truncating it in place. */
        e = call1(SYS_unlink, backup);
        if (e && e != -E_NOENT)
            return e;
        e = call2(SYS_rename, filename, backup);
        if (e)
            return e;
        e = call0(SYS_sync);
        if (e) {
            call2(SYS_rename, backup, filename);
            return e;
        }
    }

    e = call2(SYS_rename, temporary, filename);
    if (e) {
        if (exists)
            call2(SYS_rename, backup, filename);
        return e;
    }
    e = call0(SYS_sync);
    if (e)
        return e;

    exists = 1;
    original_length = length;
    original_hash = hash_bytes(14695981039346656037ULL, document, length);
    dirty = 0;
    status("Saved. Previous version: FILE.bak. Ctrl-Q quits.");
    return 0;
}

static void append_screen(size_t *used, const char *text) {
    while (*text && *used + 1 < sizeof(screen))
        screen[(*used)++] = *text++;
}

static void append_text(size_t *used, const char *text, size_t limit) {
    for (size_t i = 0; text[i] && i < limit && *used + 1 < sizeof(screen); i++) {
        char c = text[i];
        screen[(*used)++] = (c >= 32 && c < 127) ? c : '?';
    }
}

static void render(void) {
    size_t line = 0, column = cursor - line_start(cursor), count = 1;
    for (size_t i = 0; i < length; i++)
        if (document[i] == '\n') {
            count++;
            if (i < cursor)
                line++;
        }

    if (line < top)
        top = line;
    if (line >= top + rows - 3)
        top = line - (rows - 4);
    if (column < horizontal)
        horizontal = column;
    if (column >= horizontal + cols - 1)
        horizontal = column - (cols - 2);

    size_t used = 0;
    append_screen(&used, "\033[H\033[0m");
    append_screen(&used, readonly ? "EDIT [read-only] " : dirty ? "EDIT [modified] " : "EDIT ");
    append_text(&used, filename, cols > 24 ? cols - 24 : 1);
    append_screen(&used, "\033[K\r\n");

    size_t p = 0, number = 0;
    while (p < length && number < top)
        if (document[p++] == '\n')
            number++;
    for (unsigned y = 0; y < rows - 3; y++) {
        size_t end = line_end(p), start = p + horizontal;
        if (start < end)
            for (size_t i = start; i < end && i - start < cols - 1; i++) {
                char c = document[i];
                screen[used++] = (c >= 32 && c < 127) ? c : c == '\t' ? ' ' : '?';
            }
        append_screen(&used, "\033[K\r\n");
        p = end < length ? end + 1 : length;
    }

    append_text(&used, message, cols - 1);
    append_screen(&used, "\033[K\r\n");
    append_text(&used, "Ctrl-S save | Ctrl-Q quit | Ctrl-F find | arrows move", cols - 1);
    append_screen(&used, "\033[K");
    write_all(1, screen, used);
    /* A compact status on the header exposes the document location. */
    printf("\033[1;%uH%lu/%lu\033[%lu;%luH", cols > 20 ? cols - 19 : 1, (unsigned long)line + 1,
           (unsigned long)count, (unsigned long)(line - top) + 2,
           (unsigned long)(column - horizontal) + 1);
}

static int byte_with_timeout(unsigned milliseconds) {
    for (unsigned i = 0;; i++) {
        unsigned char c;
        long n = read(0, &c, 1);
        if (n < 0)
            return n;
        if (n)
            return c;
        if (milliseconds && i >= milliseconds)
            return -E_AGAIN;
        struct os_timespec delay = {0, 1000000};
        call2(SYS_nanosleep, &delay, 0);
    }
}

static int key(void) {
    int c = byte_with_timeout(0);
    if (c != 27)
        return c;
    int first = byte_with_timeout(60);
    if (first != '[' && first != 'O')
        return 27;
    c = byte_with_timeout(60);
    if (c == 'A')
        return KEY_UP;
    if (c == 'B')
        return KEY_DOWN;
    if (c == 'C')
        return KEY_RIGHT;
    if (c == 'D')
        return KEY_LEFT;
    if (c == 'H')
        return KEY_HOME;
    if (c == 'F')
        return KEY_END;
    if (c >= '1' && c <= '6') {
        if (byte_with_timeout(60) != '~')
            return 27;
        return c == '1'   ? KEY_HOME
               : c == '3' ? KEY_DELETE
               : c == '4' ? KEY_END
               : c == '5' ? KEY_PAGEUP
               : c == '6' ? KEY_PAGEDOWN
                          : 27;
    }

    return 27;
}

static void search(void) {
    char needle[64] = {0};
    size_t used = 0;
    for (;;) {
        strcpy(message, "Find: ");
        strcpy(message + 6, needle);
        render();
        int c = key();
        if (c == 27 || c == 17 || c < 0) {
            status("Search cancelled.");
            return;
        }
        if (c == '\r' || c == '\n')
            break;
        if ((c == 127 || c == 8) && used)
            needle[--used] = 0;
        else if (c >= 32 && c < 127 && used + 1 < sizeof(needle)) {
            needle[used++] = c;
            needle[used] = 0;
        }
    }

    if (!used) {
        status("Empty search.");
        return;
    }
    for (size_t offset = 1; offset <= length; offset++) {
        size_t p = (cursor + offset) % (length ? length : 1);
        if (used <= length - p && !memcmp(document + p, needle, used)) {
            cursor = p;
            status("Found.");
            return;
        }
    }

    status("Text not found.");
}

int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        puts(
            "Usage: edit [-r] FILE\nCtrl-S saves; Ctrl-Q confirms unsaved exit; Ctrl-F searches.\nArrows, Home/End, Page Up/Down, Delete, Enter, Backspace.\n256 KiB ASCII text. Saves retain FILE.bak and stage FILE.edit-save.");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "-r")) {
        readonly = 1;
        filename = argv[2];
    } else if (argc == 2 && argv[1][0] != '-')
        filename = argv[1];
    else {
        puts("Usage: edit [-r] FILE\nExample: edit /tmp/notes.txt");
        return 2;
    }

    int fd = open(filename, O_RDONLY, 0);
    if (fd >= 0) {
        struct os_stat st;
        int e = fstat(fd, &st);
        if (e || (st.mode & 0170000) != S_IFREG || st.size < 0 || st.size > CAPACITY) {
            close(fd);
            puts("edit: document must be a regular text file of at most 256 KiB.");
            return 1;
        }
        mode = st.mode & 0777;
        exists = 1;
        long n;
        while (length < CAPACITY && (n = read(fd, document + length, CAPACITY - length)) > 0)
            length += n;
        close(fd);
        if (length != (size_t)st.size) {
            puts("edit: incomplete read; document was not opened.");
            return 1;
        }
        for (size_t i = 0; i < length; i++)
            if (!document[i] || (uint8_t)document[i] >= 128 ||
                (document[i] < 32 && document[i] != '\n' && document[i] != '\r' &&
                 document[i] != '\t')) {
                puts(
                    "edit: binary or non-ASCII document; copy it before converting to ASCII text.");
                return 1;
            }
        if (call2(SYS_access, filename, 2) < 0)
            readonly = 1;
    } else if (fd != -E_NOENT || readonly) {
        printf("edit: %s: %s\n", filename, error_text(fd));
        return 1;
    }

    original_length = length;
    original_hash = hash_bytes(14695981039346656037ULL, document, length);

    struct os_termios saved, raw;
    int e = ioctl(0, TCGETS, &saved);
    if (e) {
        printf("edit: terminal: %s\n", error_text(e));
        return 1;
    }

    raw = saved;
    raw.lflag = 0;
    raw.iflag = ICRNL;
    raw.oflag = 0;
    raw.cc[VMIN] = 0;
    raw.cc[VTIME] = 0;
    e = ioctl(0, TCSETSF, &raw);
    if (e) {
        printf("edit: foreground terminal required: %s\n", error_text(e));
        return 1;
    }

    struct os_winsize size;
    if (!ioctl(0, TIOCGWINSZ, &size)) {
        rows = size.rows < 8 ? 8 : size.rows > 60 ? 60 : size.rows;
        cols = size.cols < 30 ? 30 : size.cols > 200 ? 200 : size.cols;
    }

    write_all(1, "\033[2J", 4);
    status(readonly ? "Read-only view." : "Ready. Ctrl-S saves; Ctrl-Q quits.");
    int confirm = 0;

    for (;;) {
        render();
        int c = key();
        if (c < 0)
            break;
        if (c == 17) {
            if (!dirty || confirm)
                break;
            status("Unsaved changes: Ctrl-S saves, Ctrl-Q again discards, any other key cancels.");
            confirm = 1;
            continue;
        }

        confirm = 0;
        if (c == 19) {
            e = save();
            if (e)
                status_error("Save failed (staged data may remain in FILE.edit-save)", e);
        } else if (c == 6)
            search();
        else if (c == KEY_LEFT && cursor)
            cursor--;
        else if (c == KEY_RIGHT && cursor < length)
            cursor++;
        else if (c == KEY_UP)
            vertical(0);
        else if (c == KEY_DOWN)
            vertical(1);
        else if (c == KEY_HOME || c == 1)
            cursor = line_start(cursor);
        else if (c == KEY_END || c == 5)
            cursor = line_end(cursor);
        else if (c == KEY_PAGEUP || c == KEY_PAGEDOWN) {
            for (unsigned i = 0; i < rows - 3; i++)
                vertical(c == KEY_PAGEDOWN);
        } else if (c == KEY_DELETE)
            remove_at(cursor);
        else if ((c == 127 || c == 8) && cursor) {
            if (!readonly) {
                cursor--;
                remove_at(cursor);
            } else
                status("Read-only view.");
        } else if (c == '\r' || c == '\n')
            insert('\n');
        else if (c == '\t' || (c >= 32 && c < 127))
            insert(c);
    }

    ioctl(0, TCSETSF, &saved);
    write_all(1, "\033[2J\033[H", 7);
    printf("edit: %s%s\n", filename, dirty ? " (unsaved changes discarded)" : "");
    return 0;
}
