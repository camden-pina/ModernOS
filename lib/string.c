#include <panic.h>
#include <string.h>

int memcmp(const void *str1, const void *str2, size_t count) {
    const unsigned char *s1 = str1;
    const unsigned char *s2 = str2;

    while (count-- > 0) {
        if (*s1++ != *s2++) {
            return s1[-1] < s2[-1] ? -1 : 1;
        }
    }
    return 0;
}

void *memcpy(void *dest, const void *src, size_t len) {
    void *result = dest;
    size_t words = len / 8, tail = len % 8;
    /* No SIMD/FPU state or overread: safe for exact user-copy spans and MMIO. */
    __asm__ volatile("rep movsq" : "+D"(dest), "+S"(src), "+c"(words)::"memory");
    __asm__ volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(tail)::"memory");
    return result;
}

void *memmove(void *dest, const void *src, size_t len) {
    if (len == 0) {
        return dest;
    }

    char *d = dest;
    const char *s = src;
    if (d < s) {
        while (len--) {
            *d++ = *s++;
        }
    } else {
        const char *lasts = s + (len - 1);
        char *lastd = d + (len - 1);
        while (len--) {
            *lastd-- = *lasts--;
        }
    }
    return dest;
}

void *memset(void *dest, int val, size_t len) {
    void *result = dest;
    uint64_t pattern = (uint8_t)val * 0x0101010101010101ULL;
    size_t words = len / 8, tail = len % 8;
    /* Stack/image/page initialization must not emit one C loop per byte.
       Exact integer-only spans are also safe before FPU initialization. */
    __asm__ volatile("rep stosq" : "+D"(dest), "+c"(words) : "a"(pattern) : "memory");
    __asm__ volatile("rep stosb" : "+D"(dest), "+c"(tail) : "a"((uint8_t)val) : "memory");
    return result;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1) {
        if (*s1 != *s2) {
            break;
        }
        s1++;
        s2++;
    }

    return *s1 - *s2;
}

// Adapted from: https://github.com/gcc-mirror/gcc/blob/master/libiberty/strncmp.c
int strncmp(const char *s1, const char *s2, size_t n) {
    unsigned char u1, u2;

    while (n-- > 0) {
        u1 = (unsigned char)*s1++;
        u2 = (unsigned char)*s2++;
        if (u1 != u2) {
            return u1 - u2;
        }

        if (u1 == '\0') {
            return 0;
        }
    }
    return 0;
}

int strlen(const char *s) {
    int len = 0;
    while (*s != 0) {
        len++;
        s++;
    }
    return len;
}

void *strcpy(char *dest, const char *src) {
    size_t len = strlen(src);
    char *d = dest;
    const char *s = src;
    while (len--) {
        *d++ = *s++;
    }
    *d = '\0';
    return dest;
}

void *memchr(const void *ptr, int c, size_t size) {
    const unsigned char *p = ptr;
    while (size--) {
        if (*p == (unsigned char)c) {
            return (void *)p;
        }
        p++;
    }
    return NULL;
}
