#ifndef _BITMAP_H
#define _BITMAP_H 1

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

void bitmap_init(size_t _bitmapSize, void *_bitmapAddress);
bool bitmap_set(uint64_t _idx, bool _value);
bool bitmap_get(uint64_t _idx);

void bitmap_page_lock(void *_address);
void bitmap_page_free(void *_address);
void bitmap_page_reserve(void *_address);
void bitmap_page_unreserve(void *_address);
void *bitmap_page_request(void);

#endif // _BITMAP_H
