#include <mm/user.h>
#include <mm/user_ranges.h>
#include <mm/pmm.h>
#include <mm/vmem_io.h>
#include <string.h>
#include <uapi/abi.h>
#include <boot/info.h>
#include <sync.h>
#include <fs/memfd.h>
#include <panic.h>

#define MASK 0x000ffffffffff000ULL
#define P 1ULL
#define W 2ULL
#define U 4ULL
#define NX (1ULL << 63)
#define RESERVED (1ULL << 9)
#define SHARED (1ULL << 10)
#define SHARED_RO (1ULL << 11)
/* Bit 11 is a file-snapshot tag only when SHARED is clear. */
#define PRIVATE_FILE SHARED_RO
/* Software bits outside the physical-address mask, used only in non-present
   EOF leaves. A charged physical zero page is retained, as for PROT_NONE. */
#define FILE_EOF (1ULL << 52)
#define EOF_READ (1ULL << 53)
/* Used only in non-present leaves, never a hardware-present reserved bit. */
#define ANON_ZERO (1ULL << 54)

static void shared_unmap(uint64_t physical, bool write) {
    if (!ramfs_shared_unmap(physical)) memfd_unmap_page(physical, write);
}
static bool shared_retain(uint64_t physical, bool write) {
    return memfd_retain_page(physical, write) || ramfs_shared_retain(physical,write);
}

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
            else if (t[i] & ANON_ZERO)
                continue; /* metadata only; no PFN or resident charge */
            else if (t[i] & SHARED)
                shared_unmap(child, t[i] & W);
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
    user_ranges_destroy(s);
    user_budget_uncharge(s->vm->budget, s->vm->pages);
    user_budget_put(s->vm->budget);
    kfree(s->vm);
    memset(s, 0, sizeof(*s));
}

void user_space_limit(struct address_space *s, uint64_t limit) {
    if (s->vm->budget && limit && limit < user_budget_limit(s->vm->budget))
        s->vm->range_restricted = true;
    if (limit && (!s->vm->page_limit || limit < s->vm->page_limit))
        s->vm->page_limit = limit;
}

bool user_space_budget_attach(struct address_space *s, struct user_budget *budget, uint64_t limit) {
    if (!s->root || s->vm->pages || s->vm->budget) return false;
    user_budget_get(budget);
    s->vm->budget = budget;
    user_space_limit(s, limit);
    user_space_limit(s, user_budget_limit(budget));
    return true;
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
        if (!level && (entry & ANON_ZERO)) {
            to[i] = entry; /* private zero-on-demand, no physical copy */
            continue;
        }
        if (!level && ++*pages > 262144)
            return false; /* bounded 1 GiB fork copy */

        if (!level && (entry & SHARED)) {
            if (!shared_retain(entry & MASK, entry & W))
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

    if (source->vm->budget) {
        if (!user_space_budget_attach(out, source->vm->budget, source->vm->page_limit) ||
            !user_budget_charge(out->vm->budget, source->vm->pages)) {
            user_space_destroy(out);
            return false;
        }
        /* Reserve the complete fork copy before physical allocation. Destruction
           refunds this reservation even if a partial clone later fails. */
        out->vm->pages = source->vm->pages;
    }
    if (!user_ranges_clone(out, source)) { user_space_destroy(out); return false; }
    size_t pages = 0;
    if (!clone_table(out->root, source->root, 3, &pages) ||
        (source->vm->budget && pages != source->vm->pages)) {
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
    return (entry && (*entry & (P | RESERVED))) ||
           (s->vm->budget && user_ranges_occupied(s,va));
}

bool user_mapping_flags(const struct address_space *s, uint64_t va, unsigned *flags) {
    uint64_t *entry = walk(s, va, false);
    if (!entry || !(*entry & (P | RESERVED))) {
        if (s->vm->budget && user_ranges_occupied(s,va)) { *flags=0; return true; }
        return false;
    }
    *flags = ((*entry & ((*entry & (FILE_EOF | ANON_ZERO)) ? EOF_READ : P)) ? VM_READ : 0) |
             ((*entry & W) ? VM_WRITE : 0) |
             ((*entry & NX) ? 0 : VM_EXEC);
    return true;
}

static void prune_empty_tables(struct address_space *,uint64_t);

/* Faults and independent read/write syscalls may allocate an anonymous zero
   page while a sibling's mapping syscall holds the process lock. All shared
   VM threads stay on one CPU, so reserve the VM slot and aggregate charge in
   one IRQ-excluded step. Keep the slot through allocation/publication, and
   refund it on every failure. A plain pages++/peak update compiled to separate
   loads/stores and could overwrite a preempting fault's resident count. */
static bool resident_take(const struct address_space *s) {
    uint64_t irq=irq_save();struct user_vm *v=s->vm;
    if((v->page_limit && v->pages>=v->page_limit) || !user_budget_charge(v->budget,1)) {
        irq_restore(irq);return false;
    }
    v->pages++;v->peak_pages=max(v->peak_pages,v->pages);
    irq_restore(irq);return true;
}
static void resident_give(const struct address_space *s) {
    uint64_t irq=irq_save();kassert(s->vm->pages);s->vm->pages--;
    user_budget_uncharge(s->vm->budget,1);irq_restore(irq);
}

static bool user_map_private(struct address_space *s, uint64_t va, unsigned flags, bool eof, bool file, bool ledger) {
    if (va & 4095)
        return false;
    if (!resident_take(s)) return false;
    uint64_t *e = walk(s, va, true);
    if (!e || (*e & (P | RESERVED))) {
        resident_give(s);
        if (s->vm->budget) prune_empty_tables(s,va);
        return false;
    }
    uint64_t p = table_new();
    if (!p) {
        resident_give(s);
        if (s->vm->budget) prune_empty_tables(s,va);
        return false;
    }
    if (ledger && !user_ranges_add(s,va,PAGE_SIZE,true,false)) {
        pmm_free_pages(p,1);resident_give(s);
        if(s->vm->budget)prune_empty_tables(s,va);return false;
    }
    *e = p | (((flags & VM_READ) && !eof) ? P : RESERVED) | U |
         ((flags & VM_WRITE) ? W : 0) | ((flags & VM_EXEC) ? 0 : NX);
    if (file) *e |= PRIVATE_FILE;
    if (eof) *e |= FILE_EOF | ((flags & VM_READ) ? EOF_READ : 0);
    s->vm->generation++;
    return true;
}

bool user_map(struct address_space *s, uint64_t va, unsigned flags) {
    return user_map_private(s, va, flags, false, false, true);
}
bool user_map_file(struct address_space *s, uint64_t va, unsigned flags) {
    return user_map_private(s,va,flags,false,true,true);
}
bool user_file_snapshot(const struct address_space *s,uint64_t va) {
    uint64_t *e=walk(s,va,false);
    return e && (*e & (P | RESERVED)) && !(*e & SHARED) && (*e & PRIVATE_FILE);
}
bool user_map_file_eof(struct address_space *s, uint64_t va, unsigned flags) {
    return user_map_private(s,va,flags,true,true,true);
}
bool user_commit_backing(struct address_space *s,uint64_t va,unsigned flags) {
    if (!s->vm->budget || !user_ranges_has_sparse(s,va,PAGE_SIZE)) return false;
    return user_map_private(s,va,flags,false,false,false);
}
bool user_file_eof(const struct address_space *s, uint64_t va) {
    uint64_t *e = walk(s, va, false);
    return e && (*e & RESERVED) && (*e & FILE_EOF) && !(*e & P);
}
bool user_file_eof_fault(const struct address_space *s, uint64_t va, uint64_t error) {
    /* Never classify protection/reserved-bit failures as file-data faults. */
    if (error & (1u | 8u)) return false;
    uint64_t *e = walk(s, va, false);
    if (!e || !(*e & RESERVED) || !(*e & FILE_EOF) || (*e & P) || !(*e & EOF_READ))
        return false;
    if ((error & 2u) && !(*e & W)) return false;
    if ((error & 16u) && (*e & NX)) return false;
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
    if (!resident_take(s)) return false;
    uint64_t *e = walk(s, va, true), physical;
    if (!e || (*e & (P | RESERVED)) || !memfd_map_page(m, offset, flags & VM_WRITE, &physical)) {
        resident_give(s);
        if (s->vm->budget) prune_empty_tables(s,va);
        return false;
    }
    if (!user_ranges_add(s,va,PAGE_SIZE,true,false)) {
        memfd_unmap_page(physical,flags & VM_WRITE);resident_give(s);
        if(s->vm->budget)prune_empty_tables(s,va);return false;
    }
    *e = physical | SHARED | (readonly ? SHARED_RO : 0) | ((flags & VM_READ) ? P : RESERVED) | U |
         ((flags & VM_WRITE) ? W : 0) | NX;
    s->vm->generation++;
    return true;
}

bool user_map_shared_file(struct address_space *s, uint64_t va, unsigned flags,
                          const struct vnode *node, uint64_t offset,uint64_t write_origin) {
    if ((va & (PAGE_SIZE-1)) || (flags & VM_EXEC) ||
        ((flags & VM_WRITE) && !write_origin) ||
        (write_origin && ramfs_shared_validate_write(node,offset,PAGE_SIZE,write_origin))) return false;
    if (!resident_take(s)) return false;
    uint64_t *e = walk(s, va, true), physical;
    if (!e || (*e & (P | RESERVED)) || !ramfs_shared_map(node,offset,&physical)) {
        resident_give(s);
        if (s->vm->budget) prune_empty_tables(s,va);
        return false;
    }
    if (!user_ranges_add(s,va,PAGE_SIZE,true,false)) {
        ramfs_shared_unmap(physical);resident_give(s);
        if (s->vm->budget) prune_empty_tables(s,va);
        return false;
    }
    *e = physical | SHARED | (write_origin?0:SHARED_RO) | U | NX | ((flags & VM_WRITE)?W:0) | ((flags & VM_READ) ? P : RESERVED);
    s->vm->generation++;
    return true;
}

bool user_protect_backing(struct address_space *s, uint64_t va, unsigned flags) {
    uint64_t *e = walk(s, va, false);
    if (!e || !(*e & (P | RESERVED)))
        return false;
    uint64_t old = *e;
    if ((flags & ~7u) || (flags && !(flags & VM_READ)) || (flags & 6)==6) return false;
    if (old & ANON_ZERO) {
        *e = ANON_ZERO | RESERVED | U | ((flags & VM_READ) ? EOF_READ : 0) |
             ((flags & VM_WRITE) ? W : 0) | ((flags & VM_EXEC) ? 0 : NX);
        s->vm->generation++;
        __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
        return true;
    }
    bool shared = old & SHARED, before = old & W, after = flags & VM_WRITE;
    if (shared && ((flags & VM_EXEC) || ((old & SHARED_RO) && after)))
        return false;
    if (shared && !before && after && !ramfs_shared_write_enable(old & MASK) && !memfd_write_enable(old & MASK))
        return false;
    *e = (old & (MASK | SHARED | SHARED_RO)) |
         (((flags & VM_READ) && !(old & FILE_EOF)) ? P : RESERVED) | U |
         ((flags & VM_WRITE) ? W : 0) | ((flags & VM_EXEC) ? 0 : NX);
    if (old & FILE_EOF) *e |= FILE_EOF | ((flags & VM_READ) ? EOF_READ : 0);
    s->vm->generation++;
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    if (shared && before && !after && !ramfs_shared_write_enable(old & MASK))
        memfd_write_disable(old & MASK);
    return true;
}

static void prune_empty_tables(struct address_space *s,uint64_t va) {
    if (!s->root || va<USER_MIN || va>=USER_TOP) return;
    uint64_t irq=irq_save();
    uint64_t table[4], *entry[3];table[0]=s->root;
    unsigned depth=0;
    for (unsigned i=0;i<3;i++) {
        uint64_t *t=phys_ptr(table[i]);entry[i]=&t[(va>>(39-i*9))&511];
        if (!(*entry[i]&P)) break;
        table[i+1]=*entry[i]&MASK;depth++;
    }
    for (int i=(int)depth;i>0;i--) {
        uint64_t *t=phys_ptr(table[i]);bool empty=true;
        for (unsigned j=0;j<512;j++) if (t[j]) {empty=false;break;}
        if (!empty) break;
        *entry[i-1]=0;pmm_free_pages(table[i],1);
    }
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    irq_restore(irq);
}
bool user_protect(struct address_space *s,uint64_t va,unsigned flags) {
    if (s->vm->budget && user_ranges_has_sparse(s,va,PAGE_SIZE))
        return user_ranges_protect(s,va,PAGE_SIZE,flags)==0;
    return user_protect_backing(s,va,flags);
}
bool user_unmap(struct address_space *s,uint64_t va) {
    if (s->vm->budget && user_ranges_occupied(s,va))
        return user_ranges_unmap(s,va & ~4095ULL,PAGE_SIZE);
    return user_unmap_backing(s,va);
}
bool user_unmap_backing(struct address_space *s, uint64_t va) {
    uint64_t *e = walk(s, va, false);
    if (!e || !(*e & (P | RESERVED)))
        return false;
    uint64_t old = *e, p = old & MASK;
    *e = 0;
    s->vm->generation++;
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    if (!(old & ANON_ZERO)) {
        if (old & SHARED) shared_unmap(p,old & W);
        else pmm_free_pages(p,1);
        resident_give(s);
    }
    if (s->vm->budget) prune_empty_tables(s,va);
    return true;
}

/* All user threads sharing a VM are pinned to one CPU. As with the existing
   copy paths, IRQ exclusion prevents a sibling from changing a leaf while a
   checked PFN is allocated/published. No new cross-CPU VM execution is enabled. */
static bool zero_access(const struct address_space *s,uint64_t va,unsigned intent) {
    uint64_t *e=walk(s,va,false);
    if (!e || !(*e & ANON_ZERO)) return true;
    uint64_t old=*e;
    if (!(old & EOF_READ) || ((intent & VM_WRITE) && !(old & W)) ||
        ((intent & VM_EXEC) && (old & NX))) return false;
    if (!resident_take(s)) return false;
    uint64_t p=table_new();
    if (!p) {resident_give(s);return false;}
    *e=p | P | U | (old & (W | NX));
    s->vm->generation++;
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    return true;
}
bool user_zero_fault(const struct address_space *s,uint64_t va,uint64_t error) {
    if (error & (1u | 8u)) return false;
    uint64_t irq=irq_save();
    uint64_t *e=walk(s,va,false);
    bool zero=e && (*e & ANON_ZERO);
    bool ok=zero && zero_access(s,va,VM_READ | ((error & 2) ? VM_WRITE : 0) | ((error & 16) ? VM_EXEC : 0));
    irq_restore(irq);
    return ok;
}
#if CONFIG_ELECTRON_PROFILE_SELFTEST
void user_resident_selftest(void) {
    struct address_space s={0};struct user_budget *b=user_budget_create(8);
    kassert(b && user_space_create(&s) && user_space_budget_attach(&s,b,3));
    kassert(user_map(&s,USER_MIN,VM_READ|VM_WRITE));
    kassert(user_map(&s,USER_MIN+PAGE_SIZE,VM_READ|VM_WRITE));
    kassert(!user_discard(&s,USER_MIN,2*PAGE_SIZE,4));
    kassert(!s.vm->pages && !user_budget_pages(b));
    /* Pause a mapping transaction after its reservation. A sibling's actual
       zero-page fault must add to that slot, rather than be overwritten by a
       later mapping counter store. A second reservation reaches the VM cap. */
    kassert(resident_take(&s) && s.vm->pages==1 && user_budget_pages(b)==1);
    kassert(user_zero_fault(&s,USER_MIN,2));
    kassert(s.vm->pages==2 && user_budget_pages(b)==2);
    kassert(resident_take(&s) && s.vm->pages==3 && user_budget_pages(b)==3);
    kassert(!user_zero_fault(&s,USER_MIN+PAGE_SIZE,2));
    kassert(s.vm->pages==3 && user_budget_pages(b)==3);
    resident_give(&s);
    kassert(user_zero_fault(&s,USER_MIN+PAGE_SIZE,2));
    resident_give(&s);
    kassert(s.vm->pages==2 && user_budget_pages(b)==2 && s.vm->peak_pages==3);
    kassert(user_budget_charge(b,6));
    kassert(!resident_take(&s) && s.vm->pages==2 && user_budget_pages(b)==8);
    user_budget_uncharge(b,6);
    kassert(!user_map(&s,USER_MIN,VM_READ|VM_WRITE));
    kassert(s.vm->pages==2 && user_budget_pages(b)==2);
    user_space_destroy(&s);kassert(!user_budget_pages(b));user_budget_put(b);
    pr_notice("USER_RESIDENT_INTERLEAVED_ZERO_FAULT_LIMIT_ROLLBACK_REFUND_PASS\n");
    /* The sparse tree limit now crosses 2^32 pages. Exercise the real atomic
       counters at that boundary without allocating or committing RAM. */
    b=user_budget_create(8);kassert(b);
    uint64_t maximum=USER_VIRTUAL_MAX/PAGE_SIZE;
    kassert(maximum>=1ULL<<32);
    kassert(!user_budget_virtual_take(b,UINT64_MAX));
    kassert(!user_budget_virtual_pages(b) && !user_budget_pages(b));
    kassert(user_budget_virtual_take(b,maximum));
    kassert(!user_budget_virtual_take(b,1) && user_budget_virtual_pages(b)==maximum);
    user_budget_virtual_give(b,maximum/2);
    kassert(user_budget_virtual_take(b,maximum/2));
    user_budget_virtual_give(b,maximum);
    kassert(!user_budget_virtual_pages(b) && !user_budget_pages(b) && !user_budget_records(b));
    user_budget_put(b);
    pr_notice("USER_VIRTUAL_64BIT_LIMIT_REJECT_REFUND_REUSE_PASS\n");
}
#endif
#include "empty_discard.inc"
long user_discard(struct address_space *s,uint64_t a,uint64_t bytes,unsigned advice) {
#if CONFIG_EMPTY_DISCARD_TEST
    if(bytes>USER_MAX_ALLOC && advice==4 && empty_discard_admitted(s) &&
       empty_discard_retry(s,a,bytes)) {
#if CONFIG_ZYGOTE_TRACE
        pr_info("M1_EMPTY_DISCARD bytes=%llu validated_empty=1 data_page_work=0\n",(unsigned long long)bytes);
#endif
        return 0;
    }
#endif
    if (advice!=4 || (a & (PAGE_SIZE-1)) || bytes>user_alloc_limit(s)) return -E_INVAL;
    if (a<USER_MIN || a>=USER_TOP || bytes>USER_TOP-a) return -E_NOMEM;
    uint64_t n=align(bytes,PAGE_SIZE);
    /* Preflight the complete request: unsupported snapshots/shared aliases or
       holes never partly erase an otherwise valid anonymous prefix. */
    for (uint64_t va=a;va<a+n;va+=PAGE_SIZE) {
        uint64_t *e=walk(s,va,false);
        if (!e || !(*e & (P | RESERVED))) {
            bool reserved=false;
            for (const struct user_range *r=s->vm->ranges;r && r->first<=va;r=r->next)
                if (va<r->end && !r->backed && r->sparse) {reserved=true;break;}
            if (!reserved) return -E_NOMEM;
        } else if (*e & (SHARED | PRIVATE_FILE | FILE_EOF)) return -E_NOTSUP;
    }
    for (uint64_t va=a;va<a+n;va+=PAGE_SIZE) {
        uint64_t irq=irq_save();
        uint64_t *e=walk(s,va,false);
        if (e && (*e & (P | RESERVED)) && !(*e & ANON_ZERO)) {
            uint64_t old=*e,p=old & MASK;
            *e=ANON_ZERO | RESERVED | U | ((old & P) ? EOF_READ : 0) | (old & (W | NX));
            s->vm->generation++;
            __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
            pmm_free_pages(p,1);
            resident_give(s);
        }
        irq_restore(irq);
    }
    return 0;
}

bool user_access(const struct address_space *s, uint64_t va, size_t len, bool write) {
    if (!len)
        return true;
    if (va < USER_MIN || va >= USER_TOP || len > USER_TOP - va)
        return false;

    uint64_t end = va + len;
    for (uint64_t p = va & ~4095ULL; p < end; p += PAGE_SIZE) {
        uint64_t irq=irq_save();
        bool zero=zero_access(s,p,VM_READ | (write ? VM_WRITE : 0));
        uint64_t *e = walk(s,p,false);
        bool ok=zero && e && ((*e & (P | U))==(P | U)) && (!write || (*e & W));
        irq_restore(irq);
        if (!ok) return false;
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
        bool zero=zero_access(s,u,VM_READ | ((out && !loader) ? VM_WRITE : 0));
        uint64_t *e = walk(s, u, false);
        if (!zero || !e || ((*e & (P | U)) != (P | U)) || (out && !loader && !(*e & W))) {
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
        bool zero=zero_access(s,u,VM_READ);
        uint64_t *entry = walk(s, u, false);
        if (!zero || !entry || ((*entry & (P | U)) != (P | U))) {
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

/* FUTEX_WAKE_OP must use an atomic machine RMW, including against a userspace
 * atomic store. The HHDM alias also works when SMAP or another CR3 is active. */
bool user_futex_atomic32(const struct address_space *s,uint64_t address,unsigned op,uint32_t operand,uint32_t *old) {
    if((address&3) || op>4 || !user_access(s,address,4,true))return false;
    uint64_t irq=irq_save();bool ok=zero_access(s,address,VM_WRITE);
    uint64_t *entry=ok?walk(s,address,false):NULL;
    if(!entry || ((*entry&(P|W|U))!=(P|W|U)))ok=false;
    if(ok) {
        uint32_t *word=(uint32_t *)((uint8_t *)phys_ptr(*entry&MASK)+(address&4095));
        switch(op) {
        case 0:*old=__atomic_exchange_n(word,operand,__ATOMIC_SEQ_CST);break;
        case 1:*old=__atomic_fetch_add(word,operand,__ATOMIC_SEQ_CST);break;
        case 2:*old=__atomic_fetch_or(word,operand,__ATOMIC_SEQ_CST);break;
        case 3:*old=__atomic_fetch_and(word,~operand,__ATOMIC_SEQ_CST);break;
        case 4:*old=__atomic_fetch_xor(word,operand,__ATOMIC_SEQ_CST);break;
        }
    }
    irq_restore(irq);return ok;
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
        bool zero=zero_access(s,src,VM_READ);
        uint64_t *entry = walk(s, src, false);
        if (!zero || !entry || ((*entry & (P | U)) != (P | U))) {
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
