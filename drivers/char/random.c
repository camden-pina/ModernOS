#include <random.h>
#include <stdint.h>
#include <string.h>
#include <uapi/abi.h>

/* Use a hardware entropy source only after checking the current CPU. No
 * timer, address or deterministic seed is substituted for secure entropy.
 * A failing hardware source yields an error or a documented short read. */
static unsigned features(void) {
    unsigned a, b, c, d, result = 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (c & (1u << 30))
        result |= 1;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (a >= 7) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
        if (b & (1u << 18))
            result |= 2;
    }
    return result;
}

bool random_available(void) {
    return features() != 0;
}

long random_bytes(void *buffer, size_t size) {
    unsigned support = features();
    if (!support)
        return -E_NOTSUP;
    size_t done = 0;
    while (done < size) {
        uint64_t value = 0;
        unsigned char success = 0;
        for (unsigned attempt = 0; attempt < 64 && !success; attempt++) {
            if (support & 2)
                __asm__ volatile("rdseed %0; setc %1" : "=r"(value), "=qm"(success) : : "cc");
            if (!success && (support & 1))
                __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(success) : : "cc");
            if (!success)
                __asm__ volatile("pause");
        }
        if (!success)
            return done ? (long)done : -E_AGAIN;
        size_t count = size - done < sizeof(value) ? size - done : sizeof(value);
        memcpy((uint8_t *)buffer + done, &value, count);
        done += count;
    }
    return done;
}
