#ifndef COAST_SHA256_H
#define COAST_SHA256_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
struct sha256 {
    uint32_t state[8];
    uint64_t bytes;
    size_t used;
    uint8_t block[64];
};
void sha256_init(struct sha256 *);
bool sha256_update(struct sha256 *, const void *, size_t);
void sha256_final(struct sha256 *, uint8_t digest[32]);
#endif
