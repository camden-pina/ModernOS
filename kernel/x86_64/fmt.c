#include <fmt.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>  // For uintptr_t

// --- Flag definitions ---
#define FMT_FLAG_LEFT   (1 << 0)  // '-' flag (left justify)
#define FMT_FLAG_PLUS   (1 << 1)  // '+' flag (always show sign)
#define FMT_FLAG_SPACE  (1 << 2)  // ' ' flag (space if no sign)
#define FMT_FLAG_ALT    (1 << 3)  // '#' flag (alternate form)
#define FMT_FLAG_ZERO   (1 << 4)  // '0' flag (pad with zeros)

// --- Helper: append one character into the buffer ---
static void append_char(char c, char *buf, size_t buf_size, size_t *index) {
    // Always increment the total count; only write if there’s room.
    if (*index + 1 < buf_size) {
        buf[*index] = c;
    }
    (*index)++;
}

// --- Helper: append a null-terminated string ---
static void append_string(const char *s, char *buf, size_t buf_size, size_t *index) {
    while (*s) {
        append_char(*s, buf, buf_size, index);
        s++;
    }
}

// --- Helper: output a repeated pad character ---
static void output_padding(char pad, int count, char *buf, size_t buf_size, size_t *index) {
    for (int i = 0; i < count; i++) {
        append_char(pad, buf, buf_size, index);
    }
}

// --- Helper: reverse a string in-place ---
static void reverse_str(char *str, int len) {
    int i = 0, j = len - 1;
    while (i < j) {
        char tmp = str[i];
        str[i] = str[j];
        str[j] = tmp;
        i++;
        j--;
    }
}

// --- Helper: convert an unsigned integer to string in the given base ---
// The result is written into the supplied buffer (which should be large enough)
// and the function returns the number of characters produced.
static int itoa_unsigned(unsigned long long value, int base, int uppercase, char *buf, int buf_size) {
    int i = 0;
    // Special-case zero.
    if (value == 0) {
        if (i < buf_size)
            buf[i++] = '0';
        if (i < buf_size)
            buf[i] = '\0';
        return i;
    }
    while (value != 0 && i < buf_size) {
        int digit = value % base;
        value /= base;
        if (digit < 10)
            buf[i++] = '0' + digit;
        else
            buf[i++] = (uppercase ? 'A' : 'a') + (digit - 10);
    }
    reverse_str(buf, i);
    if (i < buf_size)
        buf[i] = '\0';
    return i;
}

// --- Helper: format an integer (signed or unsigned) into the output buffer ---
// This helper applies width, precision, sign, alternate form (for hex), and padding.
static void format_integer(char *buf, size_t buf_size, size_t *index,
                           unsigned long long value, bool is_negative,
                           int base, int width, int precision, int flags, int uppercase) {
    char num_buf[64];
    int num_len = itoa_unsigned(value, base, uppercase, num_buf, sizeof(num_buf));
    
    // If precision is explicitly zero and the value is zero, output no digits.
    if (precision == 0 && value == 0)
        num_len = 0;
    
    // Determine the sign character, if any.
    char sign_char = 0;
    if (is_negative)
        sign_char = '-';
    else if (flags & FMT_FLAG_PLUS)
        sign_char = '+';
    else if (flags & FMT_FLAG_SPACE)
        sign_char = ' ';
    
    // Determine alternate form prefix for hex.
    int prefix_len = 0;
    char prefix[2] = {0, 0};
    if ((flags & FMT_FLAG_ALT) && value != 0) {
        if (base == 16) {
            prefix[0] = '0';
            prefix[1] = (uppercase ? 'X' : 'x');
            prefix_len = 2;
        }
        // (You could extend alternate forms for octal, etc., if desired.)
    }
    
    // Compute zero padding needed for the precision.
    int zeros = 0;
    if (precision > num_len)
        zeros = precision - num_len;
    
    // Total length includes digits, any zeros, sign, and prefix.
    int total_len = num_len + zeros;
    if (sign_char)
        total_len++;
    total_len += prefix_len;
    
    // Determine any extra padding needed for the given field width.
    int pad = 0;
    if (width > total_len)
        pad = width - total_len;
    
    // If not left-justified, output padding first.
    if (!(flags & FMT_FLAG_LEFT)) {
        char pad_char = ' ';
        // The zero flag (with no explicit precision) causes pad with zeros.
        if ((flags & FMT_FLAG_ZERO) && precision < 0)
            pad_char = '0';
        output_padding(pad_char, pad, buf, buf_size, index);
    }
    
    // Output sign character.
    if (sign_char)
        append_char(sign_char, buf, buf_size, index);
    
    // Output alternate form prefix.
    for (int i = 0; i < prefix_len; i++)
        append_char(prefix[i], buf, buf_size, index);
    
    // Output any zeros required by precision.
    output_padding('0', zeros, buf, buf_size, index);
    
    // Output the digits.
    for (int i = 0; i < num_len; i++)
        append_char(num_buf[i], buf, buf_size, index);
    
    // If left-justified, pad on the right.
    if (flags & FMT_FLAG_LEFT)
        output_padding(' ', pad, buf, buf_size, index);
}

// --- Main formatting function ---
// This function writes formatted output into the preallocated buffer (of size buf_size)
// and returns the number of characters that would have been written (not including the null terminator).
// The 'max_args' parameter is used to limit how many arguments are consumed.
// If a conversion specifier requires an argument and none are left, a placeholder error string is inserted.
int fmt_format(const char *format, char *buf, size_t buf_size, size_t max_args, va_list args) {
    size_t index = 0;      // current output index
    size_t arg_count = 0;  // count of arguments processed

    for (const char *p = format; *p != '\0'; p++) {
        if (*p != '%') {
            append_char(*p, buf, buf_size, &index);
            continue;
        }
        p++;  // skip '%'
        if (*p == '%') {
            // Handle literal '%'
            append_char('%', buf, buf_size, &index);
            continue;
        }
        
        // --- Parse flags ---
        int flags = 0;
        bool parsing_flags = true;
        while (parsing_flags) {
            switch (*p) {
                case '-': flags |= FMT_FLAG_LEFT;  p++; break;
                case '+': flags |= FMT_FLAG_PLUS;  p++; break;
                case ' ': flags |= FMT_FLAG_SPACE; p++; break;
                case '#': flags |= FMT_FLAG_ALT;   p++; break;
                case '0': flags |= FMT_FLAG_ZERO;  p++; break;
                default: parsing_flags = false; break;
            }
        }
        
        // --- Parse field width ---
        int width = 0;
        if (*p == '*') {
            if (arg_count >= max_args) {
                append_string("[ERR]", buf, buf_size, &index);
                break;
            }
            width = va_arg(args, int);
            arg_count++;
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }
        }
        
        // --- Parse precision ---
        int precision = -1;  // negative means "not specified"
        if (*p == '.') {
            p++;  // skip '.'
            precision = 0;
            if (*p == '*') {
                if (arg_count >= max_args) {
                    append_string("[ERR]", buf, buf_size, &index);
                    break;
                }
                precision = va_arg(args, int);
                arg_count++;
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    precision = precision * 10 + (*p - '0');
                    p++;
                }
            }
        }
        
        // --- Parse length modifier ---
        // (Supported: "l" and "ll". A single 'h' is recognized but treated as default.)
        int length = 0;  // 0: default, 1: 'l', 2: 'll', -1: 'h'
        if (*p == 'l') {
            length = 1;
            p++;
            if (*p == 'l') {
                length = 2;
                p++;
            }
        } else if (*p == 'h') {
            length = -1;
            p++;
        }
        
        // --- Parse conversion specifier ---
        char spec = *p;
        if (!spec)
            break;  // premature end-of-string

        switch (spec) {
            case 'd': {
                if (arg_count >= max_args) {
                    append_string("[ERR]", buf, buf_size, &index);
                    break;
                }
                long long val;
                if (length == 2)
                    val = va_arg(args, long long);
                else if (length == 1)
                    val = va_arg(args, long);
                else
                    val = va_arg(args, int);
                arg_count++;
                bool is_negative = (val < 0);
                unsigned long long uval = is_negative ? (unsigned long long)(-val) : (unsigned long long)val;
                // Use base 10; uppercase flag not used for decimals.
                format_integer(buf, buf_size, &index, uval, is_negative, 10, width, precision, flags, 0);
                break;
            }
            case 'u': {
                if (arg_count >= max_args) {
                    append_string("[ERR]", buf, buf_size, &index);
                    break;
                }
                unsigned long long val;
                if (length == 2)
                    val = va_arg(args, unsigned long long);
                else if (length == 1)
                    val = va_arg(args, unsigned long);
                else
                    val = va_arg(args, unsigned int);
                arg_count++;
                format_integer(buf, buf_size, &index, val, false, 10, width, precision, flags, 0);
                break;
            }
            case 'x':
            case 'X': {
                if (arg_count >= max_args) {
                    append_string("[ERR]", buf, buf_size, &index);
                    break;
                }
                unsigned long long val;
                if (length == 2)
                    val = va_arg(args, unsigned long long);
                else if (length == 1)
                    val = va_arg(args, unsigned long);
                else
                    val = va_arg(args, unsigned int);
                arg_count++;
                format_integer(buf, buf_size, &index, val, false, 16, width, precision, flags, (spec == 'X'));
                break;
            }
            case 'c': {
                if (arg_count >= max_args) {
                    append_string("[ERR]", buf, buf_size, &index);
                    break;
                }
                int ch = va_arg(args, int);
                arg_count++;
                // Handle width padding for a single character.
                if (!(flags & FMT_FLAG_LEFT)) {
                    for (int i = 1; i < width; i++) {
                        append_char(' ', buf, buf_size, &index);
                    }
                }
                append_char((char)ch, buf, buf_size, &index);
                if (flags & FMT_FLAG_LEFT) {
                    for (int i = 1; i < width; i++) {
                        append_char(' ', buf, buf_size, &index);
                    }
                }
                break;
            }
            case 's': {
                if (arg_count >= max_args) {
                    append_string("[ERR]", buf, buf_size, &index);
                    break;
                }
                const char *s = va_arg(args, const char*);
                arg_count++;
                if (!s)
                    s = "(null)";
                // Compute the string length (applying precision, if any).
                int str_len = 0;
                while (s[str_len] != '\0')
                    str_len++;
                if (precision >= 0 && precision < str_len)
                    str_len = precision;
                if (!(flags & FMT_FLAG_LEFT)) {
                    for (int i = str_len; i < width; i++)
                        append_char(' ', buf, buf_size, &index);
                }
                for (int i = 0; i < str_len; i++)
                    append_char(s[i], buf, buf_size, &index);
                if (flags & FMT_FLAG_LEFT) {
                    for (int i = str_len; i < width; i++)
                        append_char(' ', buf, buf_size, &index);
                }
                break;
            }
            case 'p': {
                if (arg_count >= max_args) {
                    append_string("[ERR]", buf, buf_size, &index);
                    break;
                }
                void *ptr = va_arg(args, void*);
                arg_count++;
                // For pointers, use the alternate form (0x...) and print as hex.
                flags |= FMT_FLAG_ALT;
                unsigned long long val = (unsigned long long)(uintptr_t)ptr;
                format_integer(buf, buf_size, &index, val, false, 16, width, precision, flags, 0);
                break;
            }
            default:
                // Unrecognized specifier: output it literally.
                append_char('%', buf, buf_size, &index);
                append_char(spec, buf, buf_size, &index);
                break;
        }
    }
    
    // Ensure the output is null-terminated.
    if (buf_size > 0) {
        if (index < buf_size)
            buf[index] = '\0';
        else
            buf[buf_size - 1] = '\0';
    }
    
    return (int)index;
}
