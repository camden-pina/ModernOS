/* Approved S1 only. State and bindings are inside P1's existing charged arena.
 * No task/context/file growth, no user-controlled root restoration interface. */
#if CONFIG_FILESYSTEM_SANDBOX_TEST
#define S1_RESTRICTED 1u
#define S1_CWD_INSIDE 2u
#define S1_PENDING_FIRST 1u
#if CONFIG_FILESYSTEM_SANDBOX_VERIFY
static unsigned s1_fault_progress;
#endif
bool s1_fault(struct task *t,unsigned point) {
#if CONFIG_FILESYSTEM_SANDBOX_VERIFY
    if(sched_ready() && t==this_cpu()->current && t->user && point && point<=9) {
        unsigned before=point-1;
        if(__atomic_compare_exchange_n(&s1_fault_progress,&before,point,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {
            pr_info("S1_FAULT_INJECT point=%u pid=%u rollback_required=1 verifier_only=1\n",point,t->pid);return true;
        }
    }
#else
    (void)t;(void)point;
#endif
    return false;
}
int s1_pointers(const struct task *t,uint64_t stack,uint64_t tls) {
    return !stack || (stack&15) || stack<USER_MIN+16 || stack>=USER_TOP ||
        !user_access(&t->space,stack-16,16,true) || tls<USER_MIN || tls>USER_TOP-16 ||
        tls==t->fsbase || !user_access(&t->space,tls,16,true)?-E_FAULT:0;
}
static struct p1_group *s1_group_locked(const struct task *t,bool create) {
    if(!t || !t->userns || !t->userns->domain->pids)return NULL;
    struct p1_arena *a=t->userns->domain->pids;
    for(unsigned i=0;i<C2_PROCESSES;i++)if(a->groups[i].group==t->userns->group)return &a->groups[i];
    if(create)for(unsigned i=0;i<C2_PROCESSES;i++)if(!a->groups[i].group) {
        a->groups[i].group=t->userns->group;return &a->groups[i];
    }
    return NULL;
}
static struct s1_filesystem *s1_state_locked(const struct task *t) {
    struct p1_group *g=s1_group_locked(t,false);
    if(!g || !g->filesystem_index)return NULL;
    kassert(g->filesystem_index<=S1_STATES);
    struct s1_filesystem *s=&t->userns->domain->pids->filesystem[g->filesystem_index-1];
    kassert(s->used && s->references);return s;
}
static void s1_drop_locked(struct c2_domain *d,struct s1_filesystem *s) {
    kassert(s->used && s->references);
    if(--s->references)return;
    if(s->root.cache[0])p1_put(d->pids,(void *)(uintptr_t)s->root.cache[0]);
    memset(s,0,sizeof(*s));
}
static void s1_group_free_locked(struct c2_domain *d,struct p1_group *g) {
    if(g->filesystem_index) {
        struct s1_filesystem *s=&d->pids->filesystem[g->filesystem_index-1];
        g->filesystem_index=g->filesystem_flags=0;s1_drop_locked(d,s);
    }
}
static struct s1_filesystem *s1_allocate_locked(struct p1_arena *a) {
    if(next_object>UINT64_MAX/65536)return NULL;
    for(unsigned i=0;i<S1_STATES;i++)if(!a->filesystem[i].used) {
        struct s1_filesystem *s=&a->filesystem[i];memset(s,0,sizeof(*s));
        s->used=1;s->serial=next_object++;s->references=1;return s;
    }
    return NULL;
}
static int s1_inherit_locked(struct task *child,const struct task *parent,bool thread,bool share) {
    struct s1_filesystem *source=s1_state_locked(parent);
    if(thread) {if(source)child->cwd=source->cwd;return 0;}
    if(!source && !share)return 0;
    struct p1_arena *a=parent->userns->domain->pids;
    if(!a || !a->committed)return -E_PERM;
    struct p1_group *cg=s1_group_locked(child,true),*pg=s1_group_locked(parent,false);
    if(!cg || !pg)return -E_NOSPC;
    if(share) {
        for(unsigned i=0;i<S1_STATES;i++)if(a->filesystem[i].borrower_outer_pid)return -E_AGAIN;
        if(!source) {
            source=s1_allocate_locked(a);if(!source)return -E_NOSPC;
            strcpy(source->cwd,parent->cwd);source->creation_mask=parent->context->creation_mask;
            source->root_kind=S1_CWD_INSIDE;
            pg->filesystem_index=source-a->filesystem+1;pg->filesystem_flags=S1_PENDING_FIRST;
            ((struct task *)parent)->cwd=source->cwd;
        }
        source->references++;source->borrower_outer_pid=child->pid;
        cg->filesystem_index=source-a->filesystem+1;child->cwd=source->cwd;
    } else {
        struct s1_filesystem *copy=s1_allocate_locked(a);if(!copy)return -E_NOSPC;
        uint64_t serial=copy->serial;*copy=*source;copy->serial=serial;copy->references=1;copy->borrower_outer_pid=0;
        if(copy->root.cache[0])p1_get(a,(void *)(uintptr_t)copy->root.cache[0]);
        cg->filesystem_index=copy-a->filesystem+1;child->cwd=copy->cwd;
    }
    return 0;
}
int s1_clone_check(struct task *t,uint64_t flags) {
    if(flags!=0x84311)return 0;
    uint64_t irq=spin_lock_irqsave(&c2_lock);int result=-E_PERM;
    if(p1_scope_locked(t) && p1_task_ns(t) && p1_view_outer_locked(t,t->tgid)==1 &&
       p1_mapped(t->userns->ns) && t->userns->ns && (t->userns->effective&(1ULL<<18))) {
        result=1;
        for(unsigned i=0;i<S1_STATES;i++)if(t->userns->domain->pids->filesystem[i].borrower_outer_pid)result=-E_AGAIN;
    }
    spin_unlock_irqrestore(&c2_lock,irq);
    if(result==1 && (!t->syscall_locked || t->context->syscall_lock.owner!=t ||
       t->held_mutexes!=1 || t->no_park_depth!=1 || t->context->fork_owner || !userns_single_live(t)))result=-E_AGAIN;
    return result;
}
int s1_publish(struct task *child,struct task *parent) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct s1_filesystem *s=s1_state_locked(parent);
    struct p1_group *g=s1_group_locked(parent,false);int e=-E_AGAIN;
    if(s && g && s==s1_state_locked(child) && s->borrower_outer_pid==child->pid) {
        g->filesystem_flags=0;e=0;
    }
    spin_unlock_irqrestore(&c2_lock,irq);return e;
}
void s1_clone_abort(struct task *parent) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct s1_filesystem *s=s1_state_locked(parent);
    struct p1_group *g=s1_group_locked(parent,false);
    if(s && g) {
        s->borrower_outer_pid=0;
        if(g->filesystem_flags&S1_PENDING_FIRST) {
            kassert(s->references==1 && !(s->root_kind&S1_RESTRICTED));
            parent->cwd=parent->context->cwd;s1_group_free_locked(parent->userns->domain,g);
        }
        g->filesystem_flags=0;
    }
    spin_unlock_irqrestore(&c2_lock,irq);
}
void s1_helper_exit(struct task *t) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct s1_filesystem *s=s1_state_locked(t);
    if(s && s->borrower_outer_pid==t->pid)s->borrower_outer_pid=0;
    spin_unlock_irqrestore(&c2_lock,irq);
}
bool s1_borrowed(const struct task *t) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct s1_filesystem *s=s1_state_locked(t);
    bool yes=s && s->borrower_outer_pid==t->pid && t->vfork_parent;
    spin_unlock_irqrestore(&c2_lock,irq);return yes;
}
bool s1_restricted(const struct task *t) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct s1_filesystem *s=s1_state_locked(t);
    bool yes=s && (s->root_kind&S1_RESTRICTED);spin_unlock_irqrestore(&c2_lock,irq);return yes;
}
int s1_cwd_check(const struct task *t) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct s1_filesystem *s=s1_state_locked(t);
    int e=s && (s->root_kind&S1_RESTRICTED) && !(s->root_kind&S1_CWD_INSIDE)?-E_ACCES:0;
    spin_unlock_irqrestore(&c2_lock,irq);return e;
}
unsigned s1_mask(const struct task *t,bool set,unsigned mask) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct s1_filesystem *s=s1_state_locked(t);
    unsigned old=s?s->creation_mask:t->context->creation_mask;
    if(set){if(s)s->creation_mask=mask&0777;else t->context->creation_mask=mask&0777;}
    spin_unlock_irqrestore(&c2_lock,irq);return old;
}
int s1_root(struct vnode *out) {
    struct task *t=current();uint64_t irq=spin_lock_irqsave(&c2_lock);struct s1_filesystem *s=s1_state_locked(t);
    int handled=s && (s->root_kind&S1_RESTRICTED);if(handled)*out=s->root;
    spin_unlock_irqrestore(&c2_lock,irq);return handled;
}
bool s1_root_matches(const struct vnode *n) {
    struct vnode root;return s1_root(&root) && root.mount==n->mount && root.id==n->id &&
        root.aux[0]==n->aux[0] && root.aux[2]==n->aux[2] && root.aux[3]==n->aux[3];
}
void s1_chdir(struct task *t,const char *path,const struct vnode *n) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct s1_filesystem *s=s1_state_locked(t);
    strcpy(t->cwd,path);if(s && (s->root_kind&S1_RESTRICTED))s->root_kind|=S1_CWD_INSIDE;
    spin_unlock_irqrestore(&c2_lock,irq);(void)n;
}
int s1_fchdir(struct task *t,const struct vnode *n,const char *path) {
    if(s1_restricted(t)) {if(!s1_root_matches(n))return -E_ACCES;s1_chdir(t,"/",n);}
    else s1_chdir(t,path,n);
    return 0;
}
#else
int s1_pointers(const struct task *t,uint64_t s,uint64_t f){(void)t;(void)s;(void)f;return -E_NOTSUP;}
bool s1_fault(struct task *t,unsigned p){(void)t;(void)p;return false;}
int s1_clone_check(struct task *t,uint64_t f){(void)t;(void)f;return 0;}
int s1_publish(struct task *c,struct task *p){(void)c;(void)p;return -E_NOTSUP;}
void s1_clone_abort(struct task *t){(void)t;}
void s1_helper_exit(struct task *t){(void)t;}
bool s1_borrowed(const struct task *t){(void)t;return false;}
bool s1_restricted(const struct task *t){(void)t;return false;}
int s1_cwd_check(const struct task *t){(void)t;return 0;}
unsigned s1_mask(const struct task *t,bool set,unsigned mask){unsigned old=t->context->creation_mask;if(set)t->context->creation_mask=mask&0777;return old;}
int s1_root(struct vnode *n){(void)n;return 0;}
bool s1_root_matches(const struct vnode *n){(void)n;return false;}
void s1_chdir(struct task *t,const char *p,const struct vnode *n){(void)n;strcpy(t->cwd,p);}
int s1_fchdir(struct task *t,const struct vnode *n,const char *p){s1_chdir(t,p,n);return 0;}
static int s1_inherit_locked(struct task *c,const struct task *p,bool th,bool sh){(void)c;(void)p;(void)th;return sh?-E_NOTSUP:0;}
#endif
