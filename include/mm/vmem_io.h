#ifndef VMEM_IO_H
#define VMEM_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* BSP initialization only, before SMP/user tasks. Returns a shared
 * higher-half UC device mapping, never an identity-mapped pointer. */
void *vmem_map_mmio(uint64_t phys, size_t bytes);
/* Existing PAT WC entries are reused; the PAT layout is never repurposed.
   Video aliases must use the same cache attributes, before SMP starts. */
void *vmem_map_video(uint64_t phys, size_t bytes, bool *write_combining);
bool vmem_translate(uint64_t virt, uint64_t *phys);
void vmem_video_cpu_init(void);
unsigned vmem_video_large_pages(void);

/* Used by vmem.c while constructing its replacement PML4. Preserves the
 * cache attributes of an existing mapping of the same physical page.
 * Splits 1 GiB/2 MiB leaves rather than walking them as page-table pointers.
 */
bool vmem_map_page_in_root(uint64_t root_phys, uint64_t virt, uint64_t phys);

/* Match the cache attributes of two existing aliases of one physical page. */
bool vmem_copy_page_cache_in_root(uint64_t root_phys, uint64_t target, uint64_t source);

#endif
