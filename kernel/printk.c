#include <printk.h>
#include <console.h>
#include <8250.h>
#include <sync.h>

static spinlock_t output_lock;
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define BUFFER_SIZE 1024

/* -------------------------------------------------------------------------- */
/* Subsystem State                                                            */
/* -------------------------------------------------------------------------- */

static struct console *console_list = NULL;
static uint64_t (*uptime_fn)(void) = NULL;
static bool color_enabled = true;
static int log_newline = 1;
static int current_level = 6; /* Default: KERN_INFO */

/* -------------------------------------------------------------------------- */
/* Integer Monotonic Timestamp Generator                                      */
/* -------------------------------------------------------------------------- */

uint64_t printk_uptime_us(void) {
    if (uptime_fn) {
        return uptime_fn();
    }
    return 0;
}

void printk_set_uptime_fn(uint64_t (*fn)(void)) {
    uptime_fn = fn;
}

void printk_set_color_enabled(bool enabled) {
    color_enabled = enabled;
}

static void format_timestamp(char *out, uint64_t sec, uint64_t usec) {
    /* Formats: "[ %5lu.%06lu] " (strictly 15 characters) */
    char s_sec[24];
    int s_idx = 0;
    uint64_t tmp_s = sec;

    if (tmp_s == 0) {
        s_sec[s_idx++] = '0';
    } else {
        char rev[24];
        int r_idx = 0;
        while (tmp_s > 0) {
            rev[r_idx++] = '0' + (char)(tmp_s % 10);
            tmp_s /= 10;
        }
        while (r_idx > 0) {
            s_sec[s_idx++] = rev[--r_idx];
        }
    }
    s_sec[s_idx] = '\0';

    int pad = 5 - s_idx;
    if (pad < 0)
        pad = 0;

    char s_usec[7];
    uint64_t tmp_u = usec % 1000000ULL;
    for (int i = 5; i >= 0; i--) {
        s_usec[i] = '0' + (char)(tmp_u % 10);
        tmp_u /= 10;
    }
    s_usec[6] = '\0';

    int o = 0;
    out[o++] = '[';
    for (int i = 0; i < pad; i++) {
        out[o++] = ' ';
    }
    for (int i = 0; i < s_idx; i++) {
        out[o++] = s_sec[i];
    }
    out[o++] = '.';
    for (int i = 0; i < 6; i++) {
        out[o++] = s_usec[i];
    }
    out[o++] = ']';
    out[o++] = ' ';
    out[o] = '\0';
}

/* -------------------------------------------------------------------------- */
/* Console Multiplexer                                                        */
/* -------------------------------------------------------------------------- */

void register_console(struct console *con) {
    if (!con || !con->write)
        return;

    struct console *curr = console_list;
    while (curr) {
        if (curr == con)
            return;
        curr = curr->next;
    }

    con->next = console_list;
    console_list = con;
}

void unregister_console(struct console *con) {
    if (!con || !console_list)
        return;

    if (console_list == con) {
        console_list = con->next;
        con->next = NULL;
        return;
    }

    struct console *curr = console_list;
    while (curr->next) {
        if (curr->next == con) {
            curr->next = con->next;
            con->next = NULL;
            return;
        }
        curr = curr->next;
    }
}

static void console_write_all(const char *str, size_t len) {
    struct console *con = console_list;
    while (con) {
        if (con->flags & CON_ENABLED) {
            con->write(con, str, len);
        }
        con = con->next;
    }
}

static size_t k_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len])
        len++;
    return len;
}

/* -------------------------------------------------------------------------- */
/* Polled Early UART Driver (COM1)                                            */
/* -------------------------------------------------------------------------- */

static struct early_serial {
    uint16_t port;
} early_serial_cfg = {
    .port = COM1_PORT,
};

static void early_serial_write(struct console *con, const char *str, size_t len) {
    struct early_serial *cfg = (struct early_serial *)con->data;
    uint16_t port = cfg ? cfg->port : COM1_PORT;

    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n') {
            serial_port_write_char(port, '\r');
        }
        serial_port_write_char(port, str[i]);
    }
}

static struct console early_serial_console = {
    .name = "earlyser0",
    .write = early_serial_write,
    .flags = CON_BOOT | CON_ENABLED,
    .data = &early_serial_cfg,
    .next = NULL,
};

/* -------------------------------------------------------------------------- */
/* Line Parser & ANSI Stream Engine                                           */
/* -------------------------------------------------------------------------- */

static const char *get_level_color(int level) {
    switch (level) {
    case 0:
        return KLOG_ANSI_EMERG;
    case 1:
        return KLOG_ANSI_ALERT;
    case 2:
        return KLOG_ANSI_CRIT;
    case 3:
        return KLOG_ANSI_ERR;
    case 4:
        return KLOG_ANSI_WARNING;
    case 5:
        return KLOG_ANSI_NOTICE;
    case 6:
        return KLOG_ANSI_INFO;
    case 7:
        return KLOG_ANSI_DEBUG;
    default:
        return KLOG_ANSI_INFO;
    }
}

static void printk_emit(const char *buf) {
    if (!buf)
        return;
    const char *p = buf;

    while (*p) {
        if (log_newline) {
            /* 1. Parse ASCII SOH level: "\001" + digit / 'c' */
            if (p[0] == KERN_SOH_ASCII && p[1] != '\0') {
                char lvl = p[1];
                p += 2;
                if (lvl == 'c') {
                    /* Continuation line requested: skip timestamp */
                    log_newline = 0;
                } else if (lvl >= '0' && lvl <= '7') {
                    current_level = lvl - '0';
                }
            }
            /* 2. Parse legacy format: "<0>" ... "<7>" or "<c>" */
            else if (p[0] == '<' && p[2] == '>') {
                if (p[1] >= '0' && p[1] <= '7') {
                    current_level = p[1] - '0';
                    p += 3;
                } else if (p[1] == 'c') {
                    log_newline = 0;
                    p += 3;
                } else {
                    current_level = 6;
                }
            } else {
                current_level = 6; /* Default: KERN_INFO */
            }

            if (*p == '\0')
                break;

            /* 3. Emit Linux monotonic timestamp if start of line */
            if (log_newline) {
                char ts[32];
                uint64_t uptime = printk_uptime_us();
                format_timestamp(ts, uptime / 1000000ULL, uptime % 1000000ULL);

                if (color_enabled) {
                    console_write_all(KLOG_ANSI_TIME, k_strlen(KLOG_ANSI_TIME));
                }
                console_write_all(ts, k_strlen(ts));
                if (color_enabled) {
                    console_write_all(KLOG_ANSI_RESET, k_strlen(KLOG_ANSI_RESET));
                    const char *lvl_col = get_level_color(current_level);
                    if (lvl_col && *lvl_col) {
                        console_write_all(lvl_col, k_strlen(lvl_col));
                    }
                }
                log_newline = 0;
            }
        } else {
            /* Mid-string tag evaluation */
            if (p[0] == KERN_SOH_ASCII && p[1] == 'c') {
                p += 2;
            } else if (p[0] == KERN_SOH_ASCII && p[1] >= '0' && p[1] <= '7') {
                if (color_enabled) {
                    console_write_all(KLOG_ANSI_RESET, k_strlen(KLOG_ANSI_RESET));
                }
                console_write_all("\n", 1);
                log_newline = 1;
                continue;
            }
        }

        /* 4. Stream slice up to next delimiter */
        const char *chunk_start = p;
        while (*p && *p != '\n' && *p != KERN_SOH_ASCII) {
            p++;
        }

        if (p > chunk_start) {
            console_write_all(chunk_start, (size_t)(p - chunk_start));
        }

        if (*p == '\n') {
            if (color_enabled) {
                console_write_all(KLOG_ANSI_RESET, k_strlen(KLOG_ANSI_RESET));
            }
            console_write_all("\n", 1);
            p++;
            log_newline = 1;
            current_level = 6;
        } else if (*p == KERN_SOH_ASCII) {
            continue;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void printk_early_init(void) {
    serial_port_init(early_serial_cfg.port);
    register_console(&early_serial_console);
}

int vprintk(const char *fmt, va_list args) {
    char str[BUFFER_SIZE];
    int len = vsnprintf(str, BUFFER_SIZE, fmt, args);
    uint64_t saved = spin_lock_irqsave(&output_lock);
    printk_emit(str);
    spin_unlock_irqrestore(&output_lock, saved);
    return len;
}

int printk(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = vprintk(fmt, args);
    va_end(args);
    return len;
}

void kprintf_kputs(const char *str) {
    if (!str)
        return;
    uint64_t saved = spin_lock_irqsave(&output_lock);
    printk_emit(str);
    spin_unlock_irqrestore(&output_lock, saved);
}

void kvfprintf(const char *format, va_list args) {
    vprintk(format, args);
}

void console_write(const char *str, size_t len) {
    uint64_t saved = spin_lock_irqsave(&output_lock);
    console_write_all(str, len);
    spin_unlock_irqrestore(&output_lock, saved);
}
