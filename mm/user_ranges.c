#include <mm/user_ranges.h>
#include <mm/pmm.h>
#include <uapi/abi.h>
#include <panic.h>

_Static_assert(sizeof(struct user_range)*USER_RANGE_MAX<=1024*1024,
               "per-tree virtual ledger payload bound");

static bool valid(uint64_t a, uint64_t size) {
    return size && !(a & 4095) && !(size & 4095) && a >= USER_MIN &&
           a < USER_TOP && size <= USER_TOP - a;
}
static void release_record(struct address_space *s, struct user_range *r) {
    kfree(r);
    user_budget_record_give(s->vm->budget, 1);
}
bool user_ranges_sparse(const struct address_space *s) {
    return s->vm->budget && !s->vm->range_restricted;
}
bool user_ranges_new_mapping(struct address_space *s) {
    if (!s->vm->budget) return true;
    if (s->vm->range_tag == UINT64_MAX) return false;
    s->vm->range_tag++;
    return true;
}
bool user_ranges_occupied(const struct address_space *s, uint64_t a) {
    for (const struct user_range *r = s->vm->ranges; r && r->first <= a; r = r->next)
        if (a < r->end) return true;
    return false;
}
bool user_ranges_free(const struct address_space *s, uint64_t a, uint64_t size) {
    if (!valid(a, size)) return false;
    for (const struct user_range *r = s->vm->ranges; r && r->first < a + size; r = r->next)
        if (a < r->end) return false;
    return true;
}
bool user_ranges_select(const struct address_space *s, uint64_t hint, uint64_t size, uint64_t *out) {
    uint64_t end = USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE;
    hint &= ~4095ULL;
    if (hint >= USER_MIN && hint < end && size <= end - hint && user_ranges_free(s,hint,size)) {
        *out = hint; return true;
    }
    uint64_t a = s->vm->mmap_next;
    if (a < USER_MIN || a >= end || size > end - a) return false;
    for (const struct user_range *r = s->vm->ranges; r; r = r->next) {
        if (r->end <= a) continue;
        if (r->first >= a && size <= r->first - a) { *out = a; return true; }
        a = r->end;
        if (a >= end || size > end - a) return false;
    }
    *out = a; return true;
}

static void split_at(struct address_space *s, uint64_t a, struct user_range **spares, unsigned *used) {
    for (struct user_range *r = s->vm->ranges; r; r = r->next) {
        if (r->first < a && a < r->end) {
            struct user_range *tail = spares[(*used)++];
            *tail = *r; tail->first = a; r->end = a; r->next = tail; return;
        }
    }
}
/* All metadata allocations and positive counter deltas precede mutation.
   mode: 0 remove, 1 replace, 2 insert-only. Physical erasure cannot fail.
   Spares count against USER_RANGE_MAX while in flight. */
static bool change(struct address_space *s, uint64_t a, uint64_t size, unsigned mode,
                   bool backed, bool sparse, uint64_t tag, bool erase_backing) {
    if (!s->vm->budget) return true;
    if (!valid(a,size)) return false;
    uint64_t b = a + size, covered = 0;
    unsigned need = 0;
    for (const struct user_range *r = s->vm->ranges; r && r->first < b; r = r->next) {
        if (r->end <= a) continue;
        covered += (min(b,r->end) - max(a,r->first)) / PAGE_SIZE;
        need += r->first < a && a < r->end;
        need += r->first < b && b < r->end;
    }
    if (mode == 2 && covered) return false;
    if (!mode && !covered) return true;
    if (mode && !covered) need++;
    kassert(need <= 3);
    uint64_t next_pages = mode ? size / PAGE_SIZE : 0;
    uint64_t extra = next_pages > covered ? next_pages - covered : 0;
    if (extra && !user_budget_virtual_take(s->vm->budget, extra)) return false;
    if (!user_budget_record_take(s->vm->budget, need)) {
        user_budget_virtual_give(s->vm->budget,extra); return false;
    }
    struct user_range *spares[3] = {0};
    for (unsigned i=0;i<need;i++) {
        spares[i] = kmalloc(sizeof(*spares[i]));
        if (!spares[i]) {
            for (unsigned j=0;j<i;j++) kfree(spares[j]);
            user_budget_record_give(s->vm->budget,need);
            user_budget_virtual_give(s->vm->budget,extra); return false;
        }
    }
    unsigned used=0;
    split_at(s,a,spares,&used); split_at(s,b,spares,&used);
    struct user_range **link=&s->vm->ranges, *keep=NULL;
    while (*link && (*link)->end<=a) link=&(*link)->next;
    while (*link && (*link)->first<b) {
        struct user_range *r=*link;
        kassert(r->first>=a && r->end<=b);
        if (erase_backing && r->backed)
            for (uint64_t va=r->first;va<r->end;va+=PAGE_SIZE) {
                bool ok=user_unmap_backing(s,va); kassert(ok);
            }
        *link=r->next;
        if (mode && !keep) keep=r; else release_record(s,r);
    }
    if (mode) {
        if (!keep) keep=spares[used++];
        *keep=(struct user_range){.first=a,.end=b,.tag=tag,.backed=backed,.sparse=sparse,.next=*link};
        *link=keep;
    }
    while (used<need) release_record(s,spares[used++]);
    for (struct user_range *r=s->vm->ranges;r && r->next;) {
        struct user_range *n=r->next;
        if (r->end==n->first && r->backed==n->backed && r->sparse==n->sparse && r->tag==n->tag) {
            r->end=n->end;r->next=n->next;release_record(s,n);
        } else r=n;
    }
    if (covered>next_pages) user_budget_virtual_give(s->vm->budget,covered-next_pages);
    s->vm->virtual_pages=s->vm->virtual_pages-covered+next_pages;
    s->vm->generation++;
    return true;
}
bool user_ranges_add(struct address_space *s,uint64_t a,uint64_t n,bool backed,bool sparse) {
    if (s->vm->budget && user_ranges_free(s,a,n)) {
        for (struct user_range *r=s->vm->ranges;r;r=r->next) {
            if (r->tag==s->vm->range_tag && r->backed==backed && r->sparse==sparse &&
                (r->end==a || a+n==r->first)) {
                if (!user_budget_virtual_take(s->vm->budget,n/PAGE_SIZE)) return false;
                if (r->end==a) r->end=a+n; else r->first=a;
                s->vm->virtual_pages+=n/PAGE_SIZE;s->vm->generation++;return true;
            }
        }
    }
    return change(s,a,n,2,backed,sparse,s->vm->range_tag,false);
}
void user_ranges_destroy(struct address_space *s) {
    if (!s->vm->budget) return;
    while (s->vm->ranges) {
        struct user_range *r=s->vm->ranges;s->vm->ranges=r->next;release_record(s,r);
    }
    user_budget_virtual_give(s->vm->budget,s->vm->virtual_pages);s->vm->virtual_pages=0;
}
bool user_ranges_clone(struct address_space *out,const struct address_space *src) {
    out->vm->range_tag=src->vm->range_tag;out->vm->range_restricted=src->vm->range_restricted;
    if (!src->vm->budget) return true;
    if (!user_budget_virtual_take(out->vm->budget,src->vm->virtual_pages)) return false;
    out->vm->virtual_pages=src->vm->virtual_pages;
    struct user_range **tail=&out->vm->ranges;
    for (const struct user_range *r=src->vm->ranges;r;r=r->next) {
        if (!user_budget_record_take(out->vm->budget,1)) return false;
        struct user_range *n=kmalloc(sizeof(*n));
        if (!n) {user_budget_record_give(out->vm->budget,1);return false;}
        *n=*r;n->next=NULL;*tail=n;tail=&n->next;
    }
    return true;
}
bool user_ranges_unmap(struct address_space *s,uint64_t a,uint64_t n) {
    if (!valid(a,n) || n>USER_RESERVE_MAX) return false;
    uint64_t physical=0;
    for (const struct user_range *r=s->vm->ranges;r && r->first<a+n;r=r->next)
        if (r->end>a && r->backed) physical+=min(r->end,a+n)-max(r->first,a);
    if (physical>USER_MAX_ALLOC) return false;
    return change(s,a,n,0,false,false,0,true);
}
bool user_ranges_reserve(struct address_space *s,uint64_t a,uint64_t n,bool fixed) {
    if (!user_ranges_sparse(s) || !valid(a,n) || n>USER_RESERVE_MAX) return false;
    if (fixed) {
        uint64_t physical=0;
        for (const struct user_range *r=s->vm->ranges;r && r->first<a+n;r=r->next)
            if (r->end>a && r->backed) physical+=min(r->end,a+n)-max(r->first,a);
        if (physical>USER_MAX_ALLOC) return false;
    }
    return change(s,a,n,fixed?1:2,false,true,s->vm->range_tag,fixed);
}
bool user_ranges_has_sparse(const struct address_space *s,uint64_t a,uint64_t n) {
    if (!valid(a,n)) return false;
    for (const struct user_range *r=s->vm->ranges;r && r->first<a+n;r=r->next)
        if (r->end>a && r->sparse) return true;
    return false;
}
static bool coverage(const struct address_space *s,uint64_t a,uint64_t b,bool *unbacked,uint64_t *backed) {
    uint64_t cursor=a;*unbacked=false;*backed=0;
    for (const struct user_range *r=s->vm->ranges;r && cursor<b;r=r->next) {
        if (r->end<=cursor) continue;
        if (r->first>cursor || !r->sparse) return false;
        uint64_t end=min(b,r->end);
        if (r->backed) *backed+=end-cursor; else *unbacked=true;
        cursor=end;
    }
    return cursor==b;
}
static void undo_new_backing(struct address_space *s,uint64_t a,uint64_t b) {
    for (const struct user_range *r=s->vm->ranges;r && r->first<b;r=r->next)
        if (!r->backed && r->end>a)
            for (uint64_t va=max(a,r->first);va<min(b,r->end);va+=PAGE_SIZE) user_unmap_backing(s,va);
}
long user_ranges_protect(struct address_space *s,uint64_t a,uint64_t n,unsigned flags) {
    if (!valid(a,n) || n>USER_RESERVE_MAX || (flags & ~7u) || (flags && !(flags & VM_READ)) || (flags & 6)==6) return -E_INVAL;
    bool unbacked;uint64_t backed;
    if (!coverage(s,a,a+n,&unbacked,&backed)) return -E_NOMEM;
    if ((flags && n>USER_MAX_ALLOC) || backed>USER_MAX_ALLOC) return -E_INVAL;
    if (!flags) {
        for (const struct user_range *r=s->vm->ranges;r && r->first<a+n;r=r->next)
            if (r->backed && r->end>a)
                for (uint64_t va=max(a,r->first);va<min(a+n,r->end);va+=PAGE_SIZE) {
                    bool ok=user_protect_backing(s,va,0);kassert(ok);
                }
        return 0;
    }
    if (unbacked) {
        for (const struct user_range *r=s->vm->ranges;r && r->first<a+n;r=r->next)
            if (!r->backed && r->end>a)
                for (uint64_t va=max(a,r->first);va<min(a+n,r->end);va+=PAGE_SIZE)
                    if (!user_commit_backing(s,va,flags)) {undo_new_backing(s,a,a+n);return -E_NOMEM;}
        uint64_t tag=0;
        for (const struct user_range *r=s->vm->ranges;r;r=r->next) if (r->end>a) {tag=r->tag;break;}
        if (!change(s,a,n,1,true,true,tag,false)) {undo_new_backing(s,a,a+n);return -E_NOMEM;}
    }
    for (uint64_t va=a;va<a+n;va+=PAGE_SIZE) {bool ok=user_protect_backing(s,va,flags);kassert(ok);}
    return 0;
}
long user_ranges_remap(struct address_space *s,uint64_t a,uint64_t old,uint64_t next) {
    if (!user_ranges_sparse(s) || !valid(a,old) || !valid(a,next) || max(old,next)>USER_RESERVE_MAX) return -E_INVAL;
    bool unbacked;uint64_t backed;
    if (!coverage(s,a,a+old,&unbacked,&backed) || backed) return -E_NOTSUP;
    if (next<old) return user_ranges_unmap(s,a+next,old-next)?(long)a:-E_NOMEM;
    if (next>old) {
        uint64_t tag=0;for (const struct user_range *r=s->vm->ranges;r;r=r->next) if(r->end>a){tag=r->tag;break;}
        if (!change(s,a+old,next-old,2,false,true,tag,false)) return -E_NOMEM;
    }
    return a;
}
