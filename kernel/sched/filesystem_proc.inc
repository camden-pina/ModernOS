#if CONFIG_FILESYSTEM_SANDBOX_TEST
static int proc_readdir(const struct vnode *,uint64_t *,struct directory_entry *);
static long proc_write(struct vnode *,uint64_t,const void *,size_t);
static bool s1_proc_available(void) {
    struct task *t=current();uint64_t irq=spin_lock_irqsave(&c2_lock);
    bool allowed=t && t->userns && t->userns->pid_identity &&
        t->userns->domain->pids && t->userns->domain->pids->committed &&
        t->space.vm && t->space.vm->budget==t->userns->domain->budget && t->userns->domain->budget_committed &&
        credentials_uid(t)==1000 && credentials_gid(t)==1000 && !t->browser_sandbox && !t->context->web_role;
    spin_unlock_irqrestore(&c2_lock,irq);return allowed;
}
static bool s1_proc_kind(unsigned kind) {return kind>=P_FDS && kind<=P_FDLEAF;}
bool s1_fd_magic(const struct vnode *n) {return userns_proc_node(n) && n->aux[1]==P_FDLINK;}
static bool s1_owner_alive(const struct vnode *n) {
    struct c2_domain *d=(void *)n->aux[0];bool alive=false;
    uint64_t irq=spin_lock_irqsave(&c2_lock);
    for(unsigned i=0;i<C2_TASKS;i++)if(d->creds[i].used && d->creds[i].live && d->creds[i].group==n->aux[2])alive=true;
    spin_unlock_irqrestore(&c2_lock,irq);return alive;
}
static int s1_proc_node(struct mount *m,unsigned kind,unsigned fd,struct vnode *out) {
    struct task *t=current();struct userns_credential *r=t->userns;
    if(!s1_proc_available())return -E_ACCES;
    if(r->group>UINT64_MAX/65536 || fd>=OS_FD_MAX)return -E_NOSPC;
    make_node(m,r->domain,kind,r->group,t->tgid,out);
    out->id=(r->group<<16)|((uint64_t)fd<<8)|kind;
    if(kind==P_FDLINK || kind==P_FDLEAF)out->cache[1]=fd+1;
    return 0;
}
static int s1_target(const struct vnode *n,struct file **out) {
    struct userns_credential *r;int e=proc_authorize(n,&r);if(e)return e;
    if(!r || n->aux[2]!=r->group || !n->cache[1] || n->cache[1]>OS_FD_MAX)return -E_ACCES;
    struct file *f=current()->fds[n->cache[1]-1];if(!f || !f->references)return -E_NOENT;
    /* VFS mutex protects close/reuse; transient access never creates a new
       persistent descriptor-reference edge or SCM cycle. */
    *out=f;return 0;
}
static int s1_fd_stat(const struct vnode *n,struct vnode *out) {
    struct file *f;int e=s1_target(n,&f);if(e)return e;
    *out=f->node;
    if(f->memory)out->size=memfd_size(f->memory);
    else if(out->mount && out->mount->ops->stat)e=out->mount->ops->stat(out);
    return e;
}
static long s1_proc_read(struct vnode *node,uint64_t offset,void *out,size_t size) {
    struct file *f;int e=s1_target(node,&f);if(e)return e;
    char text[OS_PATH_MAX+32];int n;
    if(node->aux[1]==P_FDLINK)n=snprintf(text,sizeof(text),"%s%s",f->path,f->unlinked?" (deleted)":"");
    else n=snprintf(text,sizeof(text),"pos:\t%llu\nflags:\t0%o\n",(unsigned long long)f->offset,
        f->flags|(__atomic_load_n(&current()->fd_flags[node->cache[1]-1],__ATOMIC_ACQUIRE)&FD_CLOEXEC?O_CLOEXEC:0));
    if(n<0 || n>=(int)sizeof(text))return -E_NAMETOOLONG;
    if(offset>=(unsigned)n)return 0;
    size=min(size,(size_t)n-offset);memcpy(out,text+offset,size);return size;
}
/* Relative retained-proc capability walk. No pathname prefix, global fallback
 * or arbitrary symlink target; every step keeps existing domain/group checks. */
int s1_at_lookup(struct task *t,int fd,const char *raw,bool nofollow,bool metadata,struct vnode *out) {
    if(!s1_restricted(t) || !raw[0] || raw[0]=='/' || fd==AT_FDCWD)return 1;
    if(fd<0 || fd>=OS_FD_MAX || !t->fds[fd])return -E_BADF;
    struct vnode node=t->fds[fd]->node;
    if((node.mode&0170000)!=S_IFDIR)return -E_NOTDIR;
    if(s1_root_matches(&node))return 1;
    if(!userns_proc_node(&node))return -E_ACCES;
    struct userns_credential *r;int e=proc_authorize(&node,&r);if(e)return e;
    if(!r)return -E_ACCES;
    for(size_t at=0;raw[at];) {
        while(raw[at]=='/')at++;
        size_t first=at;while(raw[at] && raw[at]!='/')at++;
        size_t n=at-first;if(!n)break;
        if(n>OS_NAME_MAX)return -E_NAMETOOLONG;
        if(n==1 && raw[first]=='.')continue;
        if(n==2 && raw[first]=='.' && raw[first+1]=='.')return -E_ACCES;
        char name[OS_NAME_MAX+1];memcpy(name,raw+first,n);name[n]=0;
        e=proc_lookup(&node,name,&node);if(e)return e;
        size_t rest=at;while(raw[rest]=='/')rest++;
        bool final=!raw[rest];
        if((node.mode&0170000)==S_IFLNK && (!final || !nofollow)) {
            if(node.aux[1]==P_SELF)make_node(node.mount,r->domain,P_PROCESS,r->group,0,&node);
            else if(node.aux[1]==P_FDLINK) {
                if(!final || !metadata)return -E_ACCES;
                e=s1_fd_stat(&node,&node);if(e)return e;
            } else if(node.aux[1]==P_NS_LINK || node.aux[1]==P_PID_LINK) {
                if(!final)return -E_NOTDIR;
                make_node(node.mount,r->domain,node.aux[1]==P_PID_LINK?P_PID_OBJECT:P_NS_OBJECT,node.aux[2],node.aux[3],&node);
            } else return -E_NOTSUP;
        }
        if(!final && (node.mode&0170000)!=S_IFDIR)return -E_NOTDIR;
    }
    if(s1_fd_magic(&node) && !metadata)return -E_NOTSUP;
    e=credentials_node(&node,0);if(e)return e;*out=node;return 0;
}
int s1_at_base(struct task *t,int fd,const char *raw,const char **base) {
    if(!raw[0] || raw[0]=='/')return 0;
    if(fd==AT_FDCWD)return s1_cwd_check(t);
    if(!s1_restricted(t))return 0;
    if(fd<0 || fd>=OS_FD_MAX || !t->fds[fd])return -E_BADF;
    if(s1_root_matches(&t->fds[fd]->node)){*base="/";return 0;}
    return -E_ACCES; /* at-calls that support a proc capability intercept first */
}
static int s1_root_commit_locked(struct task *t,const struct vnode *node) {
    struct s1_filesystem *s=s1_state_locked(t);
    if(!p1_scope_locked(t) || !t->vfork_parent || !t->userns->live || !s || s->borrower_outer_pid!=t->pid ||
       (s->root_kind&S1_RESTRICTED) || !p1_mapped(t->userns->ns) || !t->userns->ns ||
       !(t->userns->effective&(1ULL<<18)) || !userns_proc_node(node) || node->aux[1]!=P_FDINFO ||
       node->aux[0]!=(uintptr_t)t->userns->domain || node->aux[2]!=t->userns->group || node->aux[3]!=t->tgid)return -E_PERM;
    if(s1_fault(t,8))return -E_NOMEM;
    struct p1_identity *id=p1_find(t->userns->domain->pids,t->tgid);kassert(id);p1_get(t->userns->domain->pids,id);
    if(s1_fault(t,9)){p1_put(t->userns->domain->pids,id);return -E_NOMEM;}
    s->root=*node;s->root.cache[0]=(uintptr_t)id;s->root_kind=S1_RESTRICTED;return 0;
}
long s1_chroot(struct task *t,uint64_t pointer) {
    if(!s1_borrowed(t))return -E_PERM;
    char path[OS_PATH_MAX];int e=user_string(&t->space,path,pointer,sizeof(path));if(e)return e;
    mutex_lock(&vfs_mutex);struct vnode node;
    e=vfs_lookup(path,&node);
    if(!e)e=credentials_node(&node,1);
    uint64_t irq=spin_lock_irqsave(&c2_lock);if(!e)e=s1_root_commit_locked(t,&node);
    spin_unlock_irqrestore(&c2_lock,irq);mutex_unlock(&vfs_mutex);
    if(!e)s1_verify_live(t,false);
    return e;
}
void s1_verify_live(struct task *t,bool parent) {
#if CONFIG_FILESYSTEM_SANDBOX_VERIFY
    if(!s1_restricted(t))return; /* explicit root-commit fault tests */
    mutex_lock(&vfs_mutex);struct vnode root,node;unsigned directories=0,proc_caps=0,stats=0;
    kassert(s1_root(&root) && !vfs_lookup("/",&node) && s1_root_matches(&node));
    kassert(proc_write(&root,0,"x",1)==-E_ROFS);
    kassert(vfs_mkdir("/home/port-test/s1-denied",0700)<0 && vfs_symlink("/", "/home/port-test/s1-denied")<0);
    kassert(vfs_lookup("/proc",&node)<0 && vfs_lookup("/../../home/port-test",&node)<0 &&
        vfs_lookup_stat("/proc/self/exe",&node)<0);
    for(unsigned fd=0;fd<OS_FD_MAX;fd++)if(t->fds[fd]) {
        struct vnode *base=&t->fds[fd]->node;
        if((base->mode&0170000)!=S_IFDIR || s1_root_matches(base))continue;
        kassert(s1_fchdir(t,base,t->fds[fd]->path)==-E_ACCES);directories++;
        if(!userns_proc_node(base) || base->aux[1]!=P_ROOT)continue;
        kassert(s1_at_lookup(t,fd,"../..",false,true,&node)==-E_ACCES);
        kassert(!s1_at_lookup(t,fd,"self/fd",false,true,&node) && (node.mode&0170000)==S_IFDIR);
        char path[64];snprintf(path,sizeof(path),"self/fd/%u",fd);
        kassert(!s1_at_lookup(t,fd,path,false,true,&node) && (node.mode&0170000)==S_IFDIR);stats++;
        kassert(s1_at_lookup(t,fd,path,false,false,&node)<0);
        snprintf(path,sizeof(path),"self/fd/%u/escape",fd);kassert(s1_at_lookup(t,fd,path,false,true,&node)==-E_ACCES);
        proc_caps++;
    }
    if(parent) {
        uint64_t offset=2;struct directory_entry entry;kassert(!s1_owner_alive(&root) && !proc_readdir(&root,&offset,&entry));
        kassert(vfs_lookup("/0",&node)==-E_NOENT);
    } else kassert(s1_cwd_check(t)==-E_ACCES);
    pr_info("S1_LIVE_CHECK_PASS phase=%s real_admitted_context=1 rooted_paths_denied=1 outside_fchdir_denials=%u proc_caps=%u actual_directory_types=%u dead_root=%u cwd_inside=%u\n",
        parent?"parent_after_exit":"child_after_chroot",directories,proc_caps,stats,parent,!s1_cwd_check(t));
    mutex_unlock(&vfs_mutex);
#else
    (void)t;(void)parent;
#endif
}
#else
void s1_verify_live(struct task *t,bool p){(void)t;(void)p;}
static bool s1_proc_available(void){return false;}
static bool s1_proc_kind(unsigned k){(void)k;return false;}
bool s1_fd_magic(const struct vnode *n){(void)n;return false;}
static bool s1_owner_alive(const struct vnode *n){(void)n;return false;}
static int s1_proc_node(struct mount *m,unsigned k,unsigned f,struct vnode *n){(void)m;(void)k;(void)f;(void)n;return -E_NOTSUP;}
static int s1_target(const struct vnode *n,struct file **o){(void)n;(void)o;return -E_NOTSUP;}
static int s1_fd_stat(const struct vnode *n,struct vnode *o){(void)n;(void)o;return -E_NOTSUP;}
static long s1_proc_read(struct vnode *n,uint64_t o,void *b,size_t s){(void)n;(void)o;(void)b;(void)s;return -E_NOTSUP;}
int s1_at_lookup(struct task *t,int f,const char *p,bool n,bool m,struct vnode *o){(void)t;(void)f;(void)p;(void)n;(void)m;(void)o;return 1;}
int s1_at_base(struct task *t,int f,const char *p,const char **b){(void)t;(void)f;(void)p;(void)b;return 0;}
long s1_chroot(struct task *t,uint64_t p){(void)t;(void)p;return -E_NOSYS;}
#endif
