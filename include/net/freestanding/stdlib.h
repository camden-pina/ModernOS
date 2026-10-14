#ifndef MODERNOS_LWIP_STDLIB_H
#define MODERNOS_LWIP_STDLIB_H

/* lwIP uses only atoi() for interface-name indices. Never include or link a
 * host libc into the freestanding kernel. Other allocation stays in lwIP. */
static inline int atoi(const char *value) {
    unsigned result = 0;
    while (*value == ' ' || *value == '\t')
        value++;
    int sign = 1;
    if (*value == '-' || *value == '+') {
        if (*value == '-')
            sign = -1;
        value++;
    }
    while (*value >= '0' && *value <= '9') {
        unsigned digit = *value++ - '0';
        if (result > (2147483647u - digit) / 10)
            return sign < 0 ? -2147483647 : 2147483647;
        result = result * 10 + digit;
    }
    return sign * (int)result;
}

#endif
