#include <mm/user.h>
#include <mm/pmm.h>
#include <mm/vmem_io.h>
#include <string.h>
#include <uapi/abi.h>
#include <boot/info.h>
#include <sync.h>
#include <fs/memfd.h>

#define MASK 0x000ffffffffff000ULL
#define P 1ULL
#define W 2ULL
#define U 4ULL
#define NX (1ULL << 63)
#define RESERVED (1ULL << 9)
#define SHARED (1ULL << 10)
#define SHARED_RO (1ULL << 11)

static uint64_t master_root;

void *phys_ptr(uint64_t p) {
    return (void *)(p + boot_info.hhdm_offset);
}

uint64_t kernel_cr3(void) {
    if (!master_root)
        __asm__ volatile("mov %%cr3,%0" : "=r"(master_root));
    return master_root & MASK;
}

void user_cpu_init(void) {
    vmem_video_cpu_init();
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0,%0; mov %%cr4,%1" : "=r"(cr0), "=r"(cr4));
    cr0 = (cr0 & ~12ULL) | (1ULL << 16) | 2; /* MP, WP; clear EM/TS */
    /* FXSAVE context: do not expose AVX/FSGSBASE to this initial ABI. */
    cr4 = (cr4 | (3ULL << 9)) & ~((1ULL << 18) | (1ULL << 16));
    __asm__ volatile("mov %0,%%cr0; mov %1,%%cr4; fninit" ::"r"(cr0), "r"(cr4) : "memory");
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000080));
    lo |= 0x801; /* NXE and SCE */
    __asm__ volatile("wrmsr" ::"a"(lo), "d"(hi), "c"(0xc0000080) : "memory");
}

static uint64_t table_new(void) {
    uint64_t p = pmm_alloc_pages(1);
    if (p)
        memset(phys_ptr(p), 0, PAGE_SIZE);
    return p;
}

bool user_space_create(struct address_space *s) {
    memset(s, 0, sizeof(*s));
    s->root = table_new();
    if (!s->root)
        return false;

    s->vm = kmalloc(sizeof(*s->vm));
    if (!s->vm) {
        pmm_free_pages(s->root, 1);
        memset(s, 0, sizeof(*s));
        return false;
    }

    memset(s->vm, 0, sizeof(*s->vm));
    s->vm->references = 1;
    s->vm->generation = 1;

    uint64_t *root = phys_ptr(s->root), *kernel = phys_ptr(kernel_cr3());
    for (unsigned i = 256; i < 512; i++)
        root[i] = kernel[i] & ~U;
    s->vm->mmap_next = 0x100000000ULL;
    return true;
}

static void destroy_table(uint64_t p, unsigned level) {
    uint64_t *t = phys_ptr(p);
    for (unsigned i = 0; i < 512; i++)
        if ((t[i] & P) || (!level && (t[i] & RESERVED))) {
            uint64_t child = t[i] & MASK;
            if (level)
                destroy_table(child, level - 1);
            else if (t[i] & SHARED)
                memfd_unmap_page(child, t[i] & W);
            else
                pmm_free_pages(child, 1);
        }
    pmm_free_pages(p, 1);
}

void user_space_destroy(struct address_space *s) {
    if (!s->root)
        return;
    if (__atomic_fetch_sub(&s->vm->references, 1, __ATOMIC_ACQ_REL) != 1) {
        memset(s, 0, sizeof(*s));
        return;
    }

    uint64_t *t = phys_ptr(s->root);
    for (unsigned i = 0; i < 256; i++)
        if (t[i] & P)
            destroy_table(t[i] & MASK, 2);

    pmm_free_pages(s->root, 1);
    kfree(s->vm);
    memset(s, 0, sizeof(*s));
}

void user_space_share(struct address_space *out, const struct address_space *source) {
    *out = *source;
    __atomic_fetch_add(&out->vm->references, 1, __ATOMIC_ACQ_REL);
}

static bool clone_table(uint64_t target, uint64_t source, unsigned level, size_t *pages) {
    uint64_t *to = phys_ptr(target), *from = phys_ptr(source);
    unsigned limit = level == 3 ? 256 : 512;
    for (unsigned i = 0; i < limit; i++) {
        uint64_t entry = from[i];
        if (!(entry & P) && (level || !(entry & RESERVED)))
            continue;
        if ((level && (entry & 128)) || !(entry & U))
            return false;
        if (!level && ++*pages > 262144)
            return false; /* bounded 1 GiB fork copy */

        if (!level && (entry & SHARED)) {
            if (!memfd_retain_page(entry & MASK, entry & W))
                return false;
            to[i] = entry;
            continue;
        }

        uint64_t copy = table_new();
        if (!copy)
            return false;
        to[i] = copy | (entry & ~MASK);
        if (level) {
            if (!clone_table(copy, entry & MASK, level - 1, pages))
                return false;
        } else
            memcpy(phys_ptr(copy), phys_ptr(entry & MASK), PAGE_SIZE);
    }

    return true;
}

bool user_space_clone(struct address_space *out, const struct address_space *source) {
    if (!user_space_create(out))
        return false;

    size_t pages = 0;
    if (!clone_table(out->root, source->root, 3, &pages)) {
        user_space_destroy(out);
        return false;
    }

    out->vm->brk_base = source->vm->brk_base;
    out->vm->brk_end = source->vm->brk_end;
    out->vm->mmap_next = source->vm->mmap_next;
    out->vm->pages = source->vm->pages;
    out->vm->page_limit = source->vm->page_limit;
    out->vm->peak_pages = out->vm->pages;
    return true;
}

static uint64_t *walk(const struct address_space *s, uint64_t va, bool create) {
    if (!s->root || va < USER_MIN || va >= USER_TOP)
        return NULL;

    uint64_t *t = phys_ptr(s->root);
    for (int level = 3; level; level--) {
        uint64_t *e = &t[(va >> (12 + level * 9)) & 511];
        if (!(*e & P)) {
            if (!create)
                return NULL;
            uint64_t p = table_new();
            if (!p)
                return NULL;
            *e = p | P | W | U;
        }
        if ((*e & (P | U | 128)) != (P | U))
            return NULL;
        t = phys_ptr(*e & MASK);
    }

    return &t[(va >> 12) & 511];
}

bool user_mapped(const struct address_space *s, uint64_t va) {
    uint64_t *entry = walk(s, va, false);
    return entry && (*entry & (P | RESERVED));
}

bool user_mapping_flags(const struct address_space *s, uint64_t va, unsigned *flags) {
    uint64_t *entry = walk(s, va, false);
    if (!entry || !(*entry & (P | RESERVED)))
        return false;
    *flags = ((*entry & P) ? VM_READ : 0) | ((*entry & W) ? VM_WRITE : 0) |
             ((*entry & NX) ? 0 : VM_EXEC);
    return true;
}

bool user_map(struct address_space *s, uint64_t va, unsigned flags) {
    if (va & 4095)
        return false;
    if (s->vm->page_limit && s->vm->pages >= s->vm->page_limit)
        return false;
    uint64_t *e = walk(s, va, true);
    if (!e || (*e & (P | RESERVED)))
        return false;
    uint64_t p = table_new();
    if (!p)
        return false;
    *e = p | ((flags & VM_READ) ? P : RESERVED) | U | ((flags & VM_WRITE) ? W : 0) |
         ((flags & VM_EXEC) ? 0 : NX);
    s->vm->generation++;
    s->vm->pages++;
    s->vm->peak_pages = max(s->vm->peak_pages, s->vm->pages);
    return true;
}

bool user_shared(const struct address_space *s, uint64_t va) {
    uint64_t *e = walk(s, va, false);
    return e && (*e & SHARED);
}

bool user_map_memory(struct address_space *s, uint64_t va, unsigned flags, struct memory_object *m,
                     uint64_t offset, bool readonly) {
    if ((va & 4095) || (flags & VM_EXEC) || (readonly && (flags & VM_WRITE)))
        return false;
    if (s->vm->page_limit && s->vm->pages >= s->vm->page_limit)
        return false;
    uint64_t *e = walk(s, va, true), physical;
    if (!e || (*e & (P | RESERVED)) || !memfd_map_page(m, offset, flags & VM_WRITE, &physical))
        return false;
    *e = physical | SHARED | (readonly ? SHARED_RO : 0) | ((flags & VM_READ) ? P : RESERVED) | U |
         ((flags & VM_WRITE) ? W : 0) | NX;
    s->vm->generation++;
    s->vm->pages++;
    s->vm->peak_pages = max(s->vm->peak_pages, s->vm->pages);
    return true;
}

bool user_protect(struct address_space *s, uint64_t va, unsigned flags) {
    uint64_t *e = walk(s, va, false);
    if (!e || !(*e & (P | RESERVED)))
        return false;
    uint64_t old = *e;
    bool shared = old & SHARED, before = old & W, after = flags & VM_WRITE;
    if (shared && ((flags & VM_EXEC) || ((old & SHARED_RO) && after)))
        return false;
    if (shared && !before && after && !memfd_write_enable(old & MASK))
        return false;
    *e = (old & (MASK | SHARED | SHARED_RO)) | ((flags & VM_READ) ? P : RESERVED) | U |
         ((flags & VM_WRITE) ? W : 0) | ((flags & VM_EXEC) ? 0 : NX);
    s->vm->generation++;
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    if (shared && before && !after)
        memfd_write_disable(old & MASK);
    return true;
}

bool user_unmap(struct address_space *s, uint64_t va) {
    uint64_t *e = walk(s, va, false);
    if (!e || !(*e & (P | RESERVED)))
        return false;
    uint64_t old = *e, p = old & MASK;
    *e = 0;
    s->vm->generation++;
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    if (old & SHARED)
        memfd_unmap_page(p, old & W);
    else
        pmm_free_pages(p, 1);
    s->vm->pages--;
    return true;
}

bool user_access(const struct address_space *s, uint64_t va, size_t len, bool write) {
    if (!len)
        return true;
    if (va < USER_MIN || va >= USER_TOP || len > USER_TOP - va)
        return false;

    uint64_t end = va + len;
    for (uint64_t p = va & ~4095ULL; p < end; p += PAGE_SIZE) {
        uint64_t *e = walk(s, p, false);
        if (!e || ((*e & (P | U)) != (P | U)) || (write && !(*e & W)))
            return false;
    }

    return true;
}

static bool copy(const struct address_space *s, uint64_t u, void *k, size_t n, bool out,
                 bool loader) {
    if (!user_access(s, u, n, out && !loader))
        return false;

    while (n) {
        size_t part = min(n, PAGE_SIZE - (u & 4095));
        /* A sibling may unmap while this syscall is preempted. Recheck each
           page and copy with preemption disabled, never using a stale PFN. */
        uint64_t irq = irq_save();
        uint64_t *e = walk(s, u, false);
        if (!e || ((*e & (P | U)) != (P | U)) || (out && !loader && !(*e & W))) {
            irq_restore(irq);
            return false;
        }
        void *p = (uint8_t *)phys_ptr(*e & MASK) + (u & 4095);
        if (out)
            memcpy(p, k, part);
        else
            memcpy(k, p, part);
        irq_restore(irq);
        u += part;
        k = (uint8_t *)k + part;
        n -= part;
    }

    return true;
}

bool user_copy_in(const struct address_space *s, void *k, uint64_t u, size_t n) {
    return copy(s, u, k, n, false, false);
}

bool user_space_direct(const struct address_space *s) {
    uint64_t root, cr4;
    __asm__ volatile("mov %%cr3,%0; mov %%cr4,%1" : "=r"(root), "=r"(cr4));
    return (root & MASK) == s->root && !(cr4 & (1ULL << 21));
}

bool user_copy_in_validated(const struct address_space *s, void *k, uint64_t u, size_t n) {
    /* Display validates the complete mapping once. No other thread may alter
       this process's mappings during its syscall. Keep the HHDM copy path
       for inactive spaces or machines with SMAP enabled. */
    if (user_space_direct(s)) {
        memcpy(k, (const void *)(uintptr_t)u, n);
        return true;
    }
    return copy(s, u, k, n, false, false);
}

bool user_copy_video_validated(const struct address_space *s, void *k, uint64_t u, size_t n) {
    extern void video_copy(void *, const void *, size_t);
    /* Same complete-range validation requirement as user_copy_in_validated.
       The assembly preserves all SIMD registers it touches. */
    if (user_space_direct(s)) {
        video_copy(k, (const void *)(uintptr_t)u, n);
        return true;
    }
    if (!user_access(s, u, n, false))
        return false;
    while (n) {
        size_t part = min(n, PAGE_SIZE - (u & 4095));
        uint64_t irq = irq_save();
        uint64_t *entry = walk(s, u, false);
        if (!entry || ((*entry & (P | U)) != (P | U))) {
            irq_restore(irq);
            return false;
        }
        video_copy(k, (uint8_t *)phys_ptr(*entry & MASK) + (u & 4095), part);
        irq_restore(irq);
        u += part;
        k = (uint8_t *)k + part;
        n -= part;
    }
    return true;
}

bool user_copy_out(const struct address_space *s, uint64_t u, const void *k, size_t n) {
    return copy(s, u, (void *)k, n, true, false);
}

bool user_load_bytes(const struct address_space *s, uint64_t u, const void *k, size_t n) {
    return copy(s, u, (void *)k, n, true, true);
}

static int string_copy(const struct address_space *s, char *dst, uint64_t src, size_t cap,
                       size_t *size) {
    size_t used = 0;
    while (used < cap) {
        if (src < USER_MIN || src >= USER_TOP)
            return -E_FAULT;
        uint64_t irq = irq_save();
        uint64_t *entry = walk(s, src, false);
        if (!entry || ((*entry & (P | U)) != (P | U))) {
            irq_restore(irq);
            return -E_FAULT;
        }
        size_t part = min(cap - used, PAGE_SIZE - (src & 4095));
        const char *data = (const char *)phys_ptr(*entry & MASK) + (src & 4095);
        const char *end = memchr(data, 0, part);
        size_t n = end ? (size_t)(end - data) + 1 : part;
        if (dst)
            memcpy(dst + used, data, n);
        irq_restore(irq);
        if (end) {
            if (size)
                *size = used + n;
            return 0;
        }
        used += part;
        src += part;
    }
    return -E_NAMETOOLONG;
}

int user_string(const struct address_space *s, char *dst, uint64_t src, size_t cap) {
    return string_copy(s, dst, src, cap, NULL);
}

int user_string_size(const struct address_space *s, uint64_t src, size_t cap, size_t *size) {
    return string_copy(s, NULL, src, cap, size);
}
