#if CONFIG_PID_NAMESPACE_TEST
static int p1_signal_row(struct p1_arena *a,unsigned target,bool create) {
    for(unsigned i=0;i<TASK_MAX;i++)if(a->signal_target[i]==target)return i;
    if(create)for(unsigned i=0;i<TASK_MAX;i++)if(!a->signal_target[i]){a->signal_target[i]=target;return i;}
    return -1;
}
static void p1_signal_clear_locked(struct p1_arena *a,unsigned row,unsigned sig) {
    struct p1_signal_record *m=&a->signals[row][sig-1];
    if(m->valid && m->sender)p1_put(a,&a->ids[m->sender-1]);
    memset(m,0,sizeof(*m));
    for(unsigned i=0;i<64;i++)if(a->signals[row][i].valid)return;
    a->signal_target[row]=0;
}
int pidns_signal_record(const struct task *sender,struct task *target,unsigned sig,int code,unsigned status) {
    if(!sig || sig>64)return -E_INVAL;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct c2_domain *d=p1_domain;int result=0;
    if(!d || !d->pids->committed || !target->user)goto done;
    bool related=(sender && sender->userns && sender->userns->domain==d) || (target->userns && target->userns->domain==d);
    if(!related)goto done;
    uint64_t bit=1ULL<<(sig-1);
    if(__atomic_load_n(&target->signal_pending,__ATOMIC_ACQUIRE)&bit){result=1;goto done;}
    unsigned uid=sender && sender->user?credentials_uid(sender):0;
    if(!sender || !sender->user)code=128;
    /* This C1 guest has no setter for arbitrary IDs: only outer0/1000.
       Fail closed rather than truncate if that invariant ever changes. */
    if((uid!=0 && uid!=1000) || status>255 || !(code==0 || code==-6 || code==128 || (code>=1 && code<=6))){result=-E_INVAL;goto done;}
    struct p1_arena *a=d->pids;bool fresh=p1_signal_row(a,target->pid,false)<0;int row=p1_signal_row(a,target->pid,true);
    if(row<0){result=-E_NOSPC;goto done;}
    struct p1_identity *id=NULL;
    if(sender && sender->user) {
        id=p1_find(a,sender->tgid);
        if(id)p1_get(a,id);else id=p1_new(a,sender->tgid,0);
        if(!id){if(fresh)a->signal_target[row]=0;result=-E_NOSPC;goto done;}
    }
    struct p1_signal_record *m=&a->signals[row][sig-1];kassert(!m->valid);
    *m=(struct p1_signal_record){.sender=id?p1_index(a,id):0,.code=(int8_t)code,.uid=uid,.status=status,.valid=1};
    __atomic_fetch_or(&target->signal_pending,bit,__ATOMIC_RELEASE);result=1;
done:spin_unlock_irqrestore(&c2_lock,irq);return result;
}
bool pidns_signal_take(struct task *t,unsigned sig,struct pidns_signal_info *info) {
    *info=(struct pidns_signal_info){0};
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct c2_domain *d=p1_domain;bool handled=false,destroy=false;
    if(d && d->pids->committed) {
        struct p1_arena *a=d->pids;int row=p1_signal_row(a,t->pid,false);
        handled=row>=0 || (t->userns && t->userns->domain==d);
        if(handled) {
            __atomic_fetch_and(&t->signal_pending,~(1ULL<<(sig-1)),__ATOMIC_ACQ_REL);
            if(row>=0) {
                struct p1_signal_record m=a->signals[row][sig-1];
                if(m.valid){info->valid=true;info->pid=m.sender?p1_view_locked(t,&a->ids[m.sender-1]):0;info->uid=m.uid;info->code=m.code==-128?128:m.code;info->status=m.status;}
                p1_signal_clear_locked(a,row,sig);destroy=!d->references;
            }
        }
    }
    if(destroy){kassert(!p1_deferred_destroy || p1_deferred_destroy==d);p1_deferred_destroy=d;}
    spin_unlock_irqrestore(&c2_lock,irq);return handled;
}
bool pidns_signal_drop(struct task *t,unsigned sig) {
    struct pidns_signal_info info;return pidns_signal_take(t,sig,&info);
}
void pidns_signal_cleanup(struct task *t) {
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct c2_domain *d=p1_domain;bool destroy=false;
    if(d && d->pids->committed) {
        struct p1_arena *a=d->pids;int row=p1_signal_row(a,t->pid,false);
        if(row>=0){for(unsigned sig=1;sig<=64;sig++)p1_signal_clear_locked(a,row,sig);destroy=!d->references;}
    }
    if(destroy){kassert(!p1_deferred_destroy || p1_deferred_destroy==d);p1_deferred_destroy=d;}
    spin_unlock_irqrestore(&c2_lock,irq);
}
#else
int pidns_signal_record(const struct task *s,struct task *t,unsigned n,int c,unsigned x){(void)s;(void)t;(void)n;(void)c;(void)x;return 0;}
bool pidns_signal_take(struct task *t,unsigned n,struct pidns_signal_info *i){(void)t;(void)n;*i=(struct pidns_signal_info){0};return false;}
bool pidns_signal_drop(struct task *t,unsigned n){(void)t;(void)n;return false;}
void pidns_signal_cleanup(struct task *t){(void)t;}
#endif
