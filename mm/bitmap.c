#include <mm/bitmap.h>

uint8_t *bitmap_start;
uint64_t bitmap_size;
uint64_t page_bitmap_idx;

void bitmap_init(size_t _bitmap_size, void *_bitmapAddress) {
    bitmap_start = _bitmapAddress;
    bitmap_size = _bitmap_size;
    page_bitmap_idx = 0;

    memset(bitmap_start, 0, _bitmap_size);
}

bool bitmap_get(uint64_t _idx) {
    if (_idx > (bitmap_size * 8))
        return false;
    uint64_t byteIdx = _idx / 8;
    uint8_t bitIdx = _idx % 8;
    uint8_t bitMask = 0x80 >> bitIdx;

    if ((bitmap_start[byteIdx] & bitMask) > 0)
        return true;
    return false;
}

bool bitmap_set(uint64_t _idx, bool _value) {
    if (_idx > (bitmap_size * 8))
        return false;
    uint64_t byteIdx = _idx / 8;
    uint8_t bitIdx = _idx % 8;
    uint8_t bitMask = 0x80 >> bitIdx;

    bitmap_start[byteIdx] &= ~bitMask;

    if (_value)
        bitmap_start[byteIdx] |= bitMask;
    return true;
}

extern uint64_t total_memory_free;
extern uint64_t total_memory_used;
extern uint64_t total_memory_reserved;

void bitmap_page_lock(void *_address) {
    uint64_t idx = (uint64_t)_address / 4096;

    if (bitmap_get(idx)) // page is already free
        return;

    if (bitmap_set(idx, true)) {
        total_memory_free -= 0x1000;
        total_memory_used += 0x1000;
    }
}

void bitmap_page_free(void *_address) {
    uint64_t idx = (uint64_t)_address / 4096;

    if (!bitmap_get(idx)) // page is already free
        return;

    if (bitmap_set(idx, false)) {
        total_memory_free += 0x1000;
        total_memory_used -= 0x1000;

        if (page_bitmap_idx > idx)
            page_bitmap_idx = idx;
    }
}

void bitmap_page_reserve(void *_address) {
    uint64_t idx = (uint64_t)_address / 4096;

    if (bitmap_get(idx)) // page is already free
        return;

    if (bitmap_set(idx, true)) {
        total_memory_free -= 0x1000;
        total_memory_reserved += 0x1000;
    }
}

void bitmap_page_unreserve(void *_address) {
    uint64_t idx = (uint64_t)_address / 4096;

    if (!bitmap_get(idx)) // page is already free
        return;

    if (bitmap_set(idx, false)) {
        total_memory_free += 0x1000;
        total_memory_reserved -= 0x1000;

        if (page_bitmap_idx > idx)
            page_bitmap_idx = idx;
    }
}

void *bitmap_page_request(void) {
    for (; page_bitmap_idx < (bitmap_size * 8); page_bitmap_idx++) {
        if (bitmap_get(page_bitmap_idx))
            continue;
        bitmap_page_lock((void *)(page_bitmap_idx * 0x1000));
        return (void *)(page_bitmap_idx * 0x1000);
    }
    return NULL; // Page frame swap to file
}
