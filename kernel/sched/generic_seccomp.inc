/* Linux seccomp for ordinary processes. Immutable, reference-counted filter
 * chains survive every native spawn, fork, clone and exec path. The scheduler
 * lock serializes thread-wide attachment with task publication and cleanup. */
struct generic_seccomp_filter {
    struct generic_seccomp_filter *parent;
    unsigned references, length, cost;
    struct seccomp_insn instructions[];
};
static void generic_filter_get(struct generic_seccomp_filter *filter) {
    if(filter)__atomic_fetch_add(&filter->references,1,__ATOMIC_RELAXED);
}
static void generic_filter_put(struct generic_seccomp_filter *filter) {
    while(filter && __atomic_fetch_sub(&filter->references,1,__ATOMIC_ACQ_REL)==1) {
        struct generic_seccomp_filter *parent=filter->parent;
        kfree(filter);filter=parent;
    }
}
void generic_seccomp_inherit(struct task *child,const struct task *parent) {
    if(!parent || !parent->user)return;
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);
    child->seccomp_filter=parent->seccomp_filter;
    child->seccomp_mode=parent->seccomp_mode;
    generic_filter_get(child->seccomp_filter);
    spin_unlock_irqrestore(&scheduler_lock,irq);
}
void generic_seccomp_release(struct task *task) {
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);
    struct generic_seccomp_filter *filter=task->seccomp_filter;
    task->seccomp_filter=NULL;task->seccomp_mode=task->seccomp_signal_frames=0;
    spin_unlock_irqrestore(&scheduler_lock,irq);
    generic_filter_put(filter);
}
static bool generic_filter_ancestor(struct generic_seccomp_filter *head,
                                    struct generic_seccomp_filter *ancestor) {
    for(;;head=head->parent) {
        if(head==ancestor)return true;
        if(!head)return false;
    }
}
static long generic_filter_install(struct task *task,uint64_t address,bool sync) {
    if(credentials_uid(task) && !task->context->account_nnp)return -E_ACCES;
    struct {uint16_t length,padding[3];uint64_t instructions;} program;
    if(!user_copy_in(&task->space,&program,address,sizeof(program)))return -E_FAULT;
    if(!program.length || program.length>4096)return -E_INVAL;
    struct generic_seccomp_filter *filter=kmallocz(sizeof(*filter)+program.length*sizeof(struct seccomp_insn));
    uint32_t *states=kmalloc(program.length*sizeof(uint32_t));
    if(!filter || !states){kfree(filter);kfree(states);return -E_NOMEM;}
    long error=0;
    if(!user_copy_in(&task->space,filter->instructions,program.instructions,
                     program.length*sizeof(struct seccomp_insn)))error=-E_FAULT;
    else if(!seccomp_bpf_verify(filter->instructions,program.length,states,false))error=-E_INVAL;
    kfree(states);
    if(error){kfree(filter);return error;}
    filter->references=1;filter->length=program.length;
    struct generic_seccomp_filter *old[TASK_MAX];unsigned count=0;
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);
    if(task->seccomp_mode==1)error=-E_INVAL;
    unsigned cost=program.length+(task->seccomp_filter?task->seccomp_filter->cost+4:0);
    if(cost>32768)error=-E_NOMEM;
    if(!error && sync)for(unsigned i=0;i<TASK_MAX;i++) {
        struct task *other=&tasks[i];
        if(other->context==task->context && other->user &&
           other->state>=TASK_RUNNABLE && other->state<TASK_ZOMBIE &&
           (other->seccomp_mode==1 ||
            !generic_filter_ancestor(task->seccomp_filter,other->seccomp_filter))) {
            error=other->pid;break;
        }
    }
    if(!error) {
        filter->parent=task->seccomp_filter;generic_filter_get(filter->parent);filter->cost=cost;
        for(unsigned i=0;i<TASK_MAX;i++) {
            struct task *other=&tasks[i];
            if(other==task || (sync && other->context==task->context && other->user &&
                              other->state>=TASK_RUNNABLE && other->state<TASK_ZOMBIE)) {
                old[count++]=other->seccomp_filter;
                generic_filter_get(filter);
                other->seccomp_filter=filter;
                __atomic_store_n(&other->seccomp_mode,2,__ATOMIC_RELEASE);
            }
        }
    }
    spin_unlock_irqrestore(&scheduler_lock,irq);
    for(unsigned i=0;i<count;i++)generic_filter_put(old[i]);
    generic_filter_put(filter);
    return error;
}
long generic_seccomp_call(struct task *task,register_t *r) {
    if(r->rax==SYS_prctl && r->rdi==21)
        return __atomic_load_n(&task->seccomp_mode,__ATOMIC_ACQUIRE);
    unsigned operation=r->rax==SYS_prctl?(unsigned)r->rsi:(unsigned)r->rdi;
    uint64_t flags=r->rax==SYS_prctl?0:r->rsi;
    uint64_t argument=r->rdx;
    if(r->rax==SYS_prctl) {
        if(r->rdi!=22 || (operation!=1 && operation!=2))return -E_INVAL;
        operation--;
    }
    if(operation==0) {
        if(flags || task->seccomp_mode)return -E_INVAL;
        __atomic_store_n(&task->seccomp_mode,1,__ATOMIC_RELEASE);
        return 0;
    }
    if(operation==1) {
        if(flags&~1ULL)return -E_INVAL;
        return generic_filter_install(task,argument,flags&1);
    }
    if(operation==2) {
        uint32_t action;
        if(flags)return -E_INVAL;
        if(!user_copy_in(&task->space,&action,argument,sizeof(action)))return -E_FAULT;
        return !(action&0xffffu) && seccomp_bpf_action(action)?0:-E_NOTSUP;
    }
    return -E_INVAL;
}
uint32_t generic_seccomp_decide(struct task *task,const register_t *r) {
    if(!task || !task->user || !__atomic_load_n(&task->seccomp_mode,__ATOMIC_ACQUIRE))return SC_ALLOW;
    if(task->seccomp_mode==1)
        return r->rax==SYS_read || r->rax==SYS_write || r->rax==SYS_exit ||
               r->rax==SYS_rt_sigreturn?SC_ALLOW:SC_KILL_THREAD;
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);
    struct generic_seccomp_filter *head=task->seccomp_filter;generic_filter_get(head);
    spin_unlock_irqrestore(&scheduler_lock,irq);
    struct seccomp_data data[3]={{.number=(int32_t)r->rax,.arch=0xc000003e,.ip=r->rip,
                              .args={r->rdi,r->rsi,r->rdx,r->r10,r->r8,r->r9}}};
    unsigned operations=1;
    if(r->rax==SYS_spawn || r->rax==SYS_spawn_io || r->rax==SYS_spawnve ||
       r->rax==SYS_spawn_browser || r->rax==SYS_spawn_web) {
        /* Native spawn is also a process creation followed by an exec. A
           default-allow Linux policy denying either operation must not be
           bypassed by choosing Coast's combined syscall instead. */
        data[1]=(struct seccomp_data){.number=SYS_clone,.arch=0xc000003e,.ip=r->rip,.args={17,0,0,0,0,0}};
        data[2]=(struct seccomp_data){.number=SYS_execve,.arch=0xc000003e,.ip=r->rip,
            .args={r->rdi,r->rsi,r->rax==SYS_spawnve?r->rdx:0,0,0,0}};
        operations=3;
    } else if(r->rax==SYS_power) {
        data[1]=(struct seccomp_data){.number=169,.arch=0xc000003e,.ip=r->rip,
            .args={0xfee1dead,0x28121969,r->rdi==OS_POWER_OFF?0x4321fedc:0x01234567,0,0,0}};
        operations=2;
    }
    uint32_t result=SC_ALLOW;unsigned rank=4;
    for(struct generic_seccomp_filter *filter=head;filter;filter=filter->parent) {
      for(unsigned operation=0;operation<operations;operation++) {
        uint32_t value=seccomp_bpf_run(filter->instructions,filter->length,&data[operation]);
        if(!seccomp_bpf_action(value))value=SC_KILL_PROCESS;
        unsigned next=seccomp_bpf_rank(value);
        if(next<rank){rank=next;result=value;}
      }
    }
    generic_filter_put(head);
    return result;
}
