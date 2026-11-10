/* P1 shares c2_lock: canonical outer IDs and real ownership remain authority.
 * Scheduler callers take scheduler_lock before c2_lock; never the reverse. */
#if CONFIG_PID_NAMESPACE_TEST
static struct c2_domain *p1_domain,*p1_deferred_destroy;
#if CONFIG_PID_NAMESPACE_VERIFY
static bool p1_fail_allocation;
static unsigned p1_fault_progress;
#endif
_Static_assert(C2_ARENA_PAGES<=6,"P1 credential fields must fit charged C2 arena");
bool pidns_verify_fault(struct task *t,unsigned point) {
#if CONFIG_PID_NAMESPACE_VERIFY
    if(sched_ready() && t==this_cpu()->current && t->user && point && point<=7) {
        unsigned expected=point-1;
        if(__atomic_compare_exchange_n(&p1_fault_progress,&expected,point,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {
            pr_info("P1_FAULT_INJECT point=%u pid=%u rollback_required=1 verifier_only=1\n",point,t->pid);return true;
        }
    }
#else
    (void)t;(void)point;
#endif
    return false;
}
static bool p1_scope_locked(const struct task *t) {
    return t && t->user && t->userns && t->pid==t->tgid && !t->browser_sandbox &&
        !t->context->web_role && credentials_uid(t)==1000 && credentials_gid(t)==1000 &&
        t->space.vm && t->space.vm->budget && t->userns->domain->budget_committed &&
        t->userns->domain->budget==t->space.vm->budget;
}
static unsigned p1_ns_count(const struct c2_domain *d) {return d->pids?d->pids->namespaces:0;}
static bool p1_mapped(const struct c2_namespace *n) {
    for(;n;n=n->parent)if(!n->uid_mapped || !n->gid_mapped)return false;
    return true;
}
static bool p1_user_ancestor(const struct c2_namespace *ancestor,const struct c2_namespace *n) {
    for(;n;n=n->parent)if(n==ancestor)return true;
    return !ancestor;
}
static struct p1_identity *p1_find(struct p1_arena *a,unsigned outer) {
    for(unsigned i=0;i<P1_IDENTITIES;i++)if(a->ids[i].references && a->ids[i].outer==outer)return &a->ids[i];
    return NULL;
}
static unsigned p1_index(struct p1_arena *a,const struct p1_identity *id) {
    kassert(id>=a->ids && id<a->ids+P1_IDENTITIES && id->references);return id-a->ids+1;
}
static void p1_ns_put(struct p1_arena *a,unsigned index) {
    while(index) {
        struct p1_namespace *n=&a->ns[index-1];kassert(n->used && n->references);
        if(--n->references)return;
        unsigned parent=n->parent;struct c2_namespace *owner=n->owner;
        memset(n,0,sizeof(*n));kassert(a->namespaces);a->namespaces--;ns_put_locked(a->domain,owner);index=parent;
    }
}
static void p1_get(struct p1_arena *a,struct p1_identity *id) {
    kassert(id && id->references && id->references<UINT32_MAX && a->domain->references<UINT32_MAX);
    id->references++;a->domain->references++;
}
static void p1_put(struct p1_arena *a,struct p1_identity *id) {
    kassert(id && id->references && a->domain->references);a->domain->references--;
    if(--id->references)return;
    unsigned ns=id->ns;memset(id,0,sizeof(*id));kassert(a->identities);a->identities--;p1_ns_put(a,ns);
}
static struct p1_identity *p1_new(struct p1_arena *a,unsigned outer,unsigned ns) {
    if(!outer || a->identities>=P1_IDENTITIES)return NULL;
    struct p1_identity *id=NULL;
    for(unsigned i=0;i<P1_IDENTITIES;i++)if(!a->ids[i].references){id=&a->ids[i];break;}
    if(!id)return NULL;
    for(unsigned n=ns;n;n=a->ns[n-1].parent)
        if(!a->ns[n-1].used || a->ns[n-1].state!=1 || !a->ns[n-1].next || a->ns[n-1].next>INT32_MAX)return NULL;
    memset(id,0,sizeof(*id));id->outer=outer;id->references=1;id->ns=ns;id->depth=ns?a->ns[ns-1].depth:0;
    for(unsigned n=ns;n;n=a->ns[n-1].parent)id->number[a->ns[n-1].depth-1]=a->ns[n-1].next++;
    if(ns)a->ns[ns-1].references++;
    a->identities++;a->peak_identities=max(a->peak_identities,a->identities);a->domain->references++;
    return id;
}
static unsigned p1_task_ns(const struct task *t) {
    return t && t->userns && t->userns->pid_identity?t->userns->pid_identity->ns:0;
}
static unsigned p1_view_locked(const struct task *t,const struct p1_identity *id) {
    unsigned ns=p1_task_ns(t);if(!ns)return id->outer;
    struct p1_arena *a=t->userns->domain->pids;
    unsigned target=id->ns,depth=a->ns[ns-1].depth;
    for(;target && a->ns[target-1].depth>depth;target=a->ns[target-1].parent);
    return target==ns?id->number[depth-1]:0;
}
static unsigned p1_view_outer_locked(const struct task *t,unsigned outer) {
    if(!p1_task_ns(t))return outer;
    struct p1_arena *a=t->userns->domain->pids;
    if(!a || !a->committed)return outer;
    struct p1_identity *id=p1_find(a,outer);return id?p1_view_locked(t,id):0;
}
static unsigned p1_resolve_locked(const struct task *t,unsigned local) {
    if(!p1_task_ns(t))return local;
    struct p1_arena *a=t->userns->domain->pids;
    if(!a || !a->committed)return local;
    if(!local)return 0;
    for(unsigned i=0;i<P1_IDENTITIES;i++)if(a->ids[i].references && p1_view_locked(t,&a->ids[i])==local)return a->ids[i].outer;
    return 0;
}
static int p1_group_set_locked(struct task *t,unsigned pgid,unsigned sid) {
    struct p1_arena *a=t->userns->domain->pids;struct p1_group *g=NULL;
    for(unsigned i=0;i<C2_PROCESSES;i++)if(a->groups[i].group==t->userns->group){g=&a->groups[i];break;}
    if(!g)for(unsigned i=0;i<C2_PROCESSES;i++)if(!a->groups[i].group){g=&a->groups[i];break;}
    if(!g)return -E_NOSPC;
    struct p1_identity *p=p1_find(a,pgid),*s=p1_find(a,sid);
    if(p)p1_get(a,p);
    if(s)p1_get(a,s);
    if(g->pgid_id)p1_put(a,&a->ids[g->pgid_id-1]);
    if(g->sid_id)p1_put(a,&a->ids[g->sid_id-1]);
    *g=(struct p1_group){
#if CONFIG_FILESYSTEM_SANDBOX_TEST
        .filesystem_index=g->filesystem_index,.filesystem_flags=g->filesystem_flags,
#endif
        .group=t->userns->group,.pgid=pgid,.sid=sid,
        .pgid_id=p?p1_index(a,p):0,.sid_id=s?p1_index(a,s):0};return 0;
}
static void p1_group_free_locked(struct c2_domain *d,uint64_t group) {
    struct p1_arena *a=d->pids;if(!a)return;
    for(unsigned i=0;i<C2_PROCESSES;i++)if(a->groups[i].group==group) {
        struct p1_group *g=&a->groups[i];
#if CONFIG_FILESYSTEM_SANDBOX_TEST
        s1_group_free_locked(d,g);
#endif
        if(g->pgid_id)p1_put(a,&a->ids[g->pgid_id-1]);
        if(g->sid_id)p1_put(a,&a->ids[g->sid_id-1]);
        memset(g,0,sizeof(*g));return;
    }
}
static int p1_inherit_locked(struct task *child,const struct task *parent) {
    struct c2_domain *d=child->userns->domain;struct p1_arena *a=d->pids;
    if(!a)return 0;
    child->userns->pid_identity=NULL; /* never copy the creator's identity */
    if(!a->committed)return -E_AGAIN;
    unsigned ns=p1_task_ns(parent);
    if(ns && a->ns[ns-1].state!=1)return -E_NOMEM;
    struct p1_identity *id=p1_new(a,child->pid,p1_task_ns(parent));
    if(!id)return -E_NOSPC;
    child->userns->pid_identity=id;return 0;
}
static void p1_credential_free_locked(struct userns_credential *r) {
    if(r->pid_identity){p1_put(r->domain->pids,r->pid_identity);r->pid_identity=NULL;}
}
static void p1_arena_destroy(struct c2_domain *d) {
    if(!d->pids)return;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct p1_arena *a=d->pids;
    kassert(!a->identities && !a->namespaces && p1_domain==d);
    for(unsigned i=0;i<TASK_MAX;i++)kassert(!a->signal_target[i]);
    for(unsigned i=0;i<C2_PROCESSES;i++)kassert(!a->groups[i].group);
#if CONFIG_FILESYSTEM_SANDBOX_TEST
    for(unsigned i=0;i<S1_STATES;i++)kassert(!a->filesystem[i].used);
#endif
    d->pids=NULL;p1_domain=NULL;
    if(p1_deferred_destroy==d)p1_deferred_destroy=NULL;
    spin_unlock_irqrestore(&c2_lock,irq);
    pr_info("P1_METADATA_RELEASE arena_pages=%u charged_pages=%u peak_identities=%u peak_namespaces=%u no_live_refs=1\n",P1_ARENA_PAGES,P1_CHARGED_PAGES,a->peak_identities,a->peak_namespaces);
    uintptr_t physical=a->physical;memset(a,0,P1_ARENA_PAGES*PAGE_SIZE);pmm_free_pages(physical,P1_ARENA_PAGES);
}
void pidns_reap_deferred(void) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct c2_domain *d=p1_deferred_destroy;
    if(d){kassert(!d->references);p1_deferred_destroy=NULL;}
    spin_unlock_irqrestore(&c2_lock,irq);if(d)domain_destroy(d);
}
static void p1_bootstrap_rollback_locked(struct c2_domain *d) {
    struct p1_arena *a=d->pids;kassert(a && !a->committed);
    for(unsigned i=0;i<C2_TASKS;i++)if(d->creds[i].used)p1_credential_free_locked(&d->creds[i]);
    kassert(!a->identities && !a->namespaces);uintptr_t physical=a->physical;
    d->pids=NULL;p1_domain=NULL;user_budget_uncharge(d->budget,P1_CHARGED_PAGES);
    memset(a,0,P1_ARENA_PAGES*PAGE_SIZE);pmm_free_pages(physical,P1_ARENA_PAGES);
}
static int p1_begin_locked(struct task *parent) {
    struct c2_domain *d=parent->userns->domain;
    if(d->pids)return d->pids->committed?0:-E_AGAIN;
    if(p1_domain)return -E_AGAIN;
    if(!user_budget_charge(d->budget,P1_CHARGED_PAGES))return -E_NOMEM;
#if CONFIG_PID_NAMESPACE_VERIFY
    uintptr_t physical=p1_fail_allocation?0:pmm_alloc_pages(P1_ARENA_PAGES);
#else
    uintptr_t physical=pmm_alloc_pages(P1_ARENA_PAGES);
#endif
    if(!physical){user_budget_uncharge(d->budget,P1_CHARGED_PAGES);return -E_NOMEM;}
    struct p1_arena *a=phys_ptr(physical);memset(a,0,P1_ARENA_PAGES*PAGE_SIZE);
    a->physical=physical;a->domain=d;a->installing=parent->pid;d->pids=a;p1_domain=d;
    for(unsigned i=0;i<C2_TASKS;i++)if(d->creds[i].used) {
        kassert(!d->creds[i].pid_identity);
        struct p1_identity *id=p1_new(a,d->creds[i].pid,0);
        if(!id){p1_bootstrap_rollback_locked(d);return -E_NOSPC;}
        d->creds[i].pid_identity=id;
    }
    return 0;
}
bool pidns_active(const struct task *t) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);bool result=p1_task_ns(t)!=0;spin_unlock_irqrestore(&c2_lock,irq);return result;
}
bool pidns_proc_available(const struct task *t) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);bool result=t && t->user && t->userns && !t->browser_sandbox && !t->context->web_role && credentials_uid(t)==1000 && credentials_gid(t)==1000 && t->space.vm && t->space.vm->budget && t->userns->domain->budget_committed && t->userns->domain->budget==t->space.vm->budget;spin_unlock_irqrestore(&c2_lock,irq);return result;
}
unsigned pidns_view(const struct task *t,unsigned outer) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);unsigned result=p1_view_outer_locked(t,outer);spin_unlock_irqrestore(&c2_lock,irq);return result;
}
unsigned pidns_resolve(const struct task *t,unsigned local) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);unsigned result=p1_resolve_locked(t,local);spin_unlock_irqrestore(&c2_lock,irq);return result;
}
int pidns_clone_check(const struct task *t,uint64_t flags) {
    if(!(flags&0x20000000))return 0;
    if(flags!=0x20000011 && flags!=0x30000011)return -E_NOTSUP;
    uint64_t irq=spin_lock_irqsave(&c2_lock);int result=-E_PERM;
    if(!p1_scope_locked(t))goto done;
    if(t->vfork_parent || t->context->exiting || t->context->fork_owner || !t->syscall_locked || t->context->syscall_lock.owner!=t){result=-E_AGAIN;goto done;}
    if(p1_domain && p1_domain!=t->userns->domain){result=-E_AGAIN;goto done;}
    if(!p1_mapped(t->userns->ns))goto done;
    if(flags==0x20000011) {
        struct c2_namespace *n=t->userns->ns;
        if(!n || !(t->userns->effective&(1ULL<<21)))goto done;
        unsigned ns=p1_task_ns(t);
        if(ns && !p1_user_ancestor(t->userns->domain->pids->ns[ns-1].owner,n))goto done;
    }
    result=1;
done:spin_unlock_irqrestore(&c2_lock,irq);return result;
}
int pidns_clone_prepare(struct task *child,const struct task *parent,uint64_t flags) {
    if(!(flags&0x20000000))return 0;
    uint64_t irq=spin_lock_irqsave(&c2_lock);int result=p1_begin_locked((struct task *)parent);
    if(result)goto done;
    struct c2_domain *d=parent->userns->domain;struct p1_arena *a=d->pids;
    struct p1_identity *id=child->userns->pid_identity;unsigned ancestor=p1_task_ns(parent);
    if(!id || id->references!=1 || id->ns!=ancestor){result=-E_PERM;goto done;}
    if(a->namespaces>=P1_NAMESPACES || d->namespaces+a->namespaces>=C2_NAMESPACES ||
       (ancestor && a->ns[ancestor-1].depth>=P1_DEPTH) || next_object>UINT64_MAX/256){result=-E_NOSPC;goto done;}
    if(ancestor && a->ns[ancestor-1].state!=1){result=-E_NOMEM;goto done;}
    struct c2_namespace *owner=child->userns->ns;if(!owner){result=-E_PERM;goto done;}
    unsigned slot=0;for(unsigned i=0;i<P1_NAMESPACES;i++)if(!a->ns[i].used){slot=i+1;break;}
    kassert(slot);struct p1_namespace *n=&a->ns[slot-1];
    *n=(struct p1_namespace){.id=next_object++,.owner=owner,.references=1,.next=2,.init=child->tgid,
        .parent=ancestor,.depth=ancestor?a->ns[ancestor-1].depth+1:1,.state=1,.used=1};
    owner->references++;if(ancestor)a->ns[ancestor-1].references++;
    a->namespaces++;a->peak_namespaces=max(a->peak_namespaces,a->namespaces);
    id->number[n->depth-1]=1;id->ns=slot;id->depth=n->depth;p1_ns_put(a,ancestor);
    result=0;
done:spin_unlock_irqrestore(&c2_lock,irq);return result;
}
void pidns_clone_abort(struct task *parent) {
    if(!parent || !parent->userns)return;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct c2_domain *d=parent->userns->domain;
    if(d->pids && !d->pids->committed && d->pids->installing==parent->pid)p1_bootstrap_rollback_locked(d);
    spin_unlock_irqrestore(&c2_lock,irq);
}
int pidns_publish(struct task *t) {
    if(!t->userns || !t->userns->domain->pids)return 0;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct p1_arena *a=t->userns->domain->pids;int result=0;
    if(!a->committed && (!t->power_creator || t->power_creator->pid!=a->installing || !p1_task_ns(t))){result=-E_AGAIN;goto done;}
    for(unsigned ns=p1_task_ns(t);ns;ns=a->ns[ns-1].parent)if(a->ns[ns-1].state!=1){result=-E_NOMEM;goto done;}
    unsigned ns=p1_task_ns(t);bool new_init=ns && t->pid==t->tgid && a->ns[ns-1].init==t->tgid;
    if(new_init && pidns_verify_fault(t->power_creator,6)){result=-E_NOMEM;goto done;}
    result=p1_group_set_locked(t,t->context->pgid,t->context->sid);
    if(!result && new_init && pidns_verify_fault(t->power_creator,7)){result=-E_NOMEM;goto done;}
    if(!result){a->committed=true;a->installing=0;
#if CONFIG_ZYGOTE_TRACE
        if(new_init){unsigned seq=credentials_zygote_record(t);if(seq)pr_info("P1_NAMESPACE_COMMIT seq=%u outer_pid=%u inner_pid=%u depth=%u namespace=%llu uid=%u\n",seq,t->tgid,p1_view_locked(t,t->userns->pid_identity),a->ns[ns-1].depth,(unsigned long long)a->ns[ns-1].id,credentials_uid(t));}
#endif
    }
done:spin_unlock_irqrestore(&c2_lock,irq);return result;
}
int pidns_group_set(struct task *t,unsigned pgid,unsigned sid) {
    if(!t->userns || !t->userns->domain->pids)return 0;
    uint64_t irq=spin_lock_irqsave(&c2_lock);int e=p1_group_set_locked(t,pgid,sid);spin_unlock_irqrestore(&c2_lock,irq);return e;
}
/* Called only with scheduler_lock held. Namespace IDs are still canonical. */
bool pidns_init_exit(struct task *t) {
    if(!t->userns || !t->userns->domain->pids)return false;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct p1_arena *a=t->userns->domain->pids;unsigned ns=p1_task_ns(t);bool changed=false;
    if(ns && a->ns[ns-1].init==t->tgid) {
        for(unsigned i=0;i<P1_NAMESPACES;i++)if(a->ns[i].used) {
            unsigned n=i+1;for(;n && n!=ns;n=a->ns[n-1].parent);
            if(n){a->ns[i].state=2;changed=true;}
        }
    }
    spin_unlock_irqrestore(&c2_lock,irq);return changed;
}
bool pidns_dying(const struct task *t) {
    if(!t->userns || !t->userns->domain->pids)return false;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct p1_arena *a=t->userns->domain->pids;unsigned ns=p1_task_ns(t);bool dying=ns && a->ns[ns-1].state==2;spin_unlock_irqrestore(&c2_lock,irq);return dying;
}
unsigned pidns_orphan_reaper(const struct task *t,unsigned dead) {
    if(!t->userns || !t->userns->domain->pids)return 1;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct p1_arena *a=t->userns->domain->pids;unsigned result=1;
    /* A namespace init whose outer parent exits cannot adopt itself. Keep
       ordinary members with their init, then search ancestor namespaces. */
    for(unsigned ns=p1_task_ns(t);ns;ns=a->ns[ns-1].parent)
        if(a->ns[ns-1].state==1 && a->ns[ns-1].init!=dead && a->ns[ns-1].init!=t->tgid) {
            result=a->ns[ns-1].init;break;
        }
    spin_unlock_irqrestore(&c2_lock,irq);return result;
}
int pidns_signal_permission(const struct task *sender,const struct task *target,unsigned sig) {
    if(!sig || !target->userns || !target->userns->domain->pids)return 0;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct p1_arena *a=target->userns->domain->pids;unsigned ns=p1_task_ns(target);int result=0;
    if(ns && a->ns[ns-1].init==target->tgid && target->signal_actions[sig-1].handler<=1) {
        bool ancestor=!p1_task_ns(sender);
        if(sender && sender->userns && sender->userns->domain==target->userns->domain) {
            unsigned s=p1_task_ns(sender);ancestor=false;
            for(unsigned n=a->ns[ns-1].parent;n;n=a->ns[n-1].parent)if(n==s)ancestor=true;
            if(!s)ancestor=true;
        }
        if(!ancestor || (sig!=9 && sig!=19))result=-E_PERM;
    }
    spin_unlock_irqrestore(&c2_lock,irq);return result;
}
void *pidns_peer_get(const struct task *t) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct p1_identity *id=NULL;
    if(p1_task_ns(t)) {
        struct p1_arena *a=t->userns->domain->pids;
        if(a && a->committed){id=p1_find(a,t->tgid);kassert(id);p1_get(a,id);}
    }
    spin_unlock_irqrestore(&c2_lock,irq);return id;
}
void pidns_peer_put(void *reference) {
    if(!reference)return;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct c2_domain *d=p1_domain;kassert(d && d->pids);
    struct p1_identity *id=reference;p1_index(d->pids,id);p1_put(d->pids,id);bool destroy=!d->references;
    spin_unlock_irqrestore(&c2_lock,irq);if(destroy)domain_destroy(d);
}
unsigned pidns_peer_view(const struct task *t,void *reference,unsigned outer) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);unsigned result=outer;
    if(p1_task_ns(t))result=reference?p1_view_locked(t,reference):0;
    spin_unlock_irqrestore(&c2_lock,irq);return result;
}
#else
bool pidns_verify_fault(struct task *t,unsigned p){(void)t;(void)p;return false;}
void pidns_reap_deferred(void){}
static unsigned p1_ns_count(const struct c2_domain *d){(void)d;return 0;}
static void p1_arena_destroy(struct c2_domain *d){(void)d;}
static int p1_inherit_locked(struct task *c,const struct task *p){(void)c;(void)p;return 0;}
static void p1_credential_free_locked(struct userns_credential *r){(void)r;}
static void p1_group_free_locked(struct c2_domain *d,uint64_t g){(void)d;(void)g;}
static unsigned p1_view_outer_locked(const struct task *t,unsigned n){(void)t;return n;}
bool pidns_active(const struct task *t){(void)t;return false;}
bool pidns_proc_available(const struct task *t){(void)t;return false;}
unsigned pidns_view(const struct task *t,unsigned n){(void)t;return n;}
unsigned pidns_resolve(const struct task *t,unsigned n){(void)t;return n;}
int pidns_clone_check(const struct task *t,uint64_t f){(void)t;return f&0x20000000?-E_NOTSUP:0;}
int pidns_clone_prepare(struct task *c,const struct task *p,uint64_t f){(void)c;(void)p;(void)f;return 0;}
void pidns_clone_abort(struct task *t){(void)t;}
int pidns_publish(struct task *t){(void)t;return 0;}
int pidns_group_set(struct task *t,unsigned p,unsigned s){(void)t;(void)p;(void)s;return 0;}
bool pidns_init_exit(struct task *t){(void)t;return false;}
bool pidns_dying(const struct task *t){(void)t;return false;}
unsigned pidns_orphan_reaper(const struct task *t,unsigned d){(void)t;(void)d;return 1;}
int pidns_signal_permission(const struct task *s,const struct task *t,unsigned n){(void)s;(void)t;(void)n;return 0;}
void *pidns_peer_get(const struct task *t){(void)t;return NULL;}
void pidns_peer_put(void *p){(void)p;}
unsigned pidns_peer_view(const struct task *t,void *p,unsigned n){(void)t;(void)p;return n;}
#endif
#include "pidns_signals.inc"
