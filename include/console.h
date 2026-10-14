#ifndef _LINUX_CONSOLE_H
#define _LINUX_CONSOLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define CON_PRINTBUFFER (1 << 0)
#define CON_ENABLED (1 << 1)
#define CON_BOOT (1 << 2) /* Early boot console */
#define CON_ANYTIME (1 << 3)

struct console {
    const char *name;
    void (*write)(struct console *con, const char *str, size_t len);
    int flags;
    void *data;
    struct console *next;
};

void console_write(const char *str, size_t len);

void register_console(struct console *con);
void unregister_console(struct console *con);

#endif /* _LINUX_CONSOLE_H */
