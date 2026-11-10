/* Approved, default-off no-data-work subset. No ledger or PTE is changed.
   The legacy <=512MiB path below this include remains byte-identical. */
#if CONFIG_EMPTY_DISCARD_TEST
#include <sched.h>
#include <credentials.h>
#include <userns.h>
#include <printk.h>
#define EMPTY_DISCARD_MAX (8ULL << 30)
#if CONFIG_ELECTRON_PROFILE_SELFTEST
static const struct address_space *empty_test_space;
static unsigned empty_test_hook,empty_test_chunks,empty_test_leaves;
static void empty_test_mutate(const struct address_space *);
#endif

static noinline bool empty_discard_span(const struct address_space *s,uint64_t a,uint64_t n) {
    if(!s || !s->vm || !user_ranges_sparse(s) || n<=USER_MAX_ALLOC ||
       n>EMPTY_DISCARD_MAX || ((a|n)&(PAGE_SIZE-1)) ||
       a<USER_MIN || a>=USER_TOP || n>USER_TOP-a)return false;
    uint64_t end=a+n,irq=irq_save(),generation=s->vm->generation;
    bool owned=false;unsigned records=0;
    /* Never retain a ledger pointer across preemption. Shared-VM threads stay
       on the same CPU by the existing scheduler contract. */
    if(generation!=UINT64_MAX) {
        uint64_t cursor=a;
        for(const struct user_range *r=s->vm->ranges;r && records<USER_RANGE_MAX;r=r->next) {
            records++;
            if(r->end<=cursor)continue;
            if(r->first>cursor || !r->sparse || r->backed)break;
            cursor=min(end,r->end);
            if(cursor==end){owned=true;break;}
        }
    }
    irq_restore(irq);
    if(!owned)return false;
    /* At most4097 chunks and2097152 leaf reads/16MiB metadata. Each IRQ-off
       section covers at most one2MiB chunk. Missing tables skip the chunk;
       unknown/huge/supervisor/nonpresent nonzero entries fail closed. */
    for(uint64_t va=a;va<end;) {
        uint64_t stop=min(end,(va+(1ULL<<21))&~((1ULL<<21)-1));
        irq=irq_save();
        bool ok=s->vm->generation==generation;
        uint64_t *table=phys_ptr(s->root);
        for(int level=3;ok && level;level--) {
            uint64_t e=table[(va>>(12+9*level))&511];
            if(!e) {table=NULL;break;}
            if((e&(P|U))!=(P|U) || !(e&MASK) ||
               (e&~(MASK|P|W|U|(1ULL<<5)|NX))) {ok=false;break;}
            table=phys_ptr(e&MASK);
        }
        if(ok && table) {
            for(unsigned j=(va>>12)&511;va<stop;j++,va+=PAGE_SIZE)
            {
#if CONFIG_ELECTRON_PROFILE_SELFTEST
                if(s==empty_test_space)empty_test_leaves++;
#endif
                if(table[j]) {ok=false;break;}
            }
        }
        irq_restore(irq);
        if(!ok)return false;
#if CONFIG_ELECTRON_PROFILE_SELFTEST
        if(s==empty_test_space) {
            empty_test_chunks++;
            if(empty_test_hook)empty_test_mutate(s);
        }
#endif
        va=stop;
    }
    /* A mutation between chunks invalidates the whole observation, even if
       it restored an apparently equivalent ledger. No partial effects exist. */
    irq=irq_save();bool stable=s->vm->generation==generation;irq_restore(irq);
    return stable;
}
/* A sibling may change unrelated VM metadata between validation chunks.
   Retry a fresh, complete observation at most three times. A stable failure
   remains a denial; every successful attempt proves the same empty subset. */
static bool empty_discard_retry(const struct address_space *s,uint64_t a,uint64_t n) {
    if(!s || !s->vm)return false;
    for(unsigned attempt=0;attempt<3;attempt++) {
        uint64_t irq=irq_save(),before=s->vm->generation;irq_restore(irq);
        if(empty_discard_span(s,a,n))return true;
        irq=irq_save();bool changed=s->vm->generation!=before;irq_restore(irq);
        if(!changed)break;
    }
    return false;
}
static bool empty_discard_admitted(const struct address_space *s) {
    struct task *t=sched_ready()?this_cpu()->current:NULL;
    return t && t->user && !t->vfork_parent && t->space.vm==s->vm &&
           t->space.root==s->root && credentials_uid(t)==1000 && credentials_gid(t)==1000 &&
           userns_admitted_account(t) && user_ranges_sparse(s);
}
#if CONFIG_ELECTRON_PROFILE_SELFTEST
#include <panic.h>
#include <tsc.h>
/* Only unpublished selftest VMs. No user-facing hook or production state. */
static void empty_test_mutate(const struct address_space *s) {
    kassert(s==empty_test_space && s->vm!=this_cpu()->current->space.vm);
    unsigned hook=empty_test_hook;empty_test_hook=0;
    if(hook==3 || hook==4) {
        /* Synthetic unrelated generation changes at the preemption boundary. */
        ((struct address_space *)s)->vm->generation++;
        if(hook==4)empty_test_hook=4;
        return;
    }
    struct address_space *w=(struct address_space *)s;
    uint64_t a=s->vm->ranges->first;
    if(hook==1) {kassert(user_commit_backing(w,a,VM_READ|VM_WRITE));}
    else {kassert(hook==2 && user_ranges_unmap(w,a,PAGE_SIZE));
        kassert(user_ranges_new_mapping(w) && user_ranges_reserve(w,a,PAGE_SIZE,false));}
}
static void empty_test_setup(struct address_space *s,struct user_budget *b,uint64_t a,uint64_t n) {
    kassert(user_space_create(s) && user_space_budget_attach(s,b,0));
    kassert(user_ranges_new_mapping(s) && user_ranges_reserve(s,a,n,false));
}
void empty_discard_selftest(void) {
    const uint64_t a=0x2c0400001000ULL,n=EMPTY_DISCARD_MAX;
    struct user_budget *b=user_budget_create(327680);kassert(b);
    struct address_space s={0},copy={0};empty_test_setup(&s,b,a,n);
    uint64_t gen=s.vm->generation,records=user_budget_records(b),virtual=user_budget_virtual_pages(b);
    kassert(empty_discard_span(&s,a,n) && empty_discard_span(&s,a,USER_MAX_ALLOC+PAGE_SIZE));
    kassert(!empty_discard_span(&s,a,USER_MAX_ALLOC) && !empty_discard_span(&s,a,n+PAGE_SIZE));
    kassert(!empty_discard_span(&s,a+1,n) && !empty_discard_span(&s,a,n-1));
    kassert(!empty_discard_span(&s,USER_TOP-PAGE_SIZE,n) && !empty_discard_span(&s,UINT64_MAX-4095,n));
    kassert(!empty_discard_span(&s,a+PAGE_SIZE,n) && !empty_discard_span(&s,a-PAGE_SIZE,n));
    kassert(!empty_discard_admitted(&s) && user_discard(&s,a,n,4)==-E_INVAL && user_discard(&s,a,n,3)==-E_INVAL);
    kassert(gen==s.vm->generation && !user_budget_pages(b) && records==user_budget_records(b) && virtual==user_budget_virtual_pages(b));
    s.vm->generation=UINT64_MAX;kassert(!empty_discard_span(&s,a,n));s.vm->generation=gen;
    s.vm->ranges->backed=true;kassert(!empty_discard_span(&s,a,n));s.vm->ranges->backed=false;
    s.vm->range_restricted=true;kassert(!empty_discard_span(&s,a,n));s.vm->range_restricted=false;
    /* Real backing, guard, zero marker, snapshot/shared/EOF tags all fail. */
    kassert(user_commit_backing(&s,a,VM_READ|VM_WRITE));
    const char data[]="M1_REJECTION_PRESERVES_DATA";char readback[sizeof(data)];
    kassert(user_copy_out(&s,a,data,sizeof(data)) && !empty_discard_span(&s,a,n));
    kassert(user_copy_in(&s,readback,a,sizeof(readback)) && !memcmp(data,readback,sizeof(data)));
    uint64_t *leaf=walk(&s,a,false),saved=*leaf;
    const uint64_t tags[]={SHARED,PRIVATE_FILE,FILE_EOF,ANON_ZERO,1ULL<<55};
    for(unsigned i=0;i<sizeof(tags)/sizeof(*tags);i++) {*leaf=saved|tags[i];kassert(!empty_discard_span(&s,a,n));}
    *leaf=saved;kassert(user_protect_backing(&s,a,0));kassert(!empty_discard_span(&s,a,n) && !user_access(&s,a,1,false));
    kassert(!user_discard(&s,a,PAGE_SIZE,4) && !empty_discard_span(&s,a,n));
    kassert(user_unmap_backing(&s,a) && !user_budget_pages(b) && empty_discard_span(&s,a,n));
    /* Allocated-but-empty tables exercise every leaf, including a misaligned
       8GiB span crossing4097 two-MiB chunks. No data page is allocated here. */
    for(uint64_t va=a;va<a+n;va=min(a+n,(va+(1ULL<<21))&~((1ULL<<21)-1)))kassert(walk(&s,va,true));
    empty_test_space=&s;empty_test_chunks=empty_test_leaves=0;
    uint64_t started=tsc_uptime_us();kassert(empty_discard_span(&s,a,n));uint64_t elapsed=tsc_uptime_us()-started;
    kassert(empty_test_chunks==4097 && empty_test_leaves==2097152);
    pr_notice("M1_EMPTY_WORST_METADATA_PASS chunks=%u leaf_reads=%u metadata_bytes=%u elapsed_us=%llu\n",empty_test_chunks,empty_test_leaves,empty_test_leaves*8,(unsigned long long)elapsed);
    empty_test_space=NULL;
    /* Malformed and supervisor upper entries cannot masquerade as holes. */
    uint64_t *upper=phys_ptr(s.root),idx=(a>>39)&511,original=upper[idx];
    const uint64_t bad[]={NX,original|128,original&~U,original|(1ULL<<54)};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){upper[idx]=bad[i];kassert(!empty_discard_span(&s,a,n));}upper[idx]=original;
    kassert(user_space_clone(&copy,&s) && empty_discard_span(&copy,a,n));user_space_destroy(&copy);
    user_space_share(&copy,&s);kassert(copy.vm==s.vm && empty_discard_span(&copy,a,n));user_space_destroy(&copy);
    /* Deterministic preemption-boundary injections mutate an already-scanned
       chunk; generation validation must reject both write and remove/remap. */
    empty_test_space=&s;empty_test_hook=1;kassert(!empty_discard_span(&s,a,n) && !empty_test_hook);
    kassert(user_unmap_backing(&s,a));empty_test_hook=2;kassert(!empty_discard_span(&s,a,n) && !empty_test_hook);empty_test_space=NULL;
    empty_test_space=&s;empty_test_hook=3;
    kassert(empty_discard_retry(&s,a,n) && !empty_test_hook);
    empty_test_chunks=0;empty_test_hook=4;
    kassert(!empty_discard_retry(&s,a,n) && empty_test_chunks==3);empty_test_hook=0;
    empty_test_hook=1;kassert(!empty_discard_retry(&s,a,n));
    kassert(user_unmap_backing(&s,a));empty_test_hook=2;kassert(empty_discard_retry(&s,a,n));
    empty_test_space=NULL;
    /* MADV_DONTNEED may span adjacent anonymous reservations with different
       tags. Every byte must still be owned, sparse and actually empty. */
    kassert(empty_discard_span(&s,a,n));
    kassert(user_ranges_unmap(&s,a,PAGE_SIZE) && !empty_discard_span(&s,a,n));
    kassert(user_ranges_new_mapping(&s) && user_ranges_reserve(&s,a,PAGE_SIZE,false));
    kassert(empty_discard_span(&s,a,n));
    s.vm->ranges->sparse=false;kassert(!empty_discard_span(&s,a,n));s.vm->ranges->sparse=true;
    s.vm->ranges->backed=true;kassert(!empty_discard_span(&s,a,n));s.vm->ranges->backed=false;
    user_space_destroy(&s);kassert(!user_budget_pages(b) && !user_budget_records(b) && !user_budget_virtual_pages(b));user_budget_put(b);
    /* The complete ledger bound also holds for contiguous distinct mappings.
       A hole at the final record fails without partial metadata/page work. */
    b=user_budget_create(327680);kassert(b);kassert(user_space_create(&s) && user_space_budget_attach(&s,b,0));
    for(unsigned i=0;i<USER_RANGE_MAX;i++) {
        kassert(user_ranges_new_mapping(&s) && user_ranges_reserve(&s,a+(uint64_t)i*(n/USER_RANGE_MAX),n/USER_RANGE_MAX,false));
    }
    kassert(user_budget_records(b)==USER_RANGE_MAX && empty_discard_span(&s,a,n));
    gen=s.vm->generation;records=user_budget_records(b);virtual=user_budget_virtual_pages(b);
    kassert(!empty_discard_span(&s,a-PAGE_SIZE,n) && !empty_discard_span(&s,a+PAGE_SIZE,n));
    kassert(gen==s.vm->generation && records==user_budget_records(b) && virtual==user_budget_virtual_pages(b));
    kassert(user_ranges_unmap(&s,a+n-n/USER_RANGE_MAX,n/USER_RANGE_MAX) && !empty_discard_span(&s,a,n));
    user_space_destroy(&s);kassert(!user_budget_pages(b) && !user_budget_records(b) && !user_budget_virtual_pages(b));user_budget_put(b);
    /* Record boundary: last record may own the span, never beyond the cap. */
    b=user_budget_create(327680);kassert(b);empty_test_setup(&s,b,a,n);
    for(unsigned i=0;i<USER_RANGE_MAX-1;i++)kassert(user_ranges_new_mapping(&s) && user_ranges_reserve(&s,USER_MIN+(uint64_t)i*8192,PAGE_SIZE,false));
    kassert(user_budget_records(b)==USER_RANGE_MAX && empty_discard_span(&s,a,n));
    gen=s.vm->generation;kassert(user_ranges_new_mapping(&s) && !user_ranges_reserve(&s,a+n+PAGE_SIZE,PAGE_SIZE,false));
    kassert(gen==s.vm->generation && user_budget_records(b)==USER_RANGE_MAX && empty_discard_span(&s,a,n));
    user_space_destroy(&s);kassert(!user_budget_pages(b) && !user_budget_records(b) && !user_budget_virtual_pages(b));user_budget_put(b);
    /* Subsequent commits retain zero fill, permission and exhaustion checks. */
    b=user_budget_create(1);kassert(b);empty_test_setup(&s,b,a,n);kassert(empty_discard_span(&s,a,n));
    kassert(user_commit_backing(&s,a,VM_READ|VM_WRITE) && user_budget_pages(b)==1);
    kassert(user_copy_in(&s,readback,a,sizeof(readback)));for(unsigned i=0;i<sizeof(readback);i++)kassert(!readback[i]);
    kassert(!user_commit_backing(&s,a+PAGE_SIZE,VM_READ|VM_WRITE) && user_budget_pages(b)==1);
    kassert(user_unmap_backing(&s,a) && user_commit_backing(&s,a+PAGE_SIZE,VM_READ));
    kassert(!user_access(&s,a+PAGE_SIZE,1,true));user_space_destroy(&s);
    kassert(!user_budget_pages(b) && !user_budget_records(b) && !user_budget_virtual_pages(b));user_budget_put(b);
    pr_notice("M1_EMPTY_DISCARD_SELFTEST_PASS bounds/noeffects/leaf-tags/guards/generation/bounded-retry/records/contiguous-mappings/holes/clone/share/zero/exhaustion/cleanup\n");
}
#endif
#endif
