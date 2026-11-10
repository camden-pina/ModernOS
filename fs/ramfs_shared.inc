/* Included inside ramfs.c: one registry entry per genuinely shared physical
   file page, not a per-mapping copy. VFS serializes initial maps and file I/O;
   this IRQ lock also protects ownership during fork/unmap on other CPUs. */
#define SHARED_BUCKETS 1024
struct ram_shared {
    struct ram_node *node;
    struct mount *mount;
    uint64_t physical, index;
    unsigned references;
    struct ram_shared *physical_next, *index_next, *file_next;
};
static spinlock_t shared_lock;
#if CONFIG_RAMFS_SHARED_WRITE_VERIFY
static unsigned r1_fail_after;
#endif
static struct ram_shared *shared_physical[SHARED_BUCKETS], *shared_indices[SHARED_BUCKETS];
static unsigned shared_key(struct ram_node *n, uint64_t index) {
    return (((uintptr_t)n >> 4) ^ index) % SHARED_BUCKETS;
}
static struct ram_shared *shared_index(struct ram_node *n, uint64_t index) {
    for (struct ram_shared *p=shared_indices[shared_key(n,index)]; p; p=p->index_next)
        if (p->node==n && p->index==index) return p;
    return NULL;
}
static struct ram_shared *shared_page(uint64_t physical) {
    for (struct ram_shared *p=shared_physical[(physical/PAGE_SIZE)%SHARED_BUCKETS]; p; p=p->physical_next)
        if (p->physical==physical) return p;
    return NULL;
}
static void shared_remove(struct ram_shared *p) {
    struct ram_node *n=p->node;
    struct ram_shared **at=&shared_physical[(p->physical/PAGE_SIZE)%SHARED_BUCKETS];
    while (*at!=p) at=&(*at)->physical_next;
    *at=p->physical_next;
    at=&shared_indices[shared_key(n,p->index)];
    while (*at!=p) at=&(*at)->index_next;
    *at=p->index_next;
    at=&n->shared;
    while (*at!=p) at=&(*at)->file_next;
    *at=p->file_next;
    /* Clean boot-backed cache pages are evicted on last alias, so mmap/unmap
       cannot accumulate uncharged private copies in the filesystem. Dirty
       materialized file pages remain owned by RAMFS as before. */
    if (n->boot_data) pmm_free_pages(p->physical,1);
    memfd_backing_uncharge();
    kfree(p);
}
static void shared_sweep(struct ram_node *n) {
    for (struct ram_shared *p=n->shared,*next; p; p=next) {
        next=p->file_next;
        if (!p->references) shared_remove(p);
    }
}
int ramfs_shared_validate(const struct vnode *v,uint64_t offset,uint64_t size) {
    if (v->mount->ops!=&operations || (v->mode&0170000)!=S_IFREG) return -E_NOTSUP;
    struct ram_node *n=(void *)(uintptr_t)v->id;
    /* No stale EOF alias is installed: size-changing mapped files and ranges
       wholly beyond the last data page await cross-CPU invalidation support. */
    if ((offset&(PAGE_SIZE-1)) || !size || offset>=n->size ||
        size>align(n->size-offset,PAGE_SIZE)) return -E_NOTSUP;
    return 0;
}
int ramfs_shared_validate_write(const struct vnode *v,uint64_t off,uint64_t size,uint64_t origin) {
    if(!CONFIG_RAMFS_SHARED_WRITE_TEST || !origin)return -E_NOTSUP;
    int e=ramfs_shared_validate(v,off,size);if(e)return e;
    struct ram_node *n=(void *)(uintptr_t)v->id;
    if(v->mount->readonly || n->uid!=1000 || n->gid!=1000 ||
       n->write_origin!=origin || (n->mode&0600)!=0600 || n->imported || n->boot_data || !n->pages)return -E_ACCES;
    if(size&(PAGE_SIZE-1))return -E_NOTSUP;
    if(!CONFIG_RAMFS_PARTIAL_WRITE_TEST && ((n->size&(PAGE_SIZE-1)) || size>n->size-off))return -E_NOTSUP;
    /* Generic validate already rejects offset>=EOF and any whole page beyond
       rounded EOF. No resizing or extra page is manufactured for padding. */
    return 0;
}
bool ramfs_shared_write_enable(uint64_t physical) {
    if(!CONFIG_RAMFS_SHARED_WRITE_TEST)return false;
    uint64_t irq=spin_lock_irqsave(&shared_lock);struct ram_shared *p=shared_page(physical);
    bool ok=p && p->node->write_origin && !p->node->imported && !p->node->boot_data;
    spin_unlock_irqrestore(&shared_lock,irq);return ok;
}
bool ramfs_shared_map(const struct vnode *v,uint64_t offset,uint64_t *physical) {
    if (ramfs_shared_validate(v,offset,PAGE_SIZE)) return false;
    struct ram_node *n=(void *)(uintptr_t)v->id;
    uint64_t irq=spin_lock_irqsave(&shared_lock), index=offset/PAGE_SIZE;
    struct ram_shared *p=shared_index(n,index);
    if (n->shared_mappings==UINT32_MAX || (p && p->references==UINT32_MAX)) {
        spin_unlock_irqrestore(&shared_lock,irq); return false;
    }
    if (!p) {
        if (!memfd_backing_charge()) {spin_unlock_irqrestore(&shared_lock,irq);return false;}
#if CONFIG_RAMFS_SHARED_WRITE_VERIFY
        p=(r1_fail_after && !--r1_fail_after)?NULL:kmallocz(sizeof(*p));
#else
        p=kmallocz(sizeof(*p));
#endif
        uint64_t page=p ? (n->boot_data ? pmm_alloc_pages(1) : n->pages[index]) : 0;
        if (!page) {
            kfree(p); memfd_backing_uncharge();
            spin_unlock_irqrestore(&shared_lock,irq);return false;
        }
        if (n->boot_data) {
            size_t bytes=min(PAGE_SIZE,n->size-offset);
            memcpy(phys_ptr(page),n->boot_data+offset,bytes);
            memset((uint8_t *)phys_ptr(page)+bytes,0,PAGE_SIZE-bytes);
        }
        /* Caller owns vfs_mutex; shared_lock excludes concurrent map/retain/
           retirement. Only a fresh registry entry has no live alias to this
           page, so never erase padding under a live writable alias. Last
           unmap clears its PTE before retiring that entry. We do not scrub
           during last-unmap or publish zero aliases before any scrub, avoiding
           a race with the existing post-unmap resize/gap-zeroing path. */
        if(CONFIG_RAMFS_PARTIAL_WRITE_TEST && n->write_origin && !n->imported &&
           !n->boot_data && (n->size&(PAGE_SIZE-1)) && index==n->size/PAGE_SIZE) {
            size_t tail=n->size&(PAGE_SIZE-1);
            memset((uint8_t *)phys_ptr(page)+tail,0,PAGE_SIZE-tail);
        }
        p->node=n;p->mount=v->mount;p->index=index;p->physical=page;
        unsigned bucket=(page/PAGE_SIZE)%SHARED_BUCKETS;
        p->physical_next=shared_physical[bucket];shared_physical[bucket]=p;
        bucket=shared_key(n,index);p->index_next=shared_indices[bucket];shared_indices[bucket]=p;
        p->file_next=n->shared;n->shared=p;
    }
    p->references++;
    __atomic_add_fetch(&n->shared_mappings,1,__ATOMIC_RELEASE);
    *physical=p->physical;
    spin_unlock_irqrestore(&shared_lock,irq);
    return true;
}
bool ramfs_shared_retain(uint64_t physical,bool write) {
    uint64_t irq=spin_lock_irqsave(&shared_lock);
    struct ram_shared *p=shared_page(physical);
    bool ok=p && p->references<UINT32_MAX && p->node->shared_mappings<UINT32_MAX &&
        (!write || (CONFIG_RAMFS_SHARED_WRITE_TEST && p->node->write_origin && !p->node->imported));
    if (ok) {p->references++;__atomic_add_fetch(&p->node->shared_mappings,1,__ATOMIC_RELEASE);}
    spin_unlock_irqrestore(&shared_lock,irq);return ok;
}
bool ramfs_shared_unmap(uint64_t physical) {
    uint64_t irq=spin_lock_irqsave(&shared_lock);
    struct ram_shared *p=shared_page(physical);
    if (!p) {spin_unlock_irqrestore(&shared_lock,irq);return false;}
    struct ram_node *n=p->node;struct mount *m=p->mount;
    kassert(p->references && n->shared_mappings);
    p->references--;__atomic_sub_fetch(&n->shared_mappings,1,__ATOMIC_RELEASE);
    if (!p->references && !n->materializing) shared_remove(p);
    ram_destroy(m,n);
    spin_unlock_irqrestore(&shared_lock,irq);return true;
}
bool ramfs_shared_mount_busy(const struct mount *m) {
    uint64_t irq=spin_lock_irqsave(&shared_lock);bool busy=false;
    for (unsigned i=0;i<SHARED_BUCKETS && !busy;i++)
        for (struct ram_shared *p=shared_physical[i];p;p=p->physical_next)
            if (p->mount==m) {busy=true;break;}
    spin_unlock_irqrestore(&shared_lock,irq);return busy;
}
