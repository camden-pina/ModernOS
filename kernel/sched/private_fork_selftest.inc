/* Boot-only unit records. Never published, executed or presented to Electron;
 * no user-facing account creation or fault-injection interface. */
#if CONFIG_PRIVATE_FORK_VERIFY
void private_fork_scope_selftest(void) {
    for(unsigned nnp=0;nnp<2;nnp++)for(unsigned limited=0;limited<2;limited++) {
        struct task *p=kmallocz(sizeof(*p)),*c=kmallocz(sizeof(*c));
        struct task_context *pc=kmallocz(sizeof(*pc)),*cc=kmallocz(sizeof(*cc));kassert(p && c && pc && cc);
        p->user=c->user=true;p->pid=p->tgid=77;c->pid=c->tgid=78;p->context=pc;c->context=cc;
        pc->references=2;cc->references=1;pc->uid=pc->gid=cc->uid=cc->gid=1000;
        p->syscall_locked=true;pc->syscall_lock.owner=p;
        kassert(user_space_create(&p->space) && !userns_enroll(p));
        struct user_budget *b=user_budget_create(limited?(nnp?17:4):64);kassert(b);
        kassert(user_space_budget_attach(&p->space,b,0) && user_map(&p->space,0x10000000,VM_RDWR));
        kassert(!private_fork_scope(p,0x01200011,0));
        struct userns_budget_claim claim={0};kassert(!userns_budget_begin(p,b,&claim));
        kassert(!private_fork_scope(p,0x01200011,0)); /* pending != admitted */
        userns_budget_finish(&claim,false);kassert(user_budget_pages(b)==1);
        kassert(!userns_budget_begin(p,b,&claim));userns_budget_finish(&claim,true);
        kassert(private_fork_scope(p,0x01200011,0));
        kassert(!private_fork_scope(p,17,0) && !private_fork_scope(p,0x1000011,0) &&
                !private_fork_scope(p,0x01200011|0x100,0) && !private_fork_scope(p,0x01200011,0x10000000));
        pc->uid=0;kassert(!private_fork_scope(p,0x01200011,0));pc->uid=1000;
        pc->gid=0;kassert(!private_fork_scope(p,0x01200011,0));pc->gid=1000;
        p->pid=76;kassert(!private_fork_scope(p,0x01200011,0));p->pid=77;
        p->browser_sandbox=true;kassert(!private_fork_scope(p,0x01200011,0));p->browser_sandbox=false;
        pc->web_role=1;kassert(!private_fork_scope(p,0x01200011,0));pc->web_role=0;
        struct userns_credential *saved=p->userns;p->userns=NULL;kassert(!private_fork_scope(p,0x01200011,0));p->userns=saved;
        struct user_budget *saved_budget=p->space.vm->budget;p->space.vm->budget=NULL;
        kassert(!private_fork_scope(p,0x01200011,0));p->space.vm->budget=saved_budget;
        register_t call={.r10=0};kassert(process_clone(p,&call,0x01200011,0)==-E_FAULT);
        call.r10=0x10000001;kassert(process_clone(p,&call,0x01200011,0)==-E_FAULT);
        call.r10=0x10001000;kassert(process_clone(p,&call,0x01200011,0)==-E_FAULT);
        call.r10=0x10000200;kassert(user_protect(&p->space,0x10000000,VM_READ));
        kassert(process_clone(p,&call,0x01200011,0)==-E_FAULT);
        kassert(user_protect(&p->space,0x10000000,VM_RDWR));
        pc->exiting=true;kassert(process_clone(p,&call,0x01200011,0)==-E_AGAIN);pc->exiting=false;
        pc->fork_owner=77;kassert(process_clone(p,&call,0x01200011,0)==-E_AGAIN);pc->fork_owner=0;
        p->vfork_parent=9;kassert(process_clone(p,&call,0x01200011,0)==-E_AGAIN);p->vfork_parent=0;
        p->syscall_locked=false;kassert(process_clone(p,&call,0x01200011,0)==-E_AGAIN);p->syscall_locked=true;
        pc->syscall_lock.owner=c;kassert(process_clone(p,&call,0x01200011,0)==-E_AGAIN);pc->syscall_lock.owner=p;
        register_t q={.rax=SYS_prctl,.rdi=39};kassert(userns_call(p,&q)==0);
        if(nnp) {
            q.rdi=38;q.rsi=1;kassert(!userns_call(p,&q));q.rdi=39;q.rsi=0;kassert(userns_call(p,&q)==1);
            struct {uint16_t code;uint8_t jt,jf;uint32_t k;} code[]={
                {0x20,0,0,0},{0x15,0,1,8888},{6,0,0,SC_ERRNO|77},{6,0,0,SC_ALLOW}};
            struct {uint16_t n;uint16_t pad[3];uint64_t p;} program={4,{0},0x10000040};
            kassert(user_copy_out(&p->space,0x10000040,code,sizeof(code)) && user_copy_out(&p->space,0x10000010,&program,sizeof(program)));
            register_t install={.rax=SYS_seccomp,.rdi=1,.rdx=0x10000010};kassert(!seccomp_call(p,&install));
        }
        uint32_t original=0x5a5a5a5a;kassert(user_copy_out(&p->space,0x10000200,&original,4));
        uint64_t before=user_budget_pages(b);kassert(!userns_inherit(c,p,false,false));
        bool copied=user_space_clone(&c->space,&p->space);kassert(copied!=limited);
        if(copied) {
            kassert(c->space.vm!=p->space.vm && c->space.root!=p->space.root && c->space.vm->budget==b);
            uint32_t changed=78,check=0;kassert(user_copy_out(&c->space,0x10000200,&changed,4));
            kassert(user_copy_in(&p->space,&check,0x10000200,4) && check==original);
            kassert(user_copy_in(&c->space,&check,0x10000200,4) && check==changed);
            register_t test={.rax=8888,.rip=USER_MIN};kassert(seccomp_decide(c,&test)==(nnp?(SC_ERRNO|77):SC_ALLOW));
        } else kassert(!c->space.root && user_budget_pages(b)==before);
        q=(register_t){.rax=SYS_prctl,.rdi=39};kassert(userns_call(c,&q)==nnp && userns_call(p,&q)==nnp);
        user_space_destroy(&c->space);userns_task_free(c);kassert(user_budget_pages(b)==before);
        /* Actual fixed C2 records: process/task exhaustion and complete reuse,
           without constructing any runnable task or exposing a helper account. */
        struct userns_credential *owners[64];
        for(unsigned i=0;i<31;i++){c->pid=c->tgid=100+i;kassert(!userns_inherit(c,p,false,false));owners[i]=c->userns;c->userns=NULL;}
        c->pid=c->tgid=200;kassert(userns_inherit(c,p,false,false)==-E_NOSPC && !c->userns);
        for(unsigned i=0;i<31;i++){c->userns=owners[i];userns_task_free(c);}
        for(unsigned i=0;i<63;i++){c->pid=100+i;c->tgid=p->tgid;kassert(!userns_inherit(c,p,true,false));owners[i]=c->userns;c->userns=NULL;}
        c->pid=200;kassert(userns_inherit(c,p,true,false)==-E_NOSPC && !c->userns);
        for(unsigned i=0;i<63;i++){c->userns=owners[i];userns_task_free(c);}
        kassert(user_budget_pages(b)==before);
        userns_task_free(p);user_space_destroy(&p->space);kassert(!user_budget_pages(b));user_budget_put(b);
        kfree(p);kfree(c);kfree(pc);kfree(cc);
    }
    pr_info("G2_SCOPE_SELFTEST_PASS exact_flags_account_commit_ID_role_leader_stack_TID NNP0_1_F1_inherit private_copy_budget_rollback process_task_bounds no_published_synthetic_tasks\n");
}
#endif
