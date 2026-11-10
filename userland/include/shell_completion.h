#ifndef COAST_SHELL_COMPLETION_H
#define COAST_SHELL_COMPLETION_H
#include <stdbool.h>
#include <stddef.h>
#define SC_LINE 512
#define SC_MATCHES 128
struct sc_result {
    char matches[SC_MATCHES][SC_LINE];
    unsigned count;
    bool truncated, changed;
};
/* scan enumerates names only; type:1regular,2directory,3unknown. */
typedef bool (*sc_visit)(void *, const char *, unsigned);
struct sc_fs {
    int (*scan)(void *, const char *, sc_visit, void *);
    int (*kind)(void *, const char *, bool executable);
};
int shell_complete(struct sc_result *, char *, size_t, int *, const char *, const char *,
                   const struct sc_fs *, void *);
int shell_complete_native(struct sc_result *, char *, size_t, int *, const char *, const char *);
const char *shell_search_path(void);
int shell_read_interactive(char *, size_t, const char *, char[32][512], unsigned);
int shell_read_terminal(char *, size_t, const char *, char[32][512], unsigned);
#endif
