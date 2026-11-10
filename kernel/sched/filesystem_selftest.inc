#if CONFIG_FILESYSTEM_SANDBOX_VERIFY
/* Actual fixed-state/credential functions on unpublished kernel unit records.
 * No guest binary receives an account; no guest instruction executes here. */
static void s1_unit_inherit(struct task *child,struct task *parent,bool share) {
    kassert(!userns_inherit_fs(child,parent,false,false,share));
    kassert(user_space_budget_attach(&child->space,parent->space.vm->budget,0));
    child->power_creator=parent;child->vfork_parent=share?parent->pid:0;
}
static void s1_selftest(struct task *parent) {
    struct c2_domain *d=parent->userns->domain;struct p1_arena *a=d->pids;
    uint64_t before=user_budget_pages(d->budget);char *oldcwd=parent->cwd;
    kassert(!s1_pointers(parent,0x10000100,0x10000200));
    kassert(s1_pointers(parent,0,0x10000200)==-E_FAULT && s1_pointers(parent,0x10000101,0x10000200)==-E_FAULT &&
        s1_pointers(parent,0x10000100,USER_TOP)==-E_FAULT && s1_pointers(parent,0x10000100,0x10000ff8)==-E_FAULT);
    strcpy(parent->cwd,"/home/port-test");
    struct task *h=p1_test_task(80000);s1_unit_inherit(h,parent,true);
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct s1_filesystem *s=s1_state_locked(parent);
    kassert(s && s==s1_state_locked(h) && s->references==2 && parent->cwd==h->cwd);
    spin_unlock_irqrestore(&c2_lock,irq);
    p1_test_free(h);s1_clone_abort(parent);kassert(parent->cwd==oldcwd && user_budget_pages(d->budget)==before);
    h=p1_test_task(80001);s1_unit_inherit(h,parent,true);kassert(!pidns_publish(h) && !s1_publish(h,parent));userns_task_live(h,true);
    register_t q={.rax=SYS_prctl,.rdi=39};kassert(userns_call(h,&q)==userns_call(parent,&q));
    q.rdi=21;kassert(userns_call(h,&q)==userns_call(parent,&q));
    q=(register_t){.rax=8888,.rip=USER_MIN};kassert(seccomp_decide(h,&q)==seccomp_decide(parent,&q));
    kassert(s1_mask(h,true,0077)==parent->context->creation_mask && s1_mask(parent,false,0)==0077);
    struct mount mount={.ops=&proc_ops};struct vnode root;
    make_node(&mount,d,P_FDINFO,h->userns->group,h->tgid,&root);root.id=(h->userns->group<<16)|P_FDINFO;
    irq=spin_lock_irqsave(&c2_lock);s=s1_state_locked(parent);kassert(s->references==2);
    uint64_t caps=h->userns->effective;h->userns->effective&=~(1ULL<<18);
    kassert(s1_root_commit_locked(h,&root)==-E_PERM);h->userns->effective=caps;
    struct vnode bad=root;bad.aux[2]=parent->userns->group;kassert(s1_root_commit_locked(h,&bad)==-E_PERM);
    bad=root;bad.aux[1]=P_PROCESS;kassert(s1_root_commit_locked(h,&bad)==-E_PERM);
    kassert(s1_root_commit_locked(parent,&root)==-E_PERM);
    kassert(!s1_root_commit_locked(h,&root) && s1_root_commit_locked(h,&root)==-E_PERM);
    kassert(s->root_kind==S1_RESTRICTED && s->root.cache[0] && s->root.aux[2]==h->userns->group);
    spin_unlock_irqrestore(&c2_lock,irq);
    kassert(s1_cwd_check(parent)==-E_ACCES);s1_chdir(h,"/",&root);
    kassert(!s1_cwd_check(parent) && !strcmp(parent->cwd,"/"));
    struct task *copies[S1_STATES-1];
    for(unsigned i=0;i<S1_STATES-1;i++) {copies[i]=p1_test_task(80002+i);s1_unit_inherit(copies[i],parent,false);
        kassert(copies[i]->cwd!=parent->cwd && !strcmp(copies[i]->cwd,"/") && s1_restricted(copies[i]));}
    struct task *excess=p1_test_task(80002+S1_STATES);kassert(userns_inherit(excess,parent,false,false)==-E_NOSPC);p1_test_free(excess);
    userns_task_live(h,false);s1_helper_exit(h);p1_test_free(h);
    kassert(!s1_owner_alive(&root));irq=spin_lock_irqsave(&c2_lock);
    kassert(p1_find(a,80001) && s1_state_locked(parent)->borrower_outer_pid==0);spin_unlock_irqrestore(&c2_lock,irq);
    for(unsigned i=0;i<S1_STATES-1;i++)p1_test_free(copies[i]);
    /* Unit-record destruction, not a user-visible root restoration path. */
    irq=spin_lock_irqsave(&c2_lock);s1_group_free_locked(d,s1_group_locked(parent,false));parent->cwd=oldcwd;
    kassert(!p1_find(a,80001));for(unsigned i=0;i<S1_STATES;i++)kassert(!a->filesystem[i].used);
    spin_unlock_irqrestore(&c2_lock,irq);kassert(user_budget_pages(d->budget)==before);
    pr_info("S1_UNIT_PASS unpublished_records_only bounded_states rollback shared_cwd_umask CAP_owner_denials monotonic_root private_copy dead_owner_retention cleanup\n");
}
static void s1_verifier(void) {
    for(unsigned nnp=0;nnp<2;nnp++) {
        struct task *outer=p1_test_task(70000),*init=p1_test_task(70001);
        kassert(!userns_enroll(outer));struct user_budget *budget=user_budget_create(192);kassert(budget);
        kassert(user_space_budget_attach(&outer->space,budget,0) && user_map(&outer->space,0x10000000,VM_RDWR));
        struct userns_budget_claim claim={0};kassert(!userns_budget_begin(outer,budget,&claim));userns_budget_finish(&claim,true);
        if(nnp)p1_test_filter(outer);
        kassert(!p1_test_inherit(init,outer,true));init->power_creator=outer;init->ppid=outer->tgid;
        kassert(!pidns_clone_prepare(init,outer,0x30000011) && !pidns_publish(init));userns_task_live(init,true);
        init->userns->ns->uid_mapped=init->userns->ns->gid_mapped=true;
        kassert(user_map(&init->space,0x10000000,VM_RDWR) && user_map(&init->space,0x10001000,VM_READ));
        s1_selftest(init);p1_test_free(init);p1_test_free(outer);pidns_reap_deferred();
        kassert(!p1_domain && !p1_deferred_destroy && !user_budget_pages(budget));user_budget_put(budget);
    }
}
#endif
