/* Boot verifier only: real bounded metadata/VM/credential operations on
 * unpublished kernel unit records. Never grants an account to a guest helper. */
#if CONFIG_PID_NAMESPACE_VERIFY
static struct task *p1_test_task(unsigned pid) {
    struct task *t=kmallocz(sizeof(*t));struct task_context *c=kmallocz(sizeof(*c));kassert(t && c);
    c->tgid=c->pgid=c->sid=pid;c->references=1;c->uid=c->gid=1000;
    task_context_bind(t,c);t->pid=pid;t->user=true;t->syscall_locked=true;c->syscall_lock.owner=t;
    kassert(user_space_create(&t->space));return t;
}
static int p1_test_inherit(struct task *child,const struct task *parent,bool new_ns) {
    int e=userns_inherit(child,parent,false,new_ns);
    if(!e && !user_space_budget_attach(&child->space,parent->space.vm->budget,parent->space.vm->page_limit))return -E_NOMEM;
    return e;
}
static void p1_test_free(struct task *t) {
    if(!t)return;
    pidns_signal_cleanup(t);userns_task_live(t,false);userns_task_free(t);user_space_destroy(&t->space);kfree(t->context);kfree(t);
}
static void p1_test_filter(struct task *p) {
    register_t q={.rax=SYS_prctl,.rdi=38,.rsi=1};kassert(!userns_call(p,&q));
    struct {uint16_t code;uint8_t jt,jf;uint32_t k;} code[]={
        {0x20,0,0,0},{0x15,0,1,8888},{6,0,0,SC_ERRNO|77},{6,0,0,SC_ALLOW}};
    struct {uint16_t n;uint16_t pad[3];uint64_t p;} program={4,{0},0x10000040};
    kassert(user_copy_out(&p->space,0x10000040,code,sizeof(code)) && user_copy_out(&p->space,0x10000010,&program,sizeof(program)));
    register_t install={.rax=SYS_seccomp,.rdi=1,.rdx=0x10000010};kassert(!seccomp_call(p,&install));
}
static bool p1_test_contains(const char *text,const char *needle) {
    size_t n=strlen(needle);for(;*text;text++)if(!strncmp(text,needle,n))return true;return false;
}
#include "filesystem_selftest.inc"
static void p1_selftest(void) {
    /* Exercise an unaligned second packed record and every retained field. */
    struct p1_signal_record compact[2]={{0},{.sender=P1_IDENTITIES,.code=-128,.uid=1000,.valid=1,.status=255}};
    struct p1_signal_record roundtrip;memcpy(&roundtrip,&compact[1],sizeof(roundtrip));
    kassert(sizeof(roundtrip)==5 && roundtrip.sender==P1_IDENTITIES && roundtrip.code==-128 &&
        roundtrip.uid==1000 && roundtrip.valid && roundtrip.status==255);
    compact[1].valid=0;kassert(compact[1].uid==1000 && compact[1].status==255);
    compact[1].uid=0;compact[1].valid=1;compact[1].code=-6;
    kassert(compact[1].uid==0 && compact[1].valid && compact[1].code==-6 && compact[1].status==255);
    memset(&compact[1],0,sizeof(compact[1]));kassert(!compact[1].valid && !compact[1].uid);

    kassert(!p1_domain && !p1_deferred_destroy);
    /* A smaller inherited backing ceiling wins before physical allocation. */
    struct task *small=p1_test_task(900),*candidate=p1_test_task(901);kassert(!userns_enroll(small));
    struct user_budget *limited=user_budget_create(C2_CHARGED_PAGES+P1_CHARGED_PAGES-1);kassert(limited && user_space_budget_attach(&small->space,limited,0));
    struct userns_budget_claim limited_claim={0};kassert(!userns_budget_begin(small,limited,&limited_claim));userns_budget_finish(&limited_claim,true);
    kassert(!p1_test_inherit(candidate,small,true) && pidns_clone_prepare(candidate,small,0x30000011)==-E_NOMEM && !p1_domain && user_budget_pages(limited)==C2_CHARGED_PAGES);
    p1_test_free(candidate);p1_test_free(small);kassert(!user_budget_pages(limited));user_budget_put(limited);
    for(unsigned nnp=0;nnp<2;nnp++) {
        struct task *v[32]={0};unsigned count=1;v[0]=p1_test_task(1000);struct task *p=v[0];
        kassert(!userns_enroll(p));struct user_budget *budget=user_budget_create(192);kassert(budget);
        kassert(user_space_budget_attach(&p->space,budget,0) && user_map(&p->space,0x10000000,VM_RDWR));
        kassert(pidns_clone_check(p,0x30000011)==-E_PERM);
        struct userns_budget_claim claim={0};kassert(!userns_budget_begin(p,budget,&claim));userns_budget_finish(&claim,true);
        if(nnp)p1_test_filter(p);
        uint64_t before=user_budget_pages(budget);kassert(pidns_clone_check(p,0x30000011)==1);
        kassert(pidns_clone_check(p,0x20000011)==-E_PERM && pidns_clone_check(p,0x70000011)==-E_NOTSUP);
        p->context->uid=0;kassert(pidns_clone_check(p,0x30000011)==-E_PERM);p->context->uid=1000;
        p->pid=999;kassert(pidns_clone_check(p,0x30000011)==-E_PERM);p->pid=1000;
        p->context->fork_owner=p->pid;kassert(pidns_clone_check(p,0x30000011)==-E_AGAIN);p->context->fork_owner=0;
        /* Forced physical allocation failure after a real budget charge. */
        v[count++]=p1_test_task(1001);struct task *c=v[1];kassert(!p1_test_inherit(c,p,true));c->power_creator=p;
        p1_fail_allocation=true;kassert(pidns_clone_prepare(c,p,0x30000011)==-E_NOMEM);p1_fail_allocation=false;
        kassert(!p->userns->domain->pids && user_budget_pages(budget)==before);
        kassert(!pidns_clone_prepare(c,p,0x30000011));
        kassert(user_budget_pages(budget)==before+P1_CHARGED_PAGES && !p->userns->domain->pids->committed);
        /* First-install rollback detaches all temporary identities and refunds. */
        p1_test_free(c);v[1]=NULL;count=1;pidns_clone_abort(p);
        kassert(!p1_domain && !p->userns->pid_identity && user_budget_pages(budget)==before);
        v[count++]=p1_test_task(1001);c=v[1];kassert(!p1_test_inherit(c,p,true));c->power_creator=p;c->ppid=p->tgid;c->context->pgid=p->context->pgid;c->context->sid=p->context->sid;
        kassert(!pidns_clone_prepare(c,p,0x30000011) && !pidns_publish(c));userns_task_live(c,true);
        kassert(pidns_active(c) && pidns_view(c,c->tgid)==1 && pidns_view(c,p->tgid)==0 && pidns_view(p,c->tgid)==c->tgid);
        kassert(pidns_resolve(c,1)==c->tgid && !pidns_resolve(c,p->tgid));
        register_t get={.rax=SYS_prctl,.rdi=39};kassert(userns_call(c,&get)==nnp);
        register_t denied={.rax=8888,.rip=USER_MIN};kassert(seccomp_decide(c,&denied)==(nnp?(SC_ERRNO|77):SC_ALLOW));
        char status[512];uint64_t irq=spin_lock_irqsave(&c2_lock);kassert(p1_proc_status(c,status,sizeof(status))>0);spin_unlock_irqrestore(&c2_lock,irq);
        kassert(p1_test_contains(status,"Pid:\t1\n") && p1_test_contains(status,"PPid:\t0\n") && p1_test_contains(status,"Threads:\t1\n"));
        kassert(pidns_signal_permission(c,c,9)==-E_PERM && !pidns_signal_permission(p,c,9));c->signal_actions[9].handler=0x10000000;
        kassert(!pidns_signal_permission(c,c,10));
        p->context->uid=60000;
        kassert(pidns_signal_record(p,c,12,0,0)==-E_INVAL && !(c->signal_pending&(1ULL<<11)));
        p->context->uid=1000;
        kassert(pidns_signal_record(p,c,10,0,0)==1 && pidns_signal_record(c,c,10,-6,0)==1);
        struct pidns_signal_info info;kassert(pidns_signal_take(c,10,&info) && info.valid && !info.pid && info.uid==1000 && info.code==0);
        /* Descendant creation ownership: unmapped and dropped capabilities fail. */
        kassert(pidns_clone_check(c,0x20000011)==-E_PERM);
        c->userns->ns->uid_mapped=c->userns->ns->gid_mapped=true;kassert(pidns_clone_check(c,0x20000011)==1);
        uint64_t caps=c->userns->effective;c->userns->effective=0;kassert(pidns_clone_check(c,0x20000011)==-E_PERM);c->userns->effective=caps;
        for(unsigned depth=2;depth<=4;depth++) {
            struct task *parent=v[count-1],*child=p1_test_task(1000+count);v[count++]=child;
            parent->userns->ns->uid_mapped=parent->userns->ns->gid_mapped=true;
            kassert(!p1_test_inherit(child,parent,true));child->power_creator=parent;child->ppid=parent->tgid;
            kassert(!pidns_clone_prepare(child,parent,0x30000011) && !pidns_publish(child));userns_task_live(child,true);
            kassert(pidns_view(child,child->tgid)==1 && pidns_view(child,parent->tgid)==0 && pidns_view(parent,child->tgid)==2);
        }
        struct task *deep=v[count-1];deep->userns->ns->uid_mapped=deep->userns->ns->gid_mapped=true;
        struct task *excess=p1_test_task(1100);kassert(!p1_test_inherit(excess,deep,false));
        kassert(pidns_clone_prepare(excess,deep,0x20000011)==-E_NOSPC);p1_test_free(excess);
        /* Four sibling namespaces fill8; user namespaces share the original32 quota. */
        while(p->userns->domain->pids->namespaces<8) {
            struct task *child=p1_test_task(1000+count);v[count++]=child;kassert(!p1_test_inherit(child,p,true));child->power_creator=p;
            kassert(!pidns_clone_prepare(child,p,0x30000011) && !pidns_publish(child));
        }
        excess=p1_test_task(1101);kassert(!p1_test_inherit(excess,p,true));
        kassert(pidns_clone_prepare(excess,p,0x30000011)==-E_NOSPC);p1_test_free(excess);
        while(p->userns->domain->namespaces+p->userns->domain->pids->namespaces<32) {
            struct task *child=p1_test_task(1000+count);v[count++]=child;kassert(count<=32 && !p1_test_inherit(child,p,true));
        }
        excess=p1_test_task(1102);kassert(p1_test_inherit(excess,p,true)==-E_NOSPC);p1_test_free(excess);
        /* Retained identity exhaustion does not allocate or exceed fixed arena. */
        struct p1_identity *held[P1_IDENTITIES];unsigned nh=0;irq=spin_lock_irqsave(&c2_lock);struct p1_arena *a=p->userns->domain->pids;
        while(a->identities<P1_IDENTITIES){held[nh]=p1_new(a,20000+nh,0);kassert(held[nh]);nh++;}
        kassert(!p1_new(a,30000,0));while(nh)p1_put(a,held[--nh]);spin_unlock_irqrestore(&c2_lock,irq);
        /* Every receiver row x64 coalesced slots, without reducing old limits. */
        struct task *receiver=kmallocz(sizeof(*receiver));kassert(receiver);receiver->user=true;
        for(unsigned row=0;row<TASK_MAX;row++) {receiver->pid=5000+row;receiver->signal_pending=0;for(unsigned sig=1;sig<=64;sig++)kassert(pidns_signal_record(p,receiver,sig,0,0)==1);}
        for(unsigned row=0;row<TASK_MAX;row++) {receiver->pid=5000+row;receiver->signal_pending=UINT64_MAX;for(unsigned sig=1;sig<=64;sig++)kassert(pidns_signal_take(receiver,sig,&info) && info.valid && info.pid==p->tgid && info.uid==1000);}
        kfree(receiver);
        /* A separate admitted unit domain cannot claim the one active slot. */
        struct task *other=p1_test_task(4000);kassert(!userns_enroll(other));
        struct user_budget *other_budget=user_budget_create(C2_CHARGED_PAGES);kassert(other_budget && user_space_budget_attach(&other->space,other_budget,0));
        struct userns_budget_claim other_claim={0};kassert(!userns_budget_begin(other,other_budget,&other_claim));userns_budget_finish(&other_claim,true);
        kassert(pidns_clone_check(other,0x30000011)==-E_AGAIN);
        /* Actual retained peer/proc references outlive sender credentials. */
        void *peer=pidns_peer_get(c);kassert(peer && pidns_peer_view(c,peer,c->tgid)==1 && !pidns_peer_view(deep,peer,c->tgid) && pidns_peer_view(p,peer,c->tgid)==c->tgid);
        struct vnode node={0};node.aux[0]=(uintptr_t)p->userns->domain;node.aux[1]=P_PID_OBJECT;node.aux[2]=c->userns->group;
        irq=spin_lock_irqsave(&c2_lock);node.aux[3]=p1_namespace_serial(c->userns);spin_unlock_irqrestore(&c2_lock,irq);proc_retain(&node);
        struct task *member=p1_test_task(1102);kassert(!p1_test_inherit(member,deep,false));
        kassert(pidns_orphan_reaper(member,99999)==deep->tgid);p1_test_free(member);
        kassert(pidns_orphan_reaper(deep,99999)==deep->ppid && pidns_init_exit(c) && pidns_dying(deep));
        kassert(pidns_orphan_reaper(deep,c->tgid)==1);
        for(unsigned i=count;i;i--)p1_test_free(v[i-1]);
        kassert(pidns_peer_view(NULL,peer,1001)==1001);pidns_peer_put(peer);kassert(p1_domain);
        kassert(pidns_clone_check(other,0x30000011)==-E_AGAIN);proc_release(&node);
        kassert(pidns_clone_check(other,0x30000011)==1);p1_test_free(other);kassert(!user_budget_pages(other_budget));user_budget_put(other_budget);
        pidns_reap_deferred();kassert(!p1_domain && !p1_deferred_destroy && !user_budget_pages(budget));user_budget_put(budget);
    }
    pr_info("P1_SELFTEST_PASS unpublished_records_only layout_accounting rollback NNP0_1_F1 nested_views init_permissions shared_quotas retained_IDs full_signal_matrix peer_proc_lifetime\n");
}
#endif
