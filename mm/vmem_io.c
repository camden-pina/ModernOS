#include <mm/vmem_io.h>
#include <mm/pmm.h>
#include <boot/info.h>
#include <string.h>

#define PTE_P (1ull << 0)
#define PTE_RW (1ull << 1)
#define PTE_US (1ull << 2)
#define PTE_PWT (1ull << 3)
#define PTE_PCD (1ull << 4)
#define PTE_A (1ull << 5)
#define PTE_D (1ull << 6)
#define PTE_PS (1ull << 7)
#define PTE_G (1ull << 8)
#define PTE_PAT_L (1ull << 12)
#define PTE_NX (1ull << 63)
#define ADDR_MASK 0x000ffffffffff000ull
#define CACHE_4K (PTE_PWT | PTE_PCD | PTE_PS)
#define CR4_PGE (1ull << 7)
#define CR4_LA57 (1ull << 12)

static uint64_t read_cr3(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static unsigned video_large_pages;

unsigned vmem_video_large_pages(void) {
    return video_large_pages;
}

static uint64_t read_cr4(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static void flush_all_tlb(void);

void vmem_video_cpu_init(void) {
    static uint64_t boot_pat;
    static bool initialized;
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (!(d & (1u << 16)))
        return;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277u));
    uint64_t pat = lo | ((uint64_t)hi << 32);
    if (!initialized) {
        boot_pat = pat;
        initialized = true;
        return;
    }
    if (pat == boot_pat)
        return;
    /* APs must interpret shared device PTEs using the BSP's PAT. This runs
       with interrupts disabled, before the CPU publishes itself as online. */
    uint64_t cr0;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    uint64_t disabled = (cr0 | (1ULL << 30)) & ~(1ULL << 29);
    __asm__ volatile("mov %0,%%cr0; wbinvd" ::"r"(disabled) : "memory");
    __asm__ volatile("wrmsr" ::"c"(0x277u), "a"((uint32_t)boot_pat), "d"((uint32_t)(boot_pat >> 32))
                     : "memory");
    flush_all_tlb();
    __asm__ volatile("wbinvd; mov %0,%%cr0" ::"r"(cr0) : "memory");
}

static uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags)::"memory");
    return flags;
}

static void irq_restore(uint64_t flags) {
    if (flags & (1ull << 9))
        __asm__ volatile("sti" ::: "memory");
}

static void flush_all_tlb(void) {
    uint64_t cr4 = read_cr4();
    if (cr4 & CR4_PGE) {
        uint64_t temporary = cr4 & ~CR4_PGE;
        __asm__ volatile("mov %0, %%cr4; mov %1, %%cr4" ::"r"(temporary), "r"(cr4) : "memory");
    } else {
        uint64_t cr3 = read_cr3();
        __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
    }
}

static void flush_caches(void) {
    __asm__ volatile("wbinvd" ::: "memory");
}

static uint64_t *table_at(uint64_t phys) {
    if (!boot_info.hhdm_offset || !phys || (phys & 0xfffull) ||
        phys > UINT64_MAX - boot_info.hhdm_offset)
        return NULL;
    return (uint64_t *)(uintptr_t)(phys + boot_info.hhdm_offset);
}

static bool canonical(uint64_t address) {
    return address <= 0x00007fffffffffffull || address >= 0xffff800000000000ull;
}

static uint64_t new_table(void) {
    uint64_t phys = pmm_early_alloc_pages(1);
    uint64_t *table = table_at(phys);
    if (!table || (phys & ~ADDR_MASK))
        return 0;
    memset(table, 0, PAGE_SIZE);
    return phys;
}

/* level 2: PDPTE (1 GiB); level 1: PDE (2 MiB). */
static bool split_leaf(uint64_t *entry, unsigned level) {
    if (level != 1 && level != 2)
        return false;
    uint64_t old = *entry;
    uint64_t phys = new_table();
    uint64_t *children = table_at(phys);
    if (!children)
        return false;

    unsigned shift = 12u + level * 9u;
    uint64_t base = (old & ADDR_MASK) & ~((1ull << shift) - 1ull);
    uint64_t flags = old & ~ADDR_MASK;
    flags &= ~PTE_PS;
    if (level == 2) {
        flags |= PTE_PS;
        if (old & PTE_PAT_L)
            flags |= PTE_PAT_L;
    } else if (old & PTE_PAT_L) {
        /* Huge-leaf PAT bit 12 becomes 4 KiB-leaf PAT bit 7. */
        flags |= PTE_PS;
    }
    uint64_t stride = 1ull << (shift - 9u);
    for (unsigned i = 0; i < 512; ++i)
        children[i] = (base + i * stride) | flags;

    /* Leaf cache bits must not become page-table cache bits. */
    *entry = phys | PTE_P | PTE_RW | (old & (PTE_US | PTE_A | PTE_NX));
    return true;
}

static uint64_t *walk_4k(uint64_t root, uint64_t virt, bool create) {
    uint64_t *table = table_at(root & ADDR_MASK);
    if (!table || !canonical(virt))
        return NULL;
    for (unsigned level = 3; level; --level) {
        uint64_t *entry = &table[(virt >> (12u + 9u * level)) & 511u];
        if (!(*entry & PTE_P)) {
            if (!create)
                return NULL;
            uint64_t phys = new_table();
            if (!phys)
                return NULL;
            *entry = phys | PTE_P | PTE_RW;
        } else if (*entry & PTE_PS) {
            if (!split_leaf(entry, level))
                return NULL;
        }
        *entry |= PTE_RW;
        table = table_at(*entry & ADDR_MASK);
        if (!table)
            return NULL;
    }
    return &table[(virt >> 12) & 511u];
}

static bool translation(uint64_t root, uint64_t virt, uint64_t *physical) {
    uint64_t *table = table_at(root & ADDR_MASK);
    if (!table || !canonical(virt))
        return false;
    for (unsigned level = 3;; --level) {
        uint64_t entry = table[(virt >> (12u + level * 9u)) & 511u];
        if (!(entry & PTE_P))
            return false;
        if (!level || (level < 3 && (entry & PTE_PS))) {
            unsigned shift = 12u + level * 9u;
            uint64_t mask = (1ull << shift) - 1ull;
            *physical = ((entry & ADDR_MASK) & ~mask) | (virt & mask);
            return true;
        }
        if (entry & PTE_PS)
            return false; /* Reserved PS bit in a PML4E. */
        table = table_at(entry & ADDR_MASK);
        if (!table)
            return false;
    }
}

bool vmem_translate(uint64_t virt, uint64_t *phys) {
    return phys && !(read_cr4() & CR4_LA57) && translation(read_cr3() & ADDR_MASK, virt, phys);
}

static bool uc_flags(uint64_t *flags) {
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (!(d & (1u << 16))) /* PAT absent: legacy PCD+PWT selects UC. */ {
        *flags = PTE_PCD | PTE_PWT;
        return true;
    }
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x277u));
    uint64_t pat = low | ((uint64_t)high << 32);
    for (unsigned i = 0; i < 8; ++i) {
        if (((pat >> (8u * i)) & 0xffu) == 0) {
            *flags = ((i & 1u) ? PTE_PWT : 0) | ((i & 2u) ? PTE_PCD : 0) | ((i & 4u) ? PTE_PS : 0);
            return true;
        }
    }
    return false; /* Do not silently modify the machine-wide PAT. */
}

static bool already_uc(uint64_t root, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *table = table_at(root & ADDR_MASK);
    if (!table)
        return false;
    for (unsigned level = 3;; --level) {
        uint64_t entry = table[(virt >> (12u + level * 9u)) & 511u];
        if ((entry & (PTE_P | PTE_RW)) != (PTE_P | PTE_RW))
            return false;
        if (!level || (level < 3 && (entry & PTE_PS))) {
            unsigned shift = 12u + level * 9u;
            uint64_t mask = (1ull << shift) - 1ull;
            uint64_t address = ((entry & ADDR_MASK) & ~mask) | (virt & mask);
            uint64_t cache = entry & (PTE_PWT | PTE_PCD);
            if (entry & (level ? PTE_PAT_L : PTE_PS))
                cache |= PTE_PS;
            return address == phys && cache == flags;
        }
        if (entry & PTE_PS)
            return false;
        table = table_at(entry & ADDR_MASK);
        if (!table)
            return false;
    }
}

bool vmem_map_page_in_root(uint64_t root, uint64_t virt, uint64_t phys) {
    if ((read_cr4() & CR4_LA57) || ((virt | phys) & 0xfffull) || (phys & ~ADDR_MASK))
        return false;
    uint64_t flags = irq_save();
    uint64_t *entry = walk_4k(root, virt, true);
    bool success = entry != NULL;
    if (entry) {
        uint64_t attributes = 0;
        if ((*entry & PTE_P) && ((*entry & ADDR_MASK) == phys))
            attributes = *entry & ~ADDR_MASK;
        *entry = phys | attributes | PTE_P | PTE_RW;
        /* Also invalidates any shared higher-half table in active CR3. */
        flush_all_tlb();
    }
    irq_restore(flags);
    return success;
}

bool vmem_copy_page_cache_in_root(uint64_t root, uint64_t target, uint64_t source) {
    if ((read_cr4() & CR4_LA57) || ((target | source) & 0xfffull))
        return false;
    uint64_t saved = irq_save();
    uint64_t *from = walk_4k(root, source, false);
    uint64_t *to = walk_4k(root, target, false);
    bool success = from && to && (*from & PTE_P) && (*to & PTE_P) &&
                   ((*from & ADDR_MASK) == (*to & ADDR_MASK));
    if (success && ((*from ^ *to) & CACHE_4K)) {
        flush_caches();
        *to = (*to & ~CACHE_4K) | (*from & CACHE_4K);
        flush_all_tlb();
        flush_caches();
    } else {
        /* Walking may have split an existing huge leaf. */
        flush_all_tlb();
    }
    irq_restore(saved);
    return success;
}

/* All device pointers live in the shared higher half. Map MMIO only during
   BSP driver discovery, before user address spaces and APs are started. */
static void *map_device(uint64_t phys, size_t bytes, uint64_t cache) {
    if (!boot_info.hhdm_offset || !bytes || (read_cr4() & CR4_LA57) ||
        phys >= 0x0000800000000000ull || bytes > 0x0000800000000000ull - phys)
        return NULL;
    uint64_t hhdm = boot_info.hhdm_offset;
    uint64_t first = phys & ~4095ULL, last = (phys + bytes - 1) & ~4095ULL;
    if (!canonical(last + hhdm))
        return NULL;
    uint64_t root = read_cr3() & ADDR_MASK, saved = irq_save();
    bool changes = false;
    for (uint64_t page = first;; page += PAGE_SIZE) {
        uint64_t existing;
        if (translation(root, page + hhdm, &existing) && existing != page) {
            irq_restore(saved);
            return NULL;
        }
        if (!already_uc(root, page + hhdm, page, cache))
            changes = true;
        if (page == last)
            break;
    }
    bool success = true;
    if (changes) {
        flush_caches();
        for (uint64_t page = first;; page += PAGE_SIZE) {
            uint64_t *entry = walk_4k(root, page + hhdm, true);
            if (!entry) {
                success = false;
                break;
            }
            *entry = page | PTE_P | PTE_RW | PTE_NX | cache;
            if (page == last)
                break;
        }
        flush_all_tlb();
        flush_caches();
    }
    irq_restore(saved);
    return success ? (void *)(uintptr_t)(phys + hhdm) : NULL;
}

void *vmem_map_mmio(uint64_t phys, size_t bytes) {
    uint64_t cache;
    return uc_flags(&cache) ? map_device(phys, bytes, cache) : NULL;
}

static unsigned coalesce_video(uint64_t phys, size_t bytes) {
    const uint64_t huge = 2ULL * 1024 * 1024;
    uint64_t first = (phys + huge - 1) & ~(huge - 1), last = (phys + bytes) & ~(huge - 1);
    uint64_t hhdm = boot_info.hhdm_offset;
    if (hhdm & (huge - 1))
        return 0;
    uint64_t saved = irq_save(), root = read_cr3() & ADDR_MASK;
    unsigned count = 0;
    for (uint64_t page = first; page < last; page += huge) {
        uint64_t va = page + hhdm, *table = table_at(root);
        for (unsigned level = 3; table && level > 1; level--) {
            uint64_t entry = table[(va >> (12 + 9 * level)) & 511];
            if (!(entry & PTE_P) || (entry & PTE_PS)) {
                table = NULL;
                break;
            }
            table = table_at(entry & ADDR_MASK);
        }
        if (!table)
            continue;
        uint64_t *entry = &table[(va >> 21) & 511], old = *entry;
        if (!(old & PTE_P))
            continue;
        if (old & PTE_PS) {
            count++;
            continue;
        }
        uint64_t *leaves = table_at(old & ADDR_MASK);
        if (!leaves)
            continue;
        uint64_t flags = leaves[0] & ~ADDR_MASK, accessed = 0;
        bool same = true;
        for (unsigned i = 0; i < 512; i++) {
            uint64_t leaf = leaves[i];
            if (!(leaf & PTE_P) || (leaf & PTE_US) || (leaf & ADDR_MASK) != page + i * PAGE_SIZE ||
                ((leaf ^ flags) & ~(ADDR_MASK | PTE_A | (1ULL << 6)))) {
                same = false;
                break;
            }
            accessed |= leaf & (PTE_A | (1ULL << 6));
        }
        if (!same)
            continue;
        uint64_t attributes = (flags & ~PTE_PS) | accessed | PTE_PS;
        if (flags & PTE_PS)
            attributes |= PTE_PAT_L;
        *entry = page | attributes;
        /* Page tables inherited from firmware must not be returned to the
           allocator. Retain this bounded obsolete table (4 KiB per 2 MiB).
           Discovery runs before APs and user CR3s can retain old translations. */
        count++;
    }
    if (count)
        flush_all_tlb();
    irq_restore(saved);
    return count;
}

void *vmem_map_video(uint64_t phys, size_t bytes, bool *write_combining) {
    uint64_t cache;
    if (write_combining)
        *write_combining = false;
    if (!uc_flags(&cache))
        return NULL;
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (d & (1u << 16)) {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277u));
        uint64_t pat = lo | ((uint64_t)hi << 32);
        for (unsigned i = 0; i < 8; i++)
            if (((pat >> (i * 8)) & 255) == 1) {
                cache = ((i & 1) ? PTE_PWT : 0) | ((i & 2) ? PTE_PCD : 0) | ((i & 4) ? PTE_PS : 0);
                if (write_combining)
                    *write_combining = true;
                break;
            }
    }
    void *mapped = map_device(phys, bytes, cache);
    if (mapped)
        video_large_pages = coalesce_video(phys, bytes);
    return mapped;
}
