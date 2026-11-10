#ifndef COAST_PIXEL_DAMAGE_H
#define COAST_PIXEL_DAMAGE_H
#include <stdint.h>

typedef struct { unsigned x, y, width, height; } PixelDamage;

/* SSE2 is part of the x86-64 user ABI. Every load stays inside one row. */
static inline int pixel_equal4(const uint32_t *a, const uint32_t *b) {
    unsigned mask;
    __asm__("movdqu (%1),%%xmm0\n\t"
            "movdqu (%2),%%xmm1\n\t"
            "pcmpeqb %%xmm1,%%xmm0\n\t"
            "pmovmskb %%xmm0,%0"
            : "=r"(mask) : "r"(a), "r"(b) : "xmm0", "xmm1", "memory");
    return mask == 65535;
}

static inline PixelDamage pixel_damage(const uint32_t *old, const uint32_t *now,
                                        unsigned width, unsigned height) {
    if (!width || !height) return (PixelDamage){0};
    unsigned left=width, top=height, right=0, bottom=0;
    for (unsigned y=0; y<height; y++) {
        const uint32_t *a=old+(uint64_t)y*width, *b=now+(uint64_t)y*width;
        unsigned x=0, end=width;
        while (x+4<=width && pixel_equal4(a+x,b+x)) x+=4;
        while (x<width && a[x]==b[x]) x++;
        if (x==width) continue;
        while (end>=x+4 && pixel_equal4(a+end-4,b+end-4)) end-=4;
        while (end>x && a[end-1]==b[end-1]) end--;
        if (x<left) left=x;
        if (end>right) right=end;
        if (y<top) top=y;
        bottom=y+1;
    }
    return bottom ? (PixelDamage){left,top,right-left,bottom-top} : (PixelDamage){0};
}
#endif
