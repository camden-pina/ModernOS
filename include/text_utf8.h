#ifndef COAST_TEXT_UTF8_H
#define COAST_TEXT_UTF8_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Plain UTF-8 text: no NUL, terminal escape/control bytes or invalid scalars. */
static inline bool text_utf8_valid(const char *text, size_t length) {
    for (size_t i = 0; i < length;) {
        uint32_t c = (uint8_t)text[i++];
        if (c < 0x80) {
            if ((!c || c < 32 || c == 127) && c != '\n' && c != '\r' && c != '\t')
                return false;
            continue;
        }
        unsigned extra;
        uint32_t minimum;
        if (c >= 0xc2 && c <= 0xdf) {
            extra = 1;
            minimum = 0x80;
            c &= 31;
        } else if (c >= 0xe0 && c <= 0xef) {
            extra = 2;
            minimum = 0x800;
            c &= 15;
        } else if (c >= 0xf0 && c <= 0xf4) {
            extra = 3;
            minimum = 0x10000;
            c &= 7;
        } else
            return false;
        if (extra > length - i)
            return false;
        while (extra--) {
            uint8_t part = (uint8_t)text[i++];
            if ((part & 0xc0) != 0x80)
                return false;
            c = (c << 6) | (part & 63);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            return false;
    }
    return true;
}
#endif
