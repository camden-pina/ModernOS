#ifndef COAST_POWER_STATUS_H
#define COAST_POWER_STATUS_H

#include <libc.h>
#include <stdbool.h>

/* Parse complete key/value lines only. Missing or unfamiliar states fail closed;
   a firmware advertisement is never treated as OS power readiness. */
static inline bool power_status_value(const char *text, const char *key, char *out, size_t size) {
    size_t key_size = strlen(key);
    if (!size)
        return false;
    out[0] = 0;
    for (const char *p = text; *p;) {
        const char *end = p;
        while (*end && *end != '\n')
            end++;
        if (*end == '\n' && (size_t)(end - p) > key_size && !strncmp(p, key, key_size) &&
            p[key_size] == '=') {
            size_t n = (size_t)(end - p) - key_size - 1;
            if (n >= size)
                return false;
            memcpy(out, p + key_size + 1, n);
            out[n] = 0;
            return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

static inline bool power_status_uint(const char *text, const char *key, uint64_t *value) {
    char digits[21];
    if (!power_status_value(text, key, digits, sizeof(digits)) || !digits[0])
        return false;
    uint64_t n = 0;
    for (const char *p = digits; *p; p++) {
        if (*p < '0' || *p > '9' || n > (UINT64_MAX - (unsigned)(*p - '0')) / 10)
            return false;
        n = n * 10 + (unsigned)(*p - '0');
    }
    *value = n;
    return true;
}

static inline bool power_status_flag(const char *text, const char *key) {
    char value[2];
    return power_status_value(text, key, value, sizeof(value)) && !strcmp(value, "1");
}

static inline bool power_status_idle(const char *text) {
    char state[24];
    return power_status_value(text, "power_state", state, sizeof(state)) &&
           (!strcmp(state, "idle") || !strcmp(state, "failed"));
}

static inline bool power_status_cancellable(const char *text) {
    char state[24];
    return power_status_value(text, "power_state", state, sizeof(state)) &&
           (!strcmp(state, "pending") || !strcmp(state, "freezing"));
}

#endif
