#include <mm/pgtable.h>
#include <mm/pmm.h>
#include <string.h>
#include <panic.h>
#include <cpu.h>

#define NUM_ENTRIES 512
#define T_ENTRY 509ULL // temp pdpt entry index
#define R_ENTRY 510ULL // recursive entry index

#define PT_INDEX(a) (((a) >> 12) & 0x1FF)
#define PDT_INDEX(a) (((a) >> 21) & 0x1FF)
#define PDPT_INDEX(a) (((a) >> 30) & 0x1FF)
#define PML4_INDEX(a) (((a) >> 39) & 0x1FF)

#define index_for_pg_level(addr, level) (((addr) >> (12 + ((level) * 9))) & 0x1FF)
#define pg_level_to_shift(level) (12 + ((level) * 9))

#define get_virt_addr(l4, l3, l2, l1) \
  ((0xFFFFULL << 48) | ((uint64_t)(l4) << 39) | ((uint64_t)(l3) << 30) | \
  ((uint64_t)(l2) << 21) | ((uint64_t)(l1) << 12))

#define PML4_PTR    ((uint64_t *) get_virt_addr(R_ENTRY, R_ENTRY, R_ENTRY, R_ENTRY))
#define TEMP_PDPT   ((uint64_t *) get_virt_addr(R_ENTRY, R_ENTRY, R_ENTRY, T_ENTRY))
#define TEMP_PDPTE  (&TEMP_PDPT[curcpu_id])
#define TEMP_PTR    ((uint64_t *) get_virt_addr(R_ENTRY, R_ENTRY, T_ENTRY, curcpu_id))

// page entry flags
#define PE_PRESENT        (1ULL << 0)
#define PE_WRITE          (1ULL << 1)
#define PE_USER           (1ULL << 2)
#define PE_WRITE_THROUGH  (1ULL << 3)
#define PE_CACHE_DISABLE  (1ULL << 4)
#define PE_ACCESSED       (1ULL << 5)
#define PE_DIRTY          (1ULL << 6)
#define PE_SIZE           (1ULL << 7)
#define PE_GLOBAL         (1ULL << 8)
#define PE_NO_EXECUTE     (1ULL << 63)

#define PE_FLAGS_MASK 0xFFF
#define PE_FRAME_MASK 0xFFFFFFFFFFFFF000

typedef enum pg_level {
  PG_LEVEL_PT,
  PG_LEVEL_PD,
  PG_LEVEL_PDP,
  PG_LEVEL_PML4,
  PG_LEVEL_MAX
} pg_level_t;

static inline uint16_t vm_flags_to_pe_flags(uint32_t vm_flags) {
    uint16_t entry_flags = PE_PRESENT;
    entry_flags |= (vm_flags & VM_WRITE) ? PE_WRITE : 0;
    entry_flags |= (vm_flags & VM_USER) ? PE_USER : 0;
    entry_flags |= (vm_flags & VM_NOCACHE) ? PE_CACHE_DISABLE : 0;
    entry_flags |= (vm_flags & VM_WRITETHRU) ? PE_WRITE_THROUGH : 0;
    entry_flags |= (vm_flags & VM_EXEC) ? 0 : PE_NO_EXECUTE;
    entry_flags |= (vm_flags & VM_GLOBAL) ? PE_GLOBAL : 0;
    if ((vm_flags & VM_HUGE_2MB) || (vm_flags & VM_HUGE_1GB)) {
      entry_flags |= PE_SIZE;
    }
    return entry_flags;
  }

static uint64_t *early_map_entry(uintptr_t virt_addr, intptr_t phys_addr, uint32_t vm_flags) {
    kassert(virt_addr % PAGE_SIZE == 0);
    kassert(phys_addr % PAGE_SIZE == 0);

    pg_level_t map_level = PG_LEVEL_PT;
    if (vm_flags & VM_HUGE_2MB) {
        kassert(is_aligned(virt_addr, SIZE_2MB));
        kassert(is_aligned(phys_addr, SIZE_2MB));
        map_level - PG_LEVEL_PD;
    }

    uint16_t entry_flags = vm_flags_to_pe_flags(vm_flags);
    uint64_t *pml4 = (void *) ((uint64_t) boot_info_v2->pml4_addr);
    uint64_t *table = pml4;

    for (pg_level_t level = PG_LEVEL_PML4; level > map_level; level--) {
        int index = index_for_pg_level(virt_addr, level);
        uintptr_t next_table = table[index] & PE_FRAME_MASK;

        if (next_table == 0) {
            // create new table
            uintptr_t new_table = pmm_early_alloc_pages(1);
            memset((void *)new_table, 0, PAGE_SIZE);
            table[index] = new_table | PE_WRITE | PE_PRESENT;
            next_table = new_table;
        } else if (!(table[index] & PE_PRESENT)) {
            table[index] = next_table | PE_WRITE | PE_PRESENT;
        }
        table = (void *)next_table;
    }
    int index = index_for_pg_level(virt_addr, map_level);
    table[index] = phys_addr | entry_flags;
    return table + index;
}

void *early_map_entries(uintptr_t vaddr, uintptr_t paddr, size_t count, uint32_t vm_flags) {
    kassert(vaddr % PAGE_SIZE == 0);
    kassert(paddr % PAGE_SIZE == 0);
    kassert(count > 0);

    pg_level_t map_level = PG_LEVEL_PT;
    size_t stride = PAGE_SIZE;

    if (vm_flags & VM_HUGE_2MB) {
        kassert(is_aligned(vaddr, SIZE_2MB));
        kassert(is_aligned(paddr, SIZE_2MB));
        map_level = PG_LEVEL_PD;
        stride = SIZE_2MB;
    }

    void *addr = (void *) vaddr;
    uint16_t entry_flags = vm_flags_to_pe_flags(vm_flags);

    while (count > 0) {
        int index = index_for_pg_level(vaddr, map_level);
        uint64_t *entry = early_map_entry(vaddr, paddr, vm_flags);
        entry++;
        count--;
        vaddr += stride;
        paddr += stride;


        for (int i = index + 1; i < NUM_ENTRIES; i++) {
            if (count == 0) {
                break;
            }

            *entry = paddr | entry_flags;
            entry++;
            count--;
            vaddr += stride;
            paddr += stride;
            cpu_invlpg(vaddr);
        }
    }
    return addr;
}