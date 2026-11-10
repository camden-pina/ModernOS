#ifndef COAST_TEXT_INPUT_MODEL_H
#define COAST_TEXT_INPUT_MODEL_H
#include <stdbool.h>
#include <stddef.h>
static inline int input_position(const char *text, int position) {
    int length = (int)strlen(text);
    if (position < 0)
        position = 0;
    if (position > length)
        position = length;
    while (position && (((unsigned char)text[position] & 0xc0) == 0x80))
        position--;
    return position;
}
static inline int input_previous(const char *text, int position) {
    if (position > 0)
        position--;
    return input_position(text, position);
}
static inline int input_next(const char *text, int position) {
    int length = (int)strlen(text);
    if (position < length)
        position++;
    while (position < length && (((unsigned char)text[position] & 0xc0) == 0x80))
        position++;
    return position;
}
static inline bool input_replace(char *text, size_t capacity, int *cursor, int *anchor,
                                 const char *bytes, size_t length) {
    size_t n = strlen(text);
    int c = input_position(text, *cursor), a = input_position(text, *anchor);
    size_t first = c < a ? c : a, last = c < a ? a : c;
    if (!capacity || n >= capacity || length > capacity - 1 - (n - (last - first)))
        return false;
    memmove(text + first + length, text + last, n - last + 1);
    if (length)
        memcpy(text + first, bytes, length);
    *cursor = *anchor = (int)(first + length);
    return true;
}
#endif
