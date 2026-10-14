#include <libc.h>

static const char *name;

static const char *base(const char *path) {
    const char *p = path;
    while (*path) {
        if (*path++ == '/')
            p = path;
    }
    return p;
}

static int fail(const char *path, long e) {
    printf("%s: %s: %s (%ld)\n", name, path, error_text(e), e);
    return 1;
}

static int usage(const char *syntax, const char *detail) {
    printf("Usage: %s\n%s\n", syntax, detail);
    return 2;
}

static int copy_from(int fd) {
    char buffer[4096];
    long n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        int e = write_all(1, buffer, n);
        if (e)
            return e;
    }
    return n < 0 ? n : 0;
}

static int lines(int argc, char **argv, int last) {
    long count = 10;
    const char *path;
    if (argc == 2 && argv[1][0] != '-')
        path = argv[1];
    else if (argc == 4 && !strcmp(argv[1], "-n") && (count = number(argv[2], 10)) >= 0)
        path = argv[3];
    else
        return usage(last ? "tail [-n LINES] FILE" : "head [-n LINES] FILE",
                     "LINES must be a nonnegative decimal integer; default 10.");
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return fail(path, fd);
    char buffer[4096];
    int e = 0;
    if (last && count) {
        long end = lseek(fd, 0, 2), position = end, start = 0, seen = 0;
        if (end < 0) {
            close(fd);
            return fail(path, end);
        }
        while (position > 0 && !start) {
            long size = position > 4096 ? 4096 : position;
            position -= size;
            long n = syscall6(SYS_pread64, fd, (long)buffer, size, position, 0, 0);
            if (n != size) {
                e = n < 0 ? n : -E_IO;
                break;
            }
            for (long i = size - 1; i >= 0; i--)
                if (buffer[i] == '\n' && position + i != end - 1 && ++seen == count) {
                    start = position + i + 1;
                    break;
                }
        }

        if (!e) {
            long offset = lseek(fd, start, 0);
            e = offset < 0 ? offset : copy_from(fd);
        }
    } else if (!last && count) {
        long n;
        while (count && (n = read(fd, buffer, sizeof(buffer))) > 0) {
            long used = 0;
            while (used < n && count)
                if (buffer[used++] == '\n')
                    count--;
            e = write_all(1, buffer, used);
            if (e)
                break;
        }

        if (count && n < 0)
            e = n;
    }

    close(fd);
    return e ? fail(path, e) : 0;
}

static int whitespace(unsigned char c) {
    return c == ' ' || (c >= 9 && c <= 13);
}

static int wc(int argc, char **argv) {
    if (argc < 2)
        return usage("wc FILE...", "Print newline, word, and byte counts for each file.");
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            failed = fail(argv[i], fd);
            continue;
        }
        unsigned long lines = 0, words = 0, bytes = 0;
        int word = 0;
        char buffer[4096];
        long n;
        while ((n = read(fd, buffer, sizeof(buffer))) > 0)
            for (long p = 0; p < n; p++) {
                bytes++;
                if (buffer[p] == '\n')
                    lines++;
                if (whitespace(buffer[p]))
                    word = 0;
                else if (!word) {
                    word = 1;
                    words++;
                }
            }
        if (n < 0)
            failed = fail(argv[i], n);
        else
            printf("%lu %lu %lu %s\n", lines, words, bytes, argv[i]);
        close(fd);
    }

    return failed;
}

static int contains(const char *line, size_t size, const char *pattern) {
    size_t n = strlen(pattern);
    if (n > size)
        return 0;
    for (size_t i = 0; i <= size - n; i++)
        if (!memcmp(line + i, pattern, n))
            return 1;
    return 0;
}

static int grep(int argc, char **argv) {
    int numbered = argc > 1 && !strcmp(argv[1], "-n"), first = numbered ? 2 : 1;
    if (argc < first + 2)
        return usage(
            "grep [-n] PATTERN FILE...",
            "Search case-sensitive literal text. Lines up to 64 KiB; no regular expressions.");
    static char line[65536];
    int matched = 0, failed = 0;
    for (int i = first + 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            fail(argv[i], fd);
            failed = 1;
            continue;
        }
        char buffer[4096];
        long n;
        size_t used = 0;
        unsigned long number = 1;
        int overflow = 0;
        do {
            n = read(fd, buffer, sizeof(buffer));
            if (n < 0) {
                fail(argv[i], n);
                failed = 1;
                break;
            }
            for (long p = 0; p < n || (!n && used); p++) {
                int end = !n || buffer[p] == '\n';
                if (end) {
                    if (overflow) {
                        puts("grep: line exceeds 64 KiB; line skipped.");
                        failed = 1;
                    } else if (contains(line, used, argv[first])) {
                        matched = 1;
                        if (argc - first > 2)
                            printf("%s:", argv[i]);
                        if (numbered)
                            printf("%lu:", number);
                        write_all(1, line, used);
                        write_all(1, "\n", 1);
                    }

                    used = 0;
                    overflow = 0;
                    number++;
                    if (!n)
                        break;
                } else if (used < sizeof(line))
                    line[used++] = buffer[p];
                else
                    overflow = 1;
            }
        } while (n > 0);
        close(fd);
    }

    return failed ? 2 : matched ? 0 : 1;
}

static int find(const char *path, unsigned depth) {
    printf("%s\n", path);
    if (depth == 16) {
        puts("find: directory depth limit reached (16).");
        return 1;
    }
    struct os_stat st;
    int e = stat(path, &st);
    if (e)
        return fail(path, e);
    if ((st.mode & 0170000) != S_IFDIR)
        return 0;
    int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return fail(path, fd);
    uint8_t buffer[1024];
    long n;
    int failed = 0;
    while ((n = call3(SYS_getdents64, fd, buffer, sizeof(buffer))) > 0) {
        for (long off = 0; off < n;) {
            struct os_dirent *entry = (void *)(buffer + off);
            if (entry->reclen < 24 || entry->reclen > n - off) {
                close(fd);
                return fail(path, -E_IO);
            }
            off += entry->reclen;
            if (!strcmp(entry->name, ".") || !strcmp(entry->name, ".."))
                continue;
            char child[OS_PATH_MAX];
            size_t p = strlen(path), s = strlen(entry->name);
            if (p + s + 2 > sizeof(child)) {
                failed = fail(path, -E_NAMETOOLONG);
                continue;
            }
            strcpy(child, path);
            if (p && child[p - 1] != '/')
                child[p++] = '/';
            strcpy(child + p, entry->name);
            if (entry->type == 4)
                failed |= find(child, depth + 1);
            else
                puts(child);
        }
    }

    if (n < 0)
        failed = fail(path, n);
    close(fd);
    return failed;
}

static int hex(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return fail(path, fd);
    unsigned long offset = 0;
    uint8_t bytes[16];
    long n;
    const char *digits = "0123456789abcdef";
    while ((n = read(fd, bytes, sizeof(bytes))) > 0) {
        char line[96];
        size_t used = 0;
        for (int i = 15; i >= 0; i--)
            line[used++] = digits[(offset >> (i * 4)) & 15];
        line[used++] = ' ';
        line[used++] = ' ';
        for (unsigned i = 0; i < 16; i++) {
            line[used++] = i < (unsigned)n ? digits[bytes[i] >> 4] : ' ';
            line[used++] = i < (unsigned)n ? digits[bytes[i] & 15] : ' ';
            line[used++] = ' ';
        }
        line[used++] = '|';
        for (long i = 0; i < n; i++)
            line[used++] = bytes[i] >= 32 && bytes[i] < 127 ? bytes[i] : '.';
        line[used++] = '|';
        line[used++] = '\n';
        int e = write_all(1, line, used);
        if (e) {
            close(fd);
            return fail(path, e);
        }
        offset += n;
    }

    close(fd);
    return n < 0 ? fail(path, n) : 0;
}

int main(int argc, char **argv) {
    name = base(argv[0]);
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        if (!strcmp(name, "head") || !strcmp(name, "tail"))
            printf("Usage: %s [-n LINES] FILE\nPrint the %s lines, default 10.\n", name,
                   !strcmp(name, "head") ? "first" : "last");
        else if (!strcmp(name, "wc"))
            puts("Usage: wc FILE...\nCount newline bytes, words, and bytes.");
        else if (!strcmp(name, "grep"))
            puts(
                "Usage: grep [-n] PATTERN FILE...\nCase-sensitive literal text; -n shows line numbers; 64 KiB line limit.");
        else if (!strcmp(name, "find"))
            puts(
                "Usage: find [PATH]\nList a directory tree; depth limit 16; directory symlinks are not followed.");
        else
            puts("Usage: hexdump FILE\nShow 16 bytes per line as hex and printable ASCII.");
        return 0;
    }

    if (!strcmp(name, "head") || !strcmp(name, "tail"))
        return lines(argc, argv, !strcmp(name, "tail"));
    if (!strcmp(name, "wc"))
        return wc(argc, argv);
    if (!strcmp(name, "grep"))
        return grep(argc, argv);
    if (!strcmp(name, "find") && argc <= 2)
        return find(argc == 2 ? argv[1] : ".", 0);
    if (!strcmp(name, "hexdump") && argc == 2)
        return hex(argv[1]);
    return usage(!strcmp(name, "find") ? "find [PATH]" : "hexdump FILE",
                 "See COMMAND --help for details.");
}
