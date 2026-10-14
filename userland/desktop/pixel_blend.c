#include "pixel_blend.h"

void pixels_blend_scalar(uint32_t *p, size_t count, uint32_t color, unsigned alpha) {
    if (!alpha)
        return;
    uint64_t inverse = 255 - alpha;
    uint64_t rb = (color & 0xff00ff) * alpha + 0x800080, g = (color & 0x00ff00) * alpha + 0x008000;
    rb |= rb << 32;
    g |= g << 32;
    while (count >= 2) {
        uint64_t background;
        __builtin_memcpy(&background, p, sizeof(background));
        uint64_t value =
            ((((background & 0x00ff00ff00ff00ffULL) * inverse + rb) >> 8) & 0x00ff00ff00ff00ffULL) |
            ((((background & 0x0000ff000000ff00ULL) * inverse + g) >> 8) & 0x0000ff000000ff00ULL);
        __builtin_memcpy(p, &value, sizeof(value));
        p += 2;
        count -= 2;
    }

    if (count) {
        unsigned inverse32 = 255 - alpha;
        *p = ((((*p & 0xff00ff) * inverse32 + (color & 0xff00ff) * alpha + 0x800080) >> 8) &
              0xff00ff) |
             ((((*p & 0x00ff00) * inverse32 + (color & 0x00ff00) * alpha + 0x008000) >> 8) &
              0x00ff00);
    }
}

void pixels_blend_sse2(uint32_t *pixels, size_t count, uint32_t color, unsigned alpha) {
    if (!alpha)
        return;
    if (count >= 4) {
        uint64_t inverse = (255 - alpha) * 0x0001000100010001ULL;
        uint64_t foreground = ((uint64_t)((color & 255) * alpha + 128)) |
                              ((uint64_t)(((color >> 8) & 255) * alpha + 128) << 16) |
                              ((uint64_t)(((color >> 16) & 255) * alpha + 128) << 32) |
                              (128ULL << 48);
        /* Four exact pixels per iteration. Unaligned loads/stores never cross
           count, and constants stay in registers for the entire span. SSE2
           is mandatory on x86-64; ordinary task FPU preservation applies. */
        __asm__ volatile("movq %[inverse],%%xmm2\n\t"
                         "punpcklqdq %%xmm2,%%xmm2\n\t"
                         "movq %[foreground],%%xmm3\n\t"
                         "punpcklqdq %%xmm3,%%xmm3\n\t"
                         "pxor %%xmm4,%%xmm4\n\t"
                         "pcmpeqd %%xmm5,%%xmm5\n\t"
                         "psrld $8,%%xmm5\n\t"
                         "1:\n\t"
                         "movdqu (%[pixels]),%%xmm0\n\t"
                         "movdqa %%xmm0,%%xmm1\n\t"
                         "punpcklbw %%xmm4,%%xmm0\n\t"
                         "punpckhbw %%xmm4,%%xmm1\n\t"
                         "pmullw %%xmm2,%%xmm0\n\t"
                         "pmullw %%xmm2,%%xmm1\n\t"
                         "paddw %%xmm3,%%xmm0\n\t"
                         "paddw %%xmm3,%%xmm1\n\t"
                         "psrlw $8,%%xmm0\n\t"
                         "psrlw $8,%%xmm1\n\t"
                         "packuswb %%xmm1,%%xmm0\n\t"
                         "pand %%xmm5,%%xmm0\n\t"
                         "movdqu %%xmm0,(%[pixels])\n\t"
                         "add $16,%[pixels]\n\t"
                         "sub $4,%[count]\n\t"
                         "cmp $4,%[count]\n\t"
                         "jae 1b"
                         : [pixels] "+r"(pixels), [count] "+r"(count)
                         : [inverse] "r"(inverse), [foreground] "r"(foreground)
                         : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "cc", "memory");
    }

    pixels_blend_scalar(pixels, count, color, alpha);
}

bool pixels_blend_vectorized(void) {
    static int enabled = -1;
    if (enabled < 0) {
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        enabled = 1;
        if (c & (1u << 31)) {
            __asm__ volatile("cpuid"
                             : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(0x40000000), "c"(0));
            /* QEMU's official TCG signature. Keep the faster integer path
               under software emulation; KVM and physical CPUs use SSE2.
               https://gitlab.com/qemu-project/qemu/-/blob/master/target/i386/cpu.c */
            if (b == 0x54474354 && c == 0x43544743 && d == 0x47435447)
                enabled = 0;
        }
    }

    return enabled != 0;
}

void pixels_blend(uint32_t *pixels, size_t count, uint32_t color, unsigned alpha) {
    if (pixels_blend_vectorized())
        pixels_blend_sse2(pixels, count, color, alpha);
    else
        pixels_blend_scalar(pixels, count, color, alpha);
}
