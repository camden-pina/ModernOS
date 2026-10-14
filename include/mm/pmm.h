#ifndef _PMM_H
#define _PMM_H 1

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <queue.h>
#include <boot/info.h>

#include <mm/bitmap.h>

// TODO: switch to better allocator for large sizes
#define CHUNK_MIN_SIZE 8
#define CHUNK_MAX_SIZE 524288
#define CHUNK_SIZE_ALIGN 8
#define CHUNK_MIN_ALIGN 4

#define CHUNK_MAGIC 0xC0DE
#define HOLE_MAGIC 0xDEAD

#define PAGE_SIZE 0x1000

#define BIGPAGE_SIZE SIZE_2MB
#define HUGEPAGE_SIZE SIZE_1GB

#define FRAMEBUFFER_VA 0xFFFFBFFF00000000ULL
#define KERNEL_HEAP_VA 0xFFFFFF8000400000ULL
#define KERNEL_RESERVED_VA 0xFFFFFF8000C00000ULL

#define KERNEL_HEAP_SIZE (128 * SIZE_1MB)
#define KERNEL_HEAP_MAX_SIZE (1024 * SIZE_1MB)

#define PAGE_SHIFT 12
#define PAGES_TO_SIZE(pages) ((pages) << PAGE_SHIFT)
#define SIZE_TO_PAGES(size) (((size) >> PAGE_SHIFT) + (((size) & 0xFFF) ? 1 : 0))

/* prot flags */
#define VM_READ (1 << 0) // mapping is readable
#define VM_WRITE (1 << 1) // mapping is writable
#define VM_EXEC (1 << 2) // mapping is executable
#define VM_RDWR (VM_READ | VM_WRITE)
#define VM_RDEXC (VM_READ | VM_EXEC)

/* mode flags */
#define VM_PRIVATE (1 << 3) // mapping is private to the address space (copy-on-write)
#define VM_SHARED (1 << 4) // mapping is shared between address spaces

/* mapping flags */
#define VM_USER (1 << 5) // mapping lives in user space
#define VM_GLOBAL (1 << 6) // mapping is global in the TLB
#define VM_NOCACHE (1 << 7) // mapping is non-cacheable
#define VM_WRITETHRU (1 << 8) // mapping is write-through
#define VM_HUGE_2MB (1 << 9) // mapping uses 2M pages
#define VM_HUGE_1GB (1 << 10) // mapping uses 1G pages
#define VM_NOMAP (1 << 11) // do not make the mapping active after initial allocation

/* allocation flags */
#define VM_FIXED (1 << 12) // mapping has fixed address (hint used for address)
#define VM_STACK (1 << 13) // mapping grows downwards and has a guard page (only for VM_TYPE_PAGE)
#define VM_REPLACE (1 << 14) // mapping should replace any non-reserved mappings in the range (used with VM_FIXED)

/* internal flags */
#define VM_MALLOC (1 << 16) // mapping is a vmalloc allocation
#define VM_MAPPED (1 << 17) // mapping is currently active
#define VM_LINKED (1 << 18) // mapping was split and is linked to the following mapping
#define VM_SPLIT (1 << 19) // mapping was split and is the second half of the split

#define VM_PROT_MASK 0x7 // mask of protection flags
#define VM_MODE_MASK 0x18 // mask of mode flags
#define VM_MAP_MASK 0xFE0 // mask of mapping flags
#define VM_FLAGS_MASK 0xFFFF // mask of public flags

typedef struct mm_chunk {
    uint16_t magic;                   // magic number
    uint16_t prev_offset;             // offset to previous chunk
    uint32_t size : 31;               // size of chunk
    uint32_t free : 1;                // chunk free/used
    LIST_ENTRY(struct mm_chunk) list; // links to free chunks (if free)
} mm_chunk_t;

static_assert(sizeof(mm_chunk_t) == 24);

typedef struct mm_heap {
    uintptr_t phys_addr;          // physical address of heap
    uintptr_t virt_addr;          // virtual address of heap base
    mm_chunk_t *last_chunk;       // the last created chunk
    LIST_HEAD(mm_chunk_t) chunks; // linked list of free chunks

    size_t size; // the size of the heap
    size_t used; // the total number of bytes used
    struct {
        size_t alloc_count;    // the number of times malloc was called
        size_t free_count;     // the number of times free was called
        size_t alloc_sizes[9]; // a histogram of alloc request sizes
    } stats;
} mm_heap_t;

void init_kheap();

void pmm_init(struct boot_memory_range *entries, uint64_t entry_count);

// void* kmalloc(size_t sz);
void *krealloc(void *ptr, size_t sz);
// void kfree(void* ptr);

void *kmalloc(size_t size);
void *kmallocz(size_t size);
void *kmalloca(size_t size, size_t alignment);
void kfree(void *ptr);

uintptr_t pmm_early_alloc_pages(size_t count);
uintptr_t pmm_alloc_pages(size_t count);
void pmm_free_pages(uintptr_t start, size_t count);

void pmm_pages_unreserve(void *address, uint64_t pageCount);
void pmm_pages_reserve(void *address, uint64_t pageCount);
void pmm_pages_free(void *address, uint64_t pageCount);
void pmm_pages_lock(void *address, uint64_t pageCount);

/*
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_total_memory_used(void);
uint64_t pmm_get_total_memory_free(void);
uint64_t pmm_get_total_memory_reserved(void);
*/

#endif // _PMM_H
