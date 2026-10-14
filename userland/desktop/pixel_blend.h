#ifndef MODERNOS_PIXEL_BLEND_H
#define MODERNOS_PIXEL_BLEND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Exact XRGB arithmetic used by the existing shadow renderer. Alpha zero
   leaves pixels untouched; positive alpha discards the unused high byte. */
void pixels_blend_scalar(uint32_t *pixels, size_t count, uint32_t color, unsigned alpha);
void pixels_blend_sse2(uint32_t *pixels, size_t count, uint32_t color, unsigned alpha);
void pixels_blend(uint32_t *pixels, size_t count, uint32_t color, unsigned alpha);
bool pixels_blend_vectorized(void);
#endif
