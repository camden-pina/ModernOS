#include <mm/vmem.h>
#include <mm/pmm.h>
#include <mm/vmem_io.h>
#include <boot/info.h>
#include <string.h>
#include <8250.h>
#include <panic.h>

static uint64_t pml4_phys;

void vmem_memory_map(void *virtual_address, void *physical_address) {
    /* Boot adapters can provide 4 KiB, 2 MiB or 1 GiB leaves. The shared
       walker splits huge leaves and retains existing device cache attributes. */
    if (!vmem_map_page_in_root(pml4_phys, (uintptr_t)virtual_address, (uintptr_t)physical_address))
        panic("cannot establish kernel page mapping");
}

void vmem_init(uint64_t fb_base, uint64_t fb_size) {
    pml4_phys = pmm_early_alloc_pages(1);
    uint64_t *pml4 = (void *)(uintptr_t)(pml4_phys + boot_info.hhdm_offset);
    memset(pml4, 0, PAGE_SIZE);

    /* Keep the boot adapter's kernel/direct mappings; discard identity maps
       before creating user address spaces. Shared tables remain reserved. */
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3,%0" : "=r"(current_cr3));
    uint64_t *active =
        (void *)(uintptr_t)((current_cr3 & 0x000ffffffffff000ULL) + boot_info.hhdm_offset);
    for (unsigned i = 256; i < 512; i++)
        pml4[i] = active[i];

    if (fb_base && fb_size) {
        uint64_t hhdm = boot_info.hhdm_offset;
        uint64_t physical = fb_base >= hhdm ? fb_base - hhdm : fb_base;
        fb_base = physical + hhdm;
        uint64_t virtual_page = fb_base & ~(PAGE_SIZE - 1ULL);
        uint64_t physical_page = physical & ~(PAGE_SIZE - 1ULL);
        uint64_t bytes = fb_size + (fb_base - virtual_page);
        for (uint64_t offset = 0; offset < bytes; offset += PAGE_SIZE)
            vmem_memory_map((void *)(uintptr_t)(virtual_page + offset),
                            (void *)(uintptr_t)(physical_page + offset));
    }
    __asm__ volatile("cli; mov %0,%%cr3" ::"r"(pml4_phys) : "memory");
    serial_port_write("vmem_init() #5 - Paging Online\n");
}
