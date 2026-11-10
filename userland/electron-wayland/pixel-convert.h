#ifndef COAST_PIXEL_CONVERT_H
#define COAST_PIXEL_CONVERT_H
#include <stdint.h>
#include <emmintrin.h>

static inline uint32_t pixel_composite(uint32_t src) {
    unsigned a=src>>24,back=255-a;
    if(a==255)return src&0xffffffU;
    unsigned red=((src>>16)&255)+(0x18*back+127)/255;
    unsigned green=((src>>8)&255)+(0x20*back+127)/255;
    unsigned blue=(src&255)+(0x2a*back+127)/255;
    return (red>255?255:red)<<16 | (green>255?255:green)<<8 | (blue>255?255:blue);
}

static inline void pixel_convert(uint32_t *restrict dst,const uint32_t *restrict src,
                                  unsigned count,int argb) {
    const __m128i rgb=_mm_set1_epi32(0xffffff),alpha=_mm_set1_epi32((int)0xff000000U);
    unsigned x=0;
    for(;x+4<=count;x+=4) {
        __m128i pixels=_mm_loadu_si128((const __m128i *)(src+x));
        if(!argb || _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(pixels,alpha),alpha))==65535)
            _mm_storeu_si128((__m128i *)(dst+x),_mm_and_si128(pixels,rgb));
        else for(unsigned n=0;n<4;n++)dst[x+n]=pixel_composite(src[x+n]);
    }
    for(;x<count;x++)dst[x]=argb?pixel_composite(src[x]):src[x]&0xffffffU;
}

struct pixel_span { unsigned x, width; };
/* Compare after the same alpha conversion, in the same pass. SHM buffer
 * identities may alternate; the retained destination is the current surface,
 * rather than the previous contents of any particular client buffer. */
static inline struct pixel_span pixel_convert_damage(uint32_t *restrict dst,
        const uint32_t *restrict src,unsigned count,int argb) {
    const __m128i rgb=_mm_set1_epi32(0xffffff),alpha=_mm_set1_epi32((int)0xff000000U);
    unsigned first=count,last=0,x=0;
    for(;x+4<=count;x+=4) {
        __m128i pixels=_mm_loadu_si128((const __m128i *)(src+x));
        if(!argb || _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(pixels,alpha),alpha))==65535) {
            pixels=_mm_and_si128(pixels,rgb);
            unsigned different=(unsigned)~_mm_movemask_epi8(_mm_cmpeq_epi32(pixels,
                                _mm_loadu_si128((const __m128i *)(dst+x))))&65535;
            if(different) {
                unsigned left=x+(unsigned)__builtin_ctz(different)/4;
                if(left<first)first=left;
                last=x+(31u-(unsigned)__builtin_clz(different))/4+1;
                _mm_storeu_si128((__m128i *)(dst+x),pixels);
            }
        } else for(unsigned n=0;n<4;n++) {
            uint32_t pixel=pixel_composite(src[x+n]);
            if(dst[x+n]!=pixel){if(x+n<first)first=x+n;last=x+n+1;dst[x+n]=pixel;}
        }
    }
    for(;x<count;x++) {
        uint32_t pixel=argb?pixel_composite(src[x]):src[x]&0xffffffU;
        if(dst[x]!=pixel){if(x<first)first=x;last=x+1;dst[x]=pixel;}
    }
    return last?(struct pixel_span){first,last-first}:(struct pixel_span){0};
}
#endif
