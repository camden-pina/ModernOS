#if CONFIG_PID_NAMESPACE_TEST
static struct p1_namespace *p1_namespace_find(struct c2_domain *d,uint64_t serial) {
    if(!d->pids || serial==1)return NULL;
    for(unsigned i=0;i<P1_NAMESPACES;i++)if(d->pids->ns[i].used && d->pids->ns[i].id==serial)return &d->pids->ns[i];
    return NULL;
}
static uint64_t p1_namespace_serial(const struct userns_credential *r) {
    unsigned ns=r->pid_identity?r->pid_identity->ns:0;
    return ns?r->domain->pids->ns[ns-1].id:1;
}
static void p1_proc_retain(struct c2_domain *d,uint64_t serial) {
    struct p1_namespace *n=p1_namespace_find(d,serial);
    if(serial!=1){kassert(n && n->references<UINT32_MAX);n->references++;}
}
static void p1_proc_release(struct c2_domain *d,uint64_t serial) {
    struct p1_namespace *n=p1_namespace_find(d,serial);
    if(serial!=1){kassert(n);p1_ns_put(d->pids,n-d->pids->ns+1);}
}
static int p1_status_list(struct c2_domain *d,char *out,size_t size,const char *key,unsigned outer) {
    int length=snprintf(out,size,"%s:\t%u",key,outer);
    struct p1_identity *id=d->pids?p1_find(d->pids,outer):NULL;
    if(id)for(unsigned i=0;i<id->depth;i++)length+=snprintf(out+length,size-length,"\t%u",id->number[i]);
    length+=snprintf(out+length,size-length,"\n");return length;
}
static int p1_proc_status(struct task *t,char *out,size_t size) {
    struct userns_credential *r=t->userns;unsigned threads=0;
    for(unsigned i=0;i<C2_TASKS;i++)if(r->domain->creds[i].used && r->domain->creds[i].live && r->domain->creds[i].group==r->group)threads++;
    unsigned parent=__atomic_load_n(&t->ppid,__ATOMIC_ACQUIRE),pid=p1_view_outer_locked(t,t->tgid);
    int n=snprintf(out,size,"Tgid:\t%u\nPid:\t%u\nPPid:\t%u\nThreads:\t%u\n",pid,pid,p1_view_outer_locked(t,parent),threads);
    n+=p1_status_list(r->domain,out+n,size-n,"NStgid",t->tgid);
    n+=p1_status_list(r->domain,out+n,size-n,"NSpid",t->tgid);
    n+=p1_status_list(r->domain,out+n,size-n,"NSpgid",t->context->pgid);
    n+=p1_status_list(r->domain,out+n,size-n,"NSsid",t->context->sid);
    kassert(n>0 && (size_t)n<size);return n;
}
#else
static uint64_t p1_namespace_serial(const struct userns_credential *r){(void)r;return 1;}
static void p1_proc_retain(struct c2_domain *d,uint64_t n){(void)d;(void)n;}
static void p1_proc_release(struct c2_domain *d,uint64_t n){(void)d;(void)n;}
static int p1_proc_status(struct task *t,char *p,size_t n){(void)t;(void)p;(void)n;return -E_NOTSUP;}
#endif
