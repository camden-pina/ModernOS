#ifndef _KERNEL_PGTABLE_H
#define _KERNEL_PGTABLE_H

#include <stdint.h>
#include <stddef.h>

static uint64_t *early_map_entry(uintptr_t virt_addr, intptr_t phys_addr, uint32_t vm_flags);
void *early_map_entries(uintptr_t vaddr, uintptr_t paddr, size_t count, uint32_t vm_flags);

#endif
