#ifndef _VMEM_H
#define _VMEM_H 1

#include <stdint.h>
#include <stdbool.h>

#include <mm/bitmap.h>
#include <mm/pmm.h>

#define PAGE_SIZE 0x1000

void vmem_memory_map(void *vAddr, void *pAddr);
void vmem_init(uint64_t fb_base, uint64_t fb_size);

#endif // _VMEM_H
