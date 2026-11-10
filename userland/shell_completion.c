#include <libc.h>
#include <text_utf8.h>
/* Bounded, non-executing command/path completion shared by both line editors.
 */
#include "include/shell_completion.h"
#include "desktop/text_input_model.h"
static const char *const builtins[] = {
#define SHELL_COMMAND(name, ...) name,
#include "shell_commands.inc"
#undef SHELL_COMMAND
};
static bool separator(char c) { return c == ' ' || c == '\t' || c == '\n'; }
static bool operator(char c) { return c == '|' || c == '&' || c == ';' || c == '(' || c == ')'; }
static bool safe_name(const char *p) {
    if (!text_utf8_valid(p, strlen(p)))
        return false;
    for (; *p; p++)
        if ((unsigned char)*p < 32 || (unsigned char)*p == 127)
            return false;
    return true;
}
static bool copy(char *out, size_t cap, const char *in) {
    size_t n = strlen(in);
    if (n >= cap)
        return false;
    memcpy(out, in, n + 1);
    return true;
}
static bool join(char *out, const char *a, const char *b) {
    size_t n = strlen(a), m = strlen(b);
    bool slash = n && a[n - 1] != '/';
    if (n + slash + m >= SC_LINE)
        return false;
    memcpy(out, a, n);
    if (slash)
        out[n++] = '/';
    memcpy(out + n, b, m + 1);
    return true;
}
static int decode(const char *line, int first, int last, char *out, int *quote) {
    int n = 0, q = 0;
    for (int i = first; i < last; i++) {
        char c = line[i];
        bool literal = false;
        if (c == '\\' && q != '\'') {
            if (i + 1 == last)
                return -1;
            char next = line[i + 1];
            if (!q || next == '"' || next == '\\' || next == '$' || next == '`') {
                c = next;
                i++;
                literal = true;
            }
        } else if ((c == '\'' || c == '"') && (!q || q == c)) {
            q = q ? 0 : c;
            continue;
        }
        if (!literal && (c == '$' || c == '`') && q != '\'')
            return -1; /* no expansion/evaluation */
        if (n == SC_LINE - 1)
            return -1;
        out[n++] = c;
    }
    out[n] = 0;
    *quote = q;
    return n;
}
struct gather {
    struct sc_result *out;
    const struct sc_fs *fs;
    void *opaque;
    char actual[SC_LINE], display[SC_LINE], prefix[SC_LINE], suffix[SC_LINE];
    bool commands, dirs, executable;
};
static void add(struct sc_result *r, const char *name) {
    if (!safe_name(name))
        return;
    for (unsigned i = 0; i < r->count; i++)
        if (!strcmp(r->matches[i], name))
            return;
    if (r->count == SC_MATCHES) {
        r->truncated = true;
        return;
    }
    copy(r->matches[r->count++], SC_LINE, name);
}
static bool visit(void *opaque, const char *name, unsigned type) {
    struct gather *g = opaque;
    size_t p = strlen(g->prefix), n = strlen(name), s = strlen(g->suffix);
    if (!name[0] || (!g->prefix[0] && name[0] == '.') || !strcmp(name, ".") ||
        !strcmp(name, "..") || strncmp(name, g->prefix, p) || n < p + s ||
        (s && strcmp(name + n - s, g->suffix)))
        return true;
    char path[SC_LINE], value[SC_LINE];
    if (!join(path, g->actual, name))
        return true;
    int kind = type == 2 ? 2 : g->fs->kind(g->opaque, path, g->executable);
    if (kind < 1 || (g->dirs && kind != 2) ||
        (g->executable && kind != 1 && !(kind == 2 && !g->commands)))
        return true;
    if (g->commands) {
        if (!copy(value, sizeof(value), name))
            return true;
    } else {
        size_t d = strlen(g->display);
        if (d + n + (kind == 2) >= sizeof(value))
            return true;
        memcpy(value, g->display, d);
        memcpy(value + d, name, n + 1);
        if (kind == 2) {
            value[d + n] = '/';
            value[d + n + 1] = 0;
        }
    }
    add(g->out, value);
    return true;
}
static int encode(char *out, size_t cap, const char *text, size_t cursor, int style, bool closed,
                  int *encoded_cursor) {
    size_t used = 0;
    *encoded_cursor = -1;
#define EMIT(c)                                                                                    \
    do {                                                                                           \
        if (used + 1 >= cap)                                                                       \
            return -1;                                                                             \
        out[used++] = (c);                                                                         \
    } while (0)
    if (style)
        EMIT(style);
    for (size_t i = 0;; i++) {
        if (i == cursor)
            *encoded_cursor = used;
        char c = text[i];
        if (!c)
            break;
        if (style == '\'' && c == '\'') {
            EMIT('\'');
            EMIT('\\');
            EMIT('\'');
            EMIT('\'');
        } else {
            if ((style == '"' && (c == '"' || c == '\\' || c == '$' || c == '`')) ||
                (!style && (separator(c) || operator(c) || c == '\\' || c == '\'' || c == '"' ||
                            c == '<' || c == '>' || c == '$' || c == '`' || c == '#' || c == '*' ||
                            c == '?' || c == '[' || c == ']' || c == '{' || c == '}' || c == '!')))
                EMIT('\\');
            EMIT(c);
        }
    }
    if (style && closed)
        EMIT(style);
    out[used] = 0;
    return used;
#undef EMIT
}
int shell_complete(struct sc_result *r, char *line, size_t capacity, int *position, const char *cwd,
                   const char *path, const struct sc_fs *fs, void *opaque) {
    memset(r, 0, sizeof(*r));
    size_t length = strlen(line);
    if (!capacity || length >= capacity || length >= SC_LINE)
        return -E_2BIG;
    if (!text_utf8_valid(line, length))
        return 0;
    int cursor = input_position(line, *position), start = 0, end = 0, q = 0;
    bool escaping = false, command = true, redirect = false, dirs = false, run = false;
    for (int i = 0; i <= cursor; i++) {
        char c = i < cursor ? line[i] : 0;
        if (escaping) {
            escaping = false;
            continue;
        }
        if (c == '\\' && q != '\'') {
            escaping = true;
            continue;
        }
        if ((c == '\'' || c == '"') && (!q || q == c)) {
            q = q ? 0 : c;
            continue;
        }
        if (!q && (!c || separator(c) || operator(c) || c == '<' || c == '>')) {
            if (i > start) {
                if (i == cursor)
                    break;
                char word[SC_LINE];
                int state;
                if (decode(line, start, i, word, &state) < 0)
                    return 0;
                if (!redirect && command) {
                    dirs = !strcmp(word, "cd");
                    run = !strcmp(word, "run");
                    bool assignment = false;
                    for (const char *p = word; *p; p++)
                        if (*p == '=')
                            assignment = true;
                    if (!assignment)
                        command = false;
                } else if (run && !redirect) {
                    run = false;
                    command = false;
                }
                redirect = false;
            }
            start = i + 1;
            if (operator(c)) {
                command = true;
                dirs = run = redirect = false;
            } else if (c == '<' || c == '>')
                redirect = true;
        }
    }
    if (start > cursor)
        start = cursor;
    // Include the suffix so middle-of-token completion preserves it.
    int scanq = 0;
    bool esc = false;
    for (end = start; line[end]; end++) {
        char c = line[end];
        if (esc) {
            esc = false;
            continue;
        }
        if (c == '\\' && scanq != '\'') {
            esc = true;
            continue;
        }
        if ((c == '\'' || c == '"') && (!scanq || scanq == c)) {
            scanq = scanq ? 0 : c;
            continue;
        }
        if (!scanq && (separator(c) || operator(c) || c == '<' || c == '>'))
            break;
    }
    char prefix[SC_LINE], whole[SC_LINE];
    int quote, finalq;
    int prefix_n = decode(line, start, cursor, prefix, &quote),
        whole_n = decode(line, start, end, whole, &finalq);
    if (prefix_n < 0 || whole_n < prefix_n || strncmp(whole, prefix, prefix_n))
        return 0;
    struct gather g = {.out = r, .fs = fs, .opaque = opaque, .dirs = dirs && !redirect};
    copy(g.suffix, sizeof(g.suffix), whole + prefix_n);
    char *slash = NULL;
    for (char *p = prefix; *p; p++)
        if (*p == '/')
            slash = p;
    bool cmd = (command || run) && !redirect && !slash;
    if (cmd) {
        g.commands = g.executable = true;
        copy(g.prefix, sizeof(g.prefix), prefix);
        for (unsigned i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
            size_t n = strlen(builtins[i]), s = strlen(g.suffix);
            if (n >= (size_t)prefix_n + s && !strncmp(builtins[i], prefix, prefix_n) &&
                (!s || !strcmp(builtins[i] + n - s, g.suffix)))
                add(r, builtins[i]);
        }
        const char *p = path ? path : "/bin:/usr/bin";
        for (unsigned i = 0; i < 64; i++) {
            char part[SC_LINE];
            size_t n = 0;
            while (p[n] && p[n] != ':')
                n++;
            if (n < sizeof(part)) {
                memcpy(part, p, n);
                part[n] = 0;
                if (!n)
                    copy(g.actual, sizeof(g.actual), cwd);
                else if (part[0] == '/')
                    copy(g.actual, sizeof(g.actual), part);
                else
                    join(g.actual, cwd, part);
                if (fs->scan(opaque, g.actual, visit, &g) == -E_2BIG)
                    r->truncated = true;
            }
            p += n;
            if (!*p)
                break;
            p++;
            if (i == 63)
                r->truncated = true;
        }
    } else {
        g.executable = (command || run) && !redirect;
        if (slash) {
            size_t n = slash - prefix + 1;
            memcpy(g.display, prefix, n);
            g.display[n] = 0;
            copy(g.prefix, sizeof(g.prefix), slash + 1);
        } else
            copy(g.prefix, sizeof(g.prefix), prefix);
        if (g.display[0] == '/')
            copy(g.actual, sizeof(g.actual), g.display);
        else if (!g.display[0])
            copy(g.actual, sizeof(g.actual), cwd);
        else if (!join(g.actual, cwd, g.display))
            return -E_2BIG;
        if (fs->scan(opaque, g.actual, visit, &g) == -E_2BIG)
            r->truncated = true;
    }
    for (unsigned i = 1; i < r->count; i++)
        for (unsigned j = i; j && strcmp(r->matches[j - 1], r->matches[j]) > 0; j--) {
            char tmp[SC_LINE];
            memcpy(tmp, r->matches[j], SC_LINE);
            memcpy(r->matches[j], r->matches[j - 1], SC_LINE);
            memcpy(r->matches[j - 1], tmp, SC_LINE);
        }
    if (!r->count || r->truncated)
        return 0;
    size_t common = strlen(r->matches[0]);
    for (unsigned i = 1; i < r->count; i++) {
        size_t j = 0;
        while (j < common && r->matches[i][j] == r->matches[0][j])
            j++;
        common = j;
    }
    while (common && ((unsigned char)r->matches[0][common] & 0xc0) == 0x80)
        common--;
    size_t suffix = strlen(g.suffix);
    size_t match_length = strlen(r->matches[0]);
    bool match_directory = match_length && r->matches[0][match_length - 1] == '/';
    size_t expanded = r->count == 1 ? match_length - suffix - (suffix && match_directory) : common;
    if (expanded < (size_t)prefix_n)
        return 0;
    if (r->count > 1 && expanded == (size_t)prefix_n)
        return 0;
    char decoded[SC_LINE];
    if (expanded + suffix >= sizeof(decoded))
        return -E_2BIG;
    if (r->count == 1)
        copy(decoded, sizeof(decoded), r->matches[0]);
    else {
        memcpy(decoded, r->matches[0], expanded);
        memcpy(decoded + expanded, g.suffix, suffix + 1);
    }
    size_t n = strlen(decoded);
    bool directory = n && decoded[n - 1] == '/';
    int style = (line[start] == '\'' || line[start] == '"') ? line[start] : 0;
    bool closed = !finalq || (r->count == 1 && !directory);
    char encoded[SC_LINE];
    int dest_cursor;
    int encoded_n =
        encode(encoded, sizeof(encoded), decoded, expanded, style, closed, &dest_cursor);
    if (encoded_n < 0)
        return -E_2BIG;
    bool space = r->count == 1 && !directory && !suffix && end == (int)length;
    if (space) {
        if ((size_t)encoded_n + 1 >= sizeof(encoded))
            return -E_2BIG;
        encoded[encoded_n++] = ' ';
        encoded[encoded_n] = 0;
        dest_cursor = encoded_n;
    }
    if (length - (end - start) + encoded_n >= capacity)
        return -E_2BIG;
    memmove(line + start + encoded_n, line + end, length - end + 1);
    memcpy(line + start, encoded, encoded_n);
    *position = start + dest_cursor;
    r->changed = true;
    return 1;
}
