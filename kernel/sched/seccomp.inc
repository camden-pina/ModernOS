/* F1: private-domain classic BPF. Included here so the same lock serializes
 * credentials, creation, TSYNC, filter references and deferred budget attach.
 * No user pointers are retained. No filter result grants syscall authority. */
#define F1_OBJECTS 32
#define F1_CHAIN_DEPTH 4
#define F1_PROGRAM_INSNS 4096
#define F1_INSNS 16384
#define F1_ARENA_PAGES 49
#define F1_UNREACHABLE UINT32_MAX
struct f1_filter {
    struct f1_filter *parent;
    uint32_t refs;
    uint16_t start,length;
};
struct f1_arena {
    uintptr_t physical;
    unsigned objects,instructions,peak_objects,peak_instructions;
    bool installing;
    struct f1_filter nodes[F1_OBJECTS];
    struct seccomp_insn code[F1_INSNS];
    uint32_t initialized[F1_INSNS];
};
_Static_assert(sizeof(struct seccomp_insn)==8 && sizeof(struct seccomp_data)==64,"F1 ABI");
_Static_assert(sizeof(struct f1_arena)<=F1_ARENA_PAGES*PAGE_SIZE,"F1 arena");
_Static_assert(F1_ARENA_PAGES*PAGE_SIZE+sizeof(struct c2_domain)-6208<=216*1024,"F1 peak includes embedded metadata");
_Static_assert(F1_ARENA_PAGES*PAGE_SIZE+sizeof(struct c2_domain)-6208+PAGE_SIZE<=216*1024,"F1 peak includes conservative stack scratch");
_Static_assert(C2_CHARGED_PAGES+F1_ARENA_PAGES<=C2_MAX_METADATA_PAGES,"F1 combined metadata ceiling");
#if CONFIG_PID_NAMESPACE_TEST
_Static_assert(C2_CHARGED_PAGES+F1_ARENA_PAGES+P1_CHARGED_PAGES<=C2_MAX_METADATA_PAGES,"P1 combined metadata ceiling");
_Static_assert(P1_ARENA_PAGES*PAGE_SIZE+64*8+8+TASK_MAX*16+PAGE_SIZE+64<=P1_CHARGED_PAGES*PAGE_SIZE,"P1 external/stack metadata reserve");
#endif
static unsigned c2_metadata_pages(const struct c2_domain *d) {
    return C2_CHARGED_PAGES+(d->filters?F1_ARENA_PAGES:0)+p1_metadata_pages(d);
}
static void f1_get_locked(struct f1_filter *f) {
    if(f){kassert(f->refs && f->refs<UINT32_MAX);f->refs++;}
}
static void f1_put_locked(struct c2_domain *d,struct f1_filter *f) {
    while(f) {
        kassert(f->refs);if(--f->refs)return;
        struct f1_filter *parent=f->parent;struct f1_arena *a=d->filters;
        kassert(a && a->objects && a->instructions>=f->length);
        a->objects--;a->instructions-=f->length;
        memset(a->code+f->start,0,f->length*sizeof(struct seccomp_insn));
        memset(f,0,sizeof(*f));f=parent;
    }
}
static void f1_arena_destroy(struct c2_domain *d) {
    if(!d->filters)return;
    struct f1_arena *a=d->filters;kassert(!a->objects && !a->instructions && !a->installing);
    uintptr_t physical=a->physical;
    pr_info("F1_FILTER_RELEASE peak_objects=%u peak_instructions=%u metadata_pages=%u no_live_refs=1\n",
        a->peak_objects,a->peak_instructions,c2_metadata_pages(d));
    memset(a,0,F1_ARENA_PAGES*PAGE_SIZE);pmm_free_pages(physical,F1_ARENA_PAGES);d->filters=NULL;
}
static void f1_empty_locked(struct c2_domain *d) {
    if(!d->filters || d->filters->objects || d->filters->installing)return;
    user_budget_uncharge(d->budget,F1_ARENA_PAGES);f1_arena_destroy(d);
}
static int f1_arena_locked(struct c2_domain *d) {
    if(d->filters)return 0;
    if(d->budget && !user_budget_charge(d->budget,F1_ARENA_PAGES))return -E_NOMEM;
#if CONFIG_USERNS_VERIFY
    uintptr_t physical=selftest_fail_allocation?0:pmm_alloc_pages(F1_ARENA_PAGES);
#else
    uintptr_t physical=pmm_alloc_pages(F1_ARENA_PAGES);
#endif
    if(!physical){user_budget_uncharge(d->budget,F1_ARENA_PAGES);return -E_NOMEM;}
    d->filters=phys_ptr(physical);memset(d->filters,0,F1_ARENA_PAGES*PAGE_SIZE);
    d->filters->physical=physical;return 0;
}
static bool f1_ancestor(struct f1_filter *head,struct f1_filter *ancestor) {
    for(;;head=head->parent){if(head==ancestor)return true;if(!head)return false;}
}
static long f1_install(struct task *t,uintptr_t pointer,bool sync) {
    struct {uint16_t length;uint16_t pad[3];uint64_t pointer;} program;
    if(!user_copy_in(&t->space,&program,pointer,sizeof(program)))return -E_FAULT;
    if(!program.length || program.length>F1_PROGRAM_INSNS)return -E_INVAL;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct userns_credential *c=t->userns;struct c2_domain *d=c->domain;
    long e=0;struct f1_filter *node=NULL;
    if(!c->no_new_privs)e=-E_ACCES;
    else if(d->filters && d->filters->installing)e=-E_AGAIN;
    else {
        unsigned depth=0;for(struct f1_filter *f=c->filter;f;f=f->parent)depth++;
        if(depth>=F1_CHAIN_DEPTH)e=-E_NOMEM;
        if(!e)e=f1_arena_locked(d);
        struct f1_arena *a=d->filters;
        if(!e && (a->objects>=F1_OBJECTS || program.length>F1_INSNS-a->instructions))e=-E_NOMEM;
        unsigned start=0;
        while(!e && start+program.length<=F1_INSNS) {
            bool conflict=false;
            for(unsigned j=0;j<F1_OBJECTS;j++)if(a->nodes[j].refs && start<a->nodes[j].start+a->nodes[j].length && a->nodes[j].start<start+program.length) {
                start=a->nodes[j].start+a->nodes[j].length;conflict=true;break;
            }
            if(!conflict)break;
        }
        if(!e && start+program.length>F1_INSNS)e=-E_NOMEM;
        if(!e) {
            for(unsigned j=0;j<F1_OBJECTS;j++)if(!a->nodes[j].refs){node=&a->nodes[j];break;}
            kassert(node);*node=(struct f1_filter){.refs=1,.parent=c->filter,.start=start,.length=program.length};
            f1_get_locked(node->parent);a->objects++;a->instructions+=program.length;a->installing=true;
            a->peak_objects=max(a->peak_objects,a->objects);a->peak_instructions=max(a->peak_instructions,a->instructions);
        }
    }
    if(e && d->budget_committed) {
        static unsigned observed;
        unsigned index=__atomic_fetch_add(&observed,1,__ATOMIC_RELAXED);
        if(index<16)pr_warn("F1_INSTALL_DENIED pid=%u error=%ld objects=%u object_limit=%u instructions=%u instruction_limit=%u requested=%u chain_limit=%u\n",
            t->tgid,e,d->filters?d->filters->objects:0,F1_OBJECTS,
            d->filters?d->filters->instructions:0,F1_INSNS,program.length,F1_CHAIN_DEPTH);
    }
    spin_unlock_irqrestore(&c2_lock,irq);if(e)return e;
    struct f1_arena *a=d->filters;
    if(!user_copy_in(&t->space,a->code+node->start,program.pointer,program.length*sizeof(struct seccomp_insn)))e=-E_FAULT;
    else if(!seccomp_bpf_verify(a->code+node->start,node->length,a->initialized,true))e=-E_INVAL;
    irq=spin_lock_irqsave(&c2_lock);
    if(!e && sync) {
        for(unsigned i=0;i<C2_TASKS;i++) {
            struct userns_credential *other=&d->creds[i];
            if(other->used && other->group==c->group && !f1_ancestor(c->filter,other->filter)) {e=other->pid;break;}
        }
    }
    if(!e)for(unsigned i=0;i<C2_TASKS;i++) {
        struct userns_credential *other=&d->creds[i];
        if(other==c || (sync && other->used && other->group==c->group)) {
            struct f1_filter *old=other->filter;f1_get_locked(node);other->filter=node;
            if(sync)other->no_new_privs=true;
            f1_put_locked(d,old);
        }
    }
    a->installing=false;f1_put_locked(d,node);
    f1_empty_locked(d);
    spin_unlock_irqrestore(&c2_lock,irq);return e;
}
long seccomp_call(struct task *t,register_t *r) {
    if(!userns_enabled(t))return generic_seccomp_call(t,r);
    if(!CONFIG_SECCOMP_TEST)return -E_NOSYS;
    if(r->rax==SYS_prctl) {
        if(r->rdi==21) {
            uint64_t irq=spin_lock_irqsave(&c2_lock);long v=t->userns->filter?2:0;
            spin_unlock_irqrestore(&c2_lock,irq);return v;
        }
        if(r->rdi!=22 || r->rsi!=2)return -E_INVAL;
        return f1_install(t,r->rdx,false);
    }
    if(r->rdi!=1)return -E_NOTSUP;
    if(r->rsi&~1ULL)return -E_INVAL;
    return f1_install(t,r->rdx,r->rsi==1);
}
uint32_t seccomp_decide(struct task *t,const register_t *r) {
    uint32_t generic=generic_seccomp_decide(t,r);
    if(!CONFIG_SECCOMP_TEST || !userns_enabled(t))return generic;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct c2_domain *d=t->userns->domain;struct f1_filter *head=t->userns->filter;
    if(!head){spin_unlock_irqrestore(&c2_lock,irq);return generic;}
    f1_get_locked(head);d->references++;struct f1_arena *a=d->filters;
    spin_unlock_irqrestore(&c2_lock,irq);
    const struct seccomp_data data={.number=(int32_t)r->rax,.arch=0xc000003e,.ip=r->rip,.args={r->rdi,r->rsi,r->rdx,r->r10,r->r8,r->r9}};
    uint32_t result=generic;unsigned rank=seccomp_bpf_rank(generic);
    for(struct f1_filter *f=head;f;f=f->parent) {
        uint32_t v=seccomp_bpf_run(a->code+f->start,f->length,&data);
        if(!seccomp_bpf_action(v))v=SC_KILL_PROCESS;
        unsigned next=seccomp_bpf_rank(v);if(next<rank){rank=next;result=v;}
    }
    irq=spin_lock_irqsave(&c2_lock);f1_put_locked(d,head);kassert(d->references);d->references--;
    bool destroy=!d->references;spin_unlock_irqrestore(&c2_lock,irq);if(destroy)domain_destroy(d);
    return result;
}
void seccomp_frame_tag(struct task *t,unsigned depth,bool set) {
    if(depth>=4)return;
    if(!userns_enabled(t)){t->seccomp_signal_frames=(t->seccomp_signal_frames&~(1u<<depth))|(set?(1u<<depth):0);return;}
    if(!CONFIG_SECCOMP_TEST)return;
    uint64_t irq=spin_lock_irqsave(&c2_lock);
    t->userns->seccomp_frames=(t->userns->seccomp_frames&~(1u<<depth))|(set?(1u<<depth):0);
    spin_unlock_irqrestore(&c2_lock,irq);
}
bool seccomp_frame_tagged(struct task *t,unsigned depth) {
    if(depth>=4)return false;
    if(!userns_enabled(t))return t->seccomp_signal_frames&(1u<<depth);
    if(!CONFIG_SECCOMP_TEST)return false;
    uint64_t irq=spin_lock_irqsave(&c2_lock);bool v=t->userns->seccomp_frames&(1u<<depth);
    spin_unlock_irqrestore(&c2_lock,irq);return v;
}
#if CONFIG_USERNS_VERIFY
static void f1_accounting_selftest(void) {
    if(!CONFIG_SECCOMP_TEST)return;
    struct task *t=kmallocz(sizeof(*t));struct task_context *context=kmallocz(sizeof(*context));
    kassert(t && context);t->pid=t->tgid=78;t->user=true;t->context=context;kassert(!userns_enroll(t));
    struct c2_domain *d=t->userns->domain;struct userns_budget_claim first={0},second={0};
    struct user_budget *budget=user_budget_create(C2_CHARGED_PAGES+F1_ARENA_PAGES);kassert(budget);
    kassert(!userns_budget_begin(t,budget,&first));
    uint64_t irq=spin_lock_irqsave(&c2_lock);selftest_fail_allocation=true;
    kassert(f1_arena_locked(d)==-E_NOMEM && !d->filters && user_budget_pages(budget)==C2_CHARGED_PAGES);
    selftest_fail_allocation=false;spin_unlock_irqrestore(&c2_lock,irq);
    userns_budget_finish(&first,false);kassert(!d->budget && !user_budget_pages(budget));
    irq=spin_lock_irqsave(&c2_lock);kassert(!f1_arena_locked(d));spin_unlock_irqrestore(&c2_lock,irq);
    kassert(c2_metadata_pages(d)==C2_CHARGED_PAGES+F1_ARENA_PAGES && !d->budget);
    struct user_budget *small=user_budget_create(c2_metadata_pages(d)-1);kassert(small);
    kassert(userns_budget_begin(t,small,&first)==-E_NOMEM && !first.active && !d->budget && !user_budget_pages(small));user_budget_put(small);
    kassert(!userns_budget_begin(t,budget,&first) && !userns_budget_begin(t,budget,&second));
    kassert(user_budget_pages(budget)==c2_metadata_pages(d));
    userns_budget_finish(&first,false);userns_budget_finish(&second,false);
    kassert(!d->budget && !user_budget_pages(budget));
    kassert(!userns_budget_begin(t,budget,&first));userns_budget_finish(&first,true);
    irq=spin_lock_irqsave(&c2_lock);f1_empty_locked(d);spin_unlock_irqrestore(&c2_lock,irq);
    kassert(!d->filters && user_budget_pages(budget)==C2_CHARGED_PAGES);
    userns_task_free(t);kassert(!user_budget_pages(budget));user_budget_put(budget);
    kfree(t);kfree(context);
    /* A currently attached smaller account must deny arena allocation before
       any physical allocation and keep the original account intact. */
    t=kmallocz(sizeof(*t));context=kmallocz(sizeof(*context));kassert(t && context);
    t->pid=t->tgid=79;t->user=true;t->context=context;kassert(!userns_enroll(t));d=t->userns->domain;
    budget=user_budget_create(C2_CHARGED_PAGES+F1_ARENA_PAGES-1);kassert(budget);
    kassert(!userns_budget_begin(t,budget,&first));userns_budget_finish(&first,true);
    irq=spin_lock_irqsave(&c2_lock);kassert(f1_arena_locked(d)==-E_NOMEM && !d->filters);
    spin_unlock_irqrestore(&c2_lock,irq);kassert(user_budget_pages(budget)==C2_CHARGED_PAGES);
    userns_task_free(t);kassert(!user_budget_pages(budget));user_budget_put(budget);kfree(t);kfree(context);
    pr_info("F1_ACCOUNTING_SELFTEST_PASS allocation_failure_refund deferred_attach pending_rollback smaller_limit empty_arena_and_domain_cleanup\n");
}
#endif
