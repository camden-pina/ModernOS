#include <mm/pmm.h>
#include <mm/pgtable.h>
#include <mm/user.h>
#include <kernel.h>
#include <string.h>
#include <panic.h>
#include <sync.h>

uint64_t total_memory, total_memory_free, total_memory_used, total_memory_reserved;
static spinlock_t page_lock, heap_lock;
struct extent {
    uintptr_t start;
    size_t pages;
};

static struct extent free_ranges[2048];
static unsigned range_count;
struct page_region {
    uintptr_t start;
    size_t pages, bit;
};

static struct page_region page_regions[2048];
static unsigned page_region_count;
static uint64_t *page_bits, *page_summary;
static size_t page_count, page_words, summary_words, page_hint;
static struct boot_memory_range *memory_map;
static uint64_t memory_map_count;
mm_heap_t kheap;

static uint64_t bit_mask(unsigned first, unsigned length) {
    return length == 64 ? UINT64_MAX : ((1ULL << length) - 1) << first;
}

static void page_mark(size_t first, size_t count, bool available) {
    while (count) {
        size_t word = first / 64;
        unsigned shift = first % 64, part = min(count, 64 - shift);
        uint64_t mask = bit_mask(shift, part);
        if (available)
            page_bits[word] |= mask;
        else
            page_bits[word] &= ~mask;
        uint64_t index = 1ULL << (word % 64);
        if (page_bits[word])
            page_summary[word / 64] |= index;
        else
            page_summary[word / 64] &= ~index;
        first += part;
        count -= part;
    }
}

static size_t page_next(size_t first) {
    if (first >= page_count)
        return page_count;
    size_t word = first / 64;
    uint64_t bits = page_bits[word] & (~0ULL << (first % 64));
    if (bits)
        return word * 64 + __builtin_ctzll(bits);
    word++;
    size_t summary = word / 64;
    while (summary < summary_words) {
        uint64_t present = page_summary[summary];
        if (summary == word / 64)
            present &= ~0ULL << (word % 64);
        if (present) {
            size_t found = summary * 64 + __builtin_ctzll(present);
            if (found >= page_words)
                return page_count;
            return found * 64 + __builtin_ctzll(page_bits[found]);
        }
        summary++;
    }
    return page_count;
}

static unsigned page_region(size_t bit) {
    unsigned low = 0, high = page_region_count;
    while (low + 1 < high) {
        unsigned middle = low + (high - low) / 2;
        if (page_regions[middle].bit <= bit)
            low = middle;
        else
            high = middle;
    }
    return low;
}

static size_t page_run(size_t first, size_t limit, size_t count) {
    while ((first = page_next(first)) < limit) {
        struct page_region *region = &page_regions[page_region(first)];
        size_t end = region->bit + region->pages;
        if (count > end - first) {
            first = end;
            continue;
        }
        size_t checked = first, left = count;
        while (left) {
            unsigned shift = checked % 64, part = min(left, 64 - shift);
            uint64_t missing = (~page_bits[checked / 64]) & bit_mask(shift, part);
            if (missing) {
                first = checked - shift + __builtin_ctzll(missing) + 1;
                break;
            }
            checked += part;
            left -= part;
        }
        if (!left)
            return first;
    }
    return page_count;
}

uintptr_t pmm_alloc_pages(size_t count) {
    if (!count || count > SIZE_MAX / PAGE_SIZE)
        return 0;

    uint64_t f = spin_lock_irqsave(&page_lock);
    uintptr_t result = 0;
    if (page_bits) {
        size_t bit = page_run(page_hint, page_count, count);
        if (bit == page_count && page_hint)
            bit = page_run(0, page_hint, count);
        if (bit < page_count) {
            struct page_region *region = &page_regions[page_region(bit)];
            result = region->start + (bit - region->bit) * PAGE_SIZE;
            page_mark(bit, count, false);
            page_hint = bit + count;
            if (page_hint == page_count)
                page_hint = 0;
            total_memory_free -= count * PAGE_SIZE;
            total_memory_used += count * PAGE_SIZE;
        }
        spin_unlock_irqrestore(&page_lock, f);
        return result;
    }

    for (unsigned i = 0; i < range_count; i++) {
        if (free_ranges[i].pages < count)
            continue;
        result = free_ranges[i].start;
        free_ranges[i].start += count * PAGE_SIZE;
        free_ranges[i].pages -= count;
        if (!free_ranges[i].pages) {
            memmove(&free_ranges[i], &free_ranges[i + 1],
                    (--range_count - i) * sizeof(*free_ranges));
        }
        total_memory_free -= count * PAGE_SIZE;
        total_memory_used += count * PAGE_SIZE;
        break;
    }

    spin_unlock_irqrestore(&page_lock, f);
    return result;
}

uintptr_t pmm_early_alloc_pages(size_t count) {
    uintptr_t p = pmm_alloc_pages(count);
    if (!p)
        panic("physical memory exhausted (%zu pages)", count);
    return p;
}

void pmm_free_pages(uintptr_t start, size_t count) {
    if (!count || (start & 4095) || count > (UINTPTR_MAX - start) / PAGE_SIZE)
        panic("invalid physical page release");

    uintptr_t end = start + count * PAGE_SIZE;
    bool usable = false;
    for (uint64_t i = 0; i < memory_map_count; i++) {
        struct boot_memory_range *e = &memory_map[i];
        if (e->type == BOOT_MEMORY_USABLE && start >= e->base && end <= e->base + e->length)
            usable = true;
    }
    if (!usable)
        panic("releasing reserved physical memory");

    uint64_t f = spin_lock_irqsave(&page_lock);
    if (page_bits) {
        struct page_region *region = NULL;
        for (unsigned i = 0; i < page_region_count; i++)
            if (start >= page_regions[i].start &&
                end <= page_regions[i].start + page_regions[i].pages * PAGE_SIZE) {
                region = &page_regions[i];
                break;
            }
        if (!region)
            panic("invalid physical page region");

        size_t first = region->bit + (start - region->start) / PAGE_SIZE;
        for (size_t page = first, left = count; left;) {
            unsigned shift = page % 64, part = min(left, 64 - shift);
            if (page_bits[page / 64] & bit_mask(shift, part))
                panic("double page release");
            page += part;
            left -= part;
        }

        page_mark(first, count, true);
        page_hint = min(page_hint, first);
        total_memory_free += count * PAGE_SIZE;
        total_memory_used -= count * PAGE_SIZE;
        spin_unlock_irqrestore(&page_lock, f);
        return;
    }

    unsigned i = 0;
    while (i < range_count && free_ranges[i].start < start)
        i++;
    if ((i && free_ranges[i - 1].start + free_ranges[i - 1].pages * PAGE_SIZE > start) ||
        (i < range_count && end > free_ranges[i].start))
        panic("double page release");

    if (i && free_ranges[i - 1].start + free_ranges[i - 1].pages * PAGE_SIZE == start) {
        free_ranges[i - 1].pages += count;
        if (i < range_count && end == free_ranges[i].start) {
            free_ranges[i - 1].pages += free_ranges[i].pages;
            memmove(&free_ranges[i], &free_ranges[i + 1],
                    (--range_count - i) * sizeof(*free_ranges));
        }
    } else if (i < range_count && end == free_ranges[i].start) {
        free_ranges[i].start = start;
        free_ranges[i].pages += count;
    } else {
        if (range_count == ARRAY_SIZE(free_ranges))
            panic("physical extent table full");
        memmove(&free_ranges[i + 1], &free_ranges[i], (range_count - i) * sizeof(*free_ranges));
        free_ranges[i] = (struct extent){start, count};
        range_count++;
    }

    total_memory_free += count * PAGE_SIZE;
    total_memory_used -= count * PAGE_SIZE;
    spin_unlock_irqrestore(&page_lock, f);
}

void pmm_init(struct boot_memory_range *entries, uint64_t count) {
    memory_map = entries;
    memory_map_count = count;

    for (uint64_t i = 0; i < count; i++) {
        struct boot_memory_range *e = &entries[i];
        total_memory += e->length;
        if (e->type != BOOT_MEMORY_USABLE) {
            total_memory_reserved += e->length;
            continue;
        }

        uintptr_t start = align(max(e->base, 4096ULL), PAGE_SIZE);
        uintptr_t end = align_down(e->base + e->length, PAGE_SIZE);
        if (end <= start)
            continue;
        if (range_count == ARRAY_SIZE(free_ranges))
            panic("too many memory ranges");
        free_ranges[range_count++] = (struct extent){start, (end - start) / PAGE_SIZE};
        page_regions[page_region_count++] =
            (struct page_region){start, (end - start) / PAGE_SIZE, page_count};
        page_count += (end - start) / PAGE_SIZE;
        total_memory_free += end - start;
    }

    /* The normalized boot map is sorted by physical base. Never reclaim its responses or
       page tables while the shared kernel mappings still refer to them. */
    init_kheap();

    /* Early allocation uses the firmware's few contiguous ranges. Runtime
     * allocation uses indexed bits: freeing interleaved process/file pages
     * cannot exhaust an extent table or shift thousands of entries under IRQ
     * exclusion. Compress usable regions, so MMIO holes consume no bitmap. */
    page_words = (page_count + 63) / 64;
    summary_words = (page_words + 63) / 64;
    uint64_t *bits = kmallocz(page_words * sizeof(*bits));
    page_summary = kmallocz(summary_words * sizeof(*page_summary));
    if (!bits || !page_summary)
        panic("physical page bitmap allocation failed");

    page_bits = bits;
    for (unsigned i = 0; i < range_count; i++) {
        struct extent *range = &free_ranges[i];
        for (unsigned r = 0; r < page_region_count; r++) {
            struct page_region *region = &page_regions[r];
            if (range->start >= region->start && range->start + range->pages * PAGE_SIZE <=
                                                     region->start + region->pages * PAGE_SIZE) {
                page_mark(region->bit + (range->start - region->start) / PAGE_SIZE, range->pages,
                          true);
                break;
            }
        }
    }

    pr_info("Memory: %llu MiB available, reusable page and kernel heap allocators\n",
            (unsigned long long)(total_memory_free >> 20));
}

struct heap_block {
    size_t span;
    struct heap_block *prev, *next, *free_prev, *free_next;
    uint32_t magic, free;
};

static struct heap_block *heap_first, *heap_free;
struct heap_arena {
    uintptr_t start;
    size_t size;
};

static struct heap_arena heap_arenas[256];
static unsigned heap_arena_count;
#define HEAP_MAGIC 0x4b484541

/* Keep address-order links for coalescing and a separate free-block index.
 * Allocations must not walk every live source inode with IRQs disabled. */
static void free_remove(struct heap_block *b) {
    if (b->free_prev)
        b->free_prev->free_next = b->free_next;
    else
        heap_free = b->free_next;
    if (b->free_next)
        b->free_next->free_prev = b->free_prev;
    b->free_prev = b->free_next = NULL;
}

static void free_insert(struct heap_block *b) {
    b->free_prev = NULL;
    b->free_next = heap_free;
    if (heap_free)
        heap_free->free_prev = b;
    heap_free = b;
}

void init_kheap(void) {
    uintptr_t p = pmm_early_alloc_pages(SIZE_TO_PAGES(KERNEL_HEAP_SIZE));
    early_map_entries(KERNEL_HEAP_VA, p, SIZE_TO_PAGES(KERNEL_HEAP_SIZE), VM_RDWR);
    memset(&kheap, 0, sizeof(kheap));
    kheap.phys_addr = p;
    kheap.virt_addr = KERNEL_HEAP_VA;
    kheap.size = KERNEL_HEAP_SIZE;
    heap_first = (void *)KERNEL_HEAP_VA;
    *heap_first = (struct heap_block){.span = KERNEL_HEAP_SIZE, .magic = HEAP_MAGIC, .free = 1};
    heap_free = heap_first;
    heap_arenas[0] = (struct heap_arena){KERNEL_HEAP_VA, KERNEL_HEAP_SIZE};
    heap_arena_count = 1;
}

/* Limine's supervisor-only direct map is shared by every address space.
 * Extra arenas use it instead of adding per-process mappings or requiring
 * one contiguous, pre-reserved heap. Each arena has its own address-order
 * links: coalescing must never cross separately allocated physical ranges.
 * Lock order is heap -> physical allocator; neither allocates recursively. */
static bool heap_grow(size_t required) {
    if (heap_arena_count == ARRAY_SIZE(heap_arenas) || required > KERNEL_HEAP_MAX_SIZE - kheap.size)
        return false;
    size_t bytes = align(max(required, 8UL * SIZE_1MB), PAGE_SIZE);
    bytes = min(bytes, KERNEL_HEAP_MAX_SIZE - kheap.size);
    uintptr_t physical = pmm_alloc_pages(bytes / PAGE_SIZE);
    if (!physical && bytes > align(required, PAGE_SIZE)) {
        bytes = align(required, PAGE_SIZE);
        physical = pmm_alloc_pages(bytes / PAGE_SIZE);
    }
    if (!physical)
        return false;
    struct heap_block *block = phys_ptr(physical);
    *block = (struct heap_block){.span = bytes, .magic = HEAP_MAGIC, .free = 1};
    heap_arenas[heap_arena_count++] = (struct heap_arena){(uintptr_t)block, bytes};
    kheap.size += bytes;
    free_insert(block);
    return true;
}

static struct heap_block *split(struct heap_block *b, size_t span) {
    struct heap_block *n = (void *)((uintptr_t)b + span);
    *n = (struct heap_block){
        .span = b->span - span, .prev = b, .next = b->next, .magic = HEAP_MAGIC, .free = b->free};
    if (n->next)
        n->next->prev = n;
    b->next = n;
    b->span = span;
    return n;
}

void *kmalloca(size_t size, size_t alignment) {
    if (!size || !alignment || (alignment & (alignment - 1)))
        return NULL;

    alignment = max(alignment, 16UL);
    if (alignment > KERNEL_HEAP_MAX_SIZE / 2 ||
        size > KERNEL_HEAP_MAX_SIZE - sizeof(struct heap_block) - alignment)
        return NULL;

    size = align(size, 16UL);
    uint64_t f = spin_lock_irqsave(&heap_lock);
    void *result = NULL;

retry:
    for (struct heap_block *b = heap_free; b; b = b->free_next) {
        uintptr_t payload = align((uintptr_t)(b + 1), alignment);
        size_t prefix = payload - (uintptr_t)(b + 1);
        if (prefix && prefix < sizeof(*b) + 16) {
            payload = align(payload + sizeof(*b) + 16, alignment);
            prefix = payload - (uintptr_t)(b + 1);
        }

        size_t need = prefix + sizeof(*b) + size;
        if (need > b->span)
            continue;

        free_remove(b);
        if (prefix) {
            struct heap_block *aligned = split(b, prefix);
            free_insert(b);
            b = aligned;
        }

        if (b->span >= sizeof(*b) + size + sizeof(*b) + 16)
            free_insert(split(b, sizeof(*b) + size));
        b->free = 0;
        kheap.used += b->span;
        kheap.stats.alloc_count++;
        result = b + 1;
        break;
    }

    if (!result &&
        heap_grow(size + sizeof(struct heap_block) + alignment + sizeof(struct heap_block) + 16))
        goto retry;

    spin_unlock_irqrestore(&heap_lock, f);
    return result;
}

void *kmalloc(size_t n) {
    return kmalloca(n, 16);
}

void *kmallocz(size_t n) {
    void *p = kmalloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p)
        return;
    uint64_t f = spin_lock_irqsave(&heap_lock);
    struct heap_block *b = (struct heap_block *)p - 1;
    bool valid = false;
    for (unsigned i = 0; i < heap_arena_count; i++)
        if ((uintptr_t)b >= heap_arenas[i].start &&
            (uintptr_t)b - heap_arenas[i].start <= heap_arenas[i].size - sizeof(*b)) {
            valid = true;
            break;
        }
    if (!valid || b->magic != HEAP_MAGIC || b->free)
        panic("invalid kernel heap release");
    kheap.used -= b->span;
    kheap.stats.free_count++;
    b->free = 1;
    if (b->next && b->next->free) {
        free_remove(b->next);
        b->span += b->next->span;
        b->next = b->next->next;
        if (b->next)
            b->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        b->prev->span += b->span;
        b->prev->next = b->next;
        if (b->next)
            b->next->prev = b->prev;
    } else
        free_insert(b);
    spin_unlock_irqrestore(&heap_lock, f);
}

void *krealloc(void *p, size_t n) {
    if (!p)
        return kmalloc(n);
    if (!n) {
        kfree(p);
        return NULL;
    }
    struct heap_block *b = (struct heap_block *)p - 1;
    void *q = kmalloc(n);
    if (q) {
        memcpy(q, p, min(n, b->span - sizeof(*b)));
        kfree(p);
    }
    return q;
}

uint64_t pmm_get_total_memory(void) {
    return total_memory;
}

uint64_t pmm_get_total_memory_used(void) {
    return total_memory_used;
}

uint64_t pmm_get_total_memory_free(void) {
    return total_memory_free;
}

uint64_t pmm_get_total_memory_reserved(void) {
    return total_memory_reserved;
}
