#include <printk.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define FMT_PUT(c)                                      \
    do {                                                \
        if (written < size) {                           \
            buf[written] = (c);                         \
        }                                               \
        written++;                                      \
    } while (0)

static const char digits_lower[] = "0123456789abcdef";
static const char digits_upper[] = "0123456789ABCDEF";

static size_t num_to_str(uint64_t val, int base, bool uppercase, char *out) {
    char rev[65];
    size_t idx = 0;
    const char *digits = uppercase ? digits_upper : digits_lower;

    if (val == 0) {
        rev[idx++] = '0';
    } else {
        while (val > 0) {
            rev[idx++] = digits[val % (unsigned int)base];
            val /= (unsigned int)base;
        }
    }

    for (size_t i = 0; i < idx; i++) {
        out[i] = rev[idx - 1 - i];
    }
    out[idx] = '\0';
    return idx;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    size_t written = 0;

    if (!buf && size > 0) return 0;

    while (*fmt) {
        if (*fmt != '%') {
            FMT_PUT(*fmt++);
            continue;
        }

        fmt++; /* Skip '%' */

        /* Parse Flags */
        bool left_align = false;
        bool plus_sign = false;
        bool space_sign = false;
        bool zero_pad = false;

        while (1) {
            if (*fmt == '-')      left_align = true;
            else if (*fmt == '+') plus_sign = true;
            else if (*fmt == ' ') space_sign = true;
            else if (*fmt == '0') zero_pad = true;
            else break;
            fmt++;
        }

        if (left_align) zero_pad = false;

        /* Parse Field Width */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                left_align = true;
                zero_pad = false;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Parse Precision */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(args, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        /* Parse Length Modifiers */
        int length = 0; /* 0: standard, 1: long (l), 2: long long (ll), 3: size_t (z) */
        if (*fmt == 'l') {
            length = 1;
            fmt++;
            if (*fmt == 'l') {
                length = 2;
                fmt++;
            }
        } else if (*fmt == 'z') {
            length = 3;
            fmt++;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;
        }

        char spec = *fmt++;
        if (!spec) break;

        switch (spec) {
            case 'c': {
                char c = (char)va_arg(args, int);
                if (!left_align) {
                    while (--width > 0) FMT_PUT(' ');
                }
                FMT_PUT(c);
                while (--width > 0) FMT_PUT(' ');
                break;
            }

            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";

                size_t len = 0;
                while (s[len] && (precision < 0 || (int)len < precision)) {
                    len++;
                }

                if (!left_align) {
                    for (int w = (int)len; w < width; w++) FMT_PUT(' ');
                }
                for (size_t i = 0; i < len; i++) {
                    FMT_PUT(s[i]);
                }
                if (left_align) {
                    for (int w = (int)len; w < width; w++) FMT_PUT(' ');
                }
                break;
            }

            case 'd':
            case 'i': {
                int64_t val;
                if (length == 2)      val = va_arg(args, long long);
                else if (length == 1) val = va_arg(args, long);
                else if (length == 3) val = va_arg(args, ptrdiff_t);
                else                  val = va_arg(args, int);

                bool neg = val < 0;
                uint64_t uval = neg ? (uint64_t)(-val) : (uint64_t)val;

                char num_buf[32];
                size_t num_len = num_to_str(uval, 10, false, num_buf);

                char sign_char = 0;
                if (neg) sign_char = '-';
                else if (plus_sign) sign_char = '+';
                else if (space_sign) sign_char = ' ';

                int total_len = (int)num_len + (sign_char ? 1 : 0);

                if (!left_align && !zero_pad) {
                    for (int w = total_len; w < width; w++) FMT_PUT(' ');
                }
                if (sign_char) FMT_PUT(sign_char);
                if (!left_align && zero_pad) {
                    for (int w = total_len; w < width; w++) FMT_PUT('0');
                }
                for (size_t i = 0; i < num_len; i++) {
                    FMT_PUT(num_buf[i]);
                }
                if (left_align) {
                    for (int w = total_len; w < width; w++) FMT_PUT(' ');
                }
                break;
            }

            case 'u':
            case 'x':
            case 'X':
            case 'o':
            case 'p': {
                uint64_t uval;
                int base = 10;
                bool upper = (spec == 'X');

                if (spec == 'p') {
                    uval = (uintptr_t)va_arg(args, void *);
                    base = 16;
                    zero_pad = true;
                    width = (int)(sizeof(void *) * 2);
                } else {
                    if (length == 2)      uval = va_arg(args, unsigned long long);
                    else if (length == 1) uval = va_arg(args, unsigned long);
                    else if (length == 3) uval = va_arg(args, size_t);
                    else                  uval = va_arg(args, unsigned int);

                    if (spec == 'x' || spec == 'X') base = 16;
                    else if (spec == 'o')           base = 8;
                }

                char num_buf[65];
                size_t num_len = num_to_str(uval, base, upper, num_buf);

                if (!left_align && zero_pad) {
                    for (int w = (int)num_len; w < width; w++) FMT_PUT('0');
                } else if (!left_align) {
                    for (int w = (int)num_len; w < width; w++) FMT_PUT(' ');
                }
                for (size_t i = 0; i < num_len; i++) {
                    FMT_PUT(num_buf[i]);
                }
                if (left_align) {
                    for (int w = (int)num_len; w < width; w++) FMT_PUT(' ');
                }
                break;
            }

            case '%':
                FMT_PUT('%');
                break;

            default:
                FMT_PUT('%');
                FMT_PUT(spec);
                break;
        }
    }

    if (size > 0) {
        if (written < size) {
            buf[written] = '\0';
        } else {
            buf[size - 1] = '\0';
        }
    }

    return (int)written;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}

/* AML Interpreter compatibility */
void lai_vsnprintf(char *buf, size_t len, const char *fmt, va_list arg) {
    vsnprintf(buf, len, fmt, arg);
}