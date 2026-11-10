#ifndef COAST_REGION_BITMAP_H
#define COAST_REGION_BITMAP_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Update an exact, half-open bit span. Whole bytes replace the old per-pixel
   read/modify/write loop; partial endpoints retain every neighboring bit. */
static inline void region_span(uint8_t *bits,size_t first,size_t end,int add) {
    if(first>=end)return;
    while(first<end && (first&7)) {
        uint8_t mask=(uint8_t)(1u<<(first&7));
        if(add)bits[first>>3]|=mask;else bits[first>>3]&=(uint8_t)~mask;
        first++;
    }
    size_t whole=(end-first)>>3;
    if(whole)memset(bits+(first>>3),add?255:0,whole);
    first+=whole*8;
    while(first<end) {
        uint8_t mask=(uint8_t)(1u<<(first&7));
        if(add)bits[first>>3]|=mask;else bits[first>>3]&=(uint8_t)~mask;
        first++;
    }
}
#endif
