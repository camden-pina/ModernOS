/* Linux PTY capability provider for the exact admitted ordinary account.
 * No devfs nodes, external device access, namespace/renderer grants, or root
 * terminal handles. Names resolve only against this retained account budget.
 * VFS serializes table/endpoint lifetime. Data/config use the existing PTY lock.
 * At most 16 live objects, each fully charged (including allocator overhead).
 */
enum { PRIVATE_PTY_LIMIT=16, PRIVATE_PTY_PAGES=(sizeof(struct pty)+256+PAGE_SIZE-1)/PAGE_SIZE };
_Static_assert(PRIVATE_PTY_PAGES*PRIVATE_PTY_LIMIT*PAGE_SIZE<=1024*1024,"private PTY pool charge bound");
static struct pty *private_ptys[PRIVATE_PTY_LIMIT];
static unsigned private_pty_serial;
static bool private_pty_admitted(const struct task *t) {
    return CONFIG_ELECTRON_PROFILE && t && credentials_uid(t)==1000 && credentials_gid(t)==1000 &&
        !t->browser_sandbox && !t->context->web_role && !pidns_active(t) && userns_admitted_account(t);
}
bool pty_private_file(const struct task *t,const struct file *f) {
    return f && f->pty && f->pty->budget && private_pty_admitted(t) &&
        t->space.vm->budget==f->pty->budget;
}
static void private_pty_release(struct pty *p) {
    if(!p->budget)return;
    for(unsigned i=0;i<PRIVATE_PTY_LIMIT;i++)if(private_ptys[i]==p)private_ptys[i]=NULL;
    struct user_budget *budget=p->budget;p->budget=NULL;
    user_budget_uncharge(budget,PRIVATE_PTY_PAGES);user_budget_put(budget);
}
static int private_pty_slave(struct task *t,struct pty *p,unsigned flags,struct file **out) {
    if(p->budget!=t->space.vm->budget)return -E_ACCES;
    if(p->locked || !p->master)return -E_IO;
    char path[64];snprintf(path,sizeof(path),"/dev/pts/%u",p->private_id);
    int error=vfs_pty_endpoint(p,false,flags,path,out);
    if(!error)p->slave++;
    return error;
}
int pty_private_open(struct task *t,const char *path,unsigned flags,struct file **out) {
    if(!private_pty_admitted(t))return 1;
    bool master=!strcmp(path,"/dev/ptmx"),ctty=!strcmp(path,"/dev/tty");
    if(!master && !ctty && strncmp(path,"/dev/pts/",9))return 1;
    if((flags&3)!=O_RDWR || flags&~(3u|O_CLOEXEC|O_NONBLOCK|O_NOCTTY|O_LARGEFILE|O_NOFOLLOW))return -E_INVAL;
    if(master) {
        unsigned slot=0;while(slot<PRIVATE_PTY_LIMIT && private_ptys[slot])slot++;
        if(slot==PRIVATE_PTY_LIMIT || private_pty_serial==0x7fffffffu)return -E_NOSPC;
        struct user_budget *budget=t->space.vm->budget;
        if(!user_budget_charge(budget,PRIVATE_PTY_PAGES))return -E_NOMEM;
        struct pty *p=pty_allocate();
        if(!p){user_budget_uncharge(budget,PRIVATE_PTY_PAGES);return -E_NOMEM;}
        p->slave=0;p->locked=true;p->private_id=++private_pty_serial;
        int error=vfs_pty_endpoint(p,true,flags,path,out);
        if(error){kfree(p);user_budget_uncharge(budget,PRIVATE_PTY_PAGES);return error;}
        p->budget=budget;user_budget_get(budget);private_ptys[slot]=p;return 0;
    }
    for(unsigned i=0;i<PRIVATE_PTY_LIMIT;i++) {
        struct pty *p=private_ptys[i];
        if(!p || p->budget!=t->space.vm->budget)continue;
        uint64_t irq=spin_lock_irqsave(&p->lock);
        bool matches=ctty && p->session && p->session==t->context->sid;
        spin_unlock_irqrestore(&p->lock,irq);
        if(!ctty) {char expected[64];snprintf(expected,sizeof(expected),"/dev/pts/%u",p->private_id);matches=!strcmp(path,expected);}
        if(matches)return private_pty_slave(t,p,flags,out);
    }
    return -E_NOENT;
}
bool pty_private_ioctl(const struct task *t,unsigned fd,unsigned request) {
    if(fd>=OS_FD_MAX)return false;
    mutex_lock(&vfs_mutex);bool owned=pty_private_file(t,t->fds[fd]);mutex_unlock(&vfs_mutex);
    if(!owned)return false;
    switch(request) {
    case TCGETS:case TCSETS:case TCSETSW:case TCSETSF:case TCFLSH:
    case TIOCGPGRP:case TIOCSPGRP:case TIOCGWINSZ:case TIOCSWINSZ:case FIONREAD:
    case 0x5421:case 0x540e:case 0x5429:case 0x80045430:case 0x40045431:case 0x80045439:case 0x5441:
        return true;
    default:return false;
    }
}
/* Return 1 only for the existing generic PTY requests; all new pointer writes
 * are checked before publication. A peer FD has its own endpoint reference.
 */
static long private_pty_control(struct task *t,struct file *f,unsigned request,uint64_t arg) {
    struct pty *p=f->pty;
    if(!pty_private_file(t,f))return -E_PERM;
    if(request==0x80045430 || request==0x80045439) {
        if(!f->pty_master)return -E_NOTTY;
        mutex_lock(&vfs_mutex);unsigned value=request==0x80045430?p->private_id:p->locked;mutex_unlock(&vfs_mutex);
        return user_copy_out(&t->space,arg,&value,4)?0:-E_FAULT;
    }
    if(request==0x40045431 || request==0x5421) {
        int value;if(!user_copy_in(&t->space,&value,arg,4))return -E_FAULT;
        if(request==0x5421){mutex_lock(&vfs_mutex);f->flags=(f->flags&~O_NONBLOCK)|(value?O_NONBLOCK:0);mutex_unlock(&vfs_mutex);return 0;}
        if(!f->pty_master)return -E_NOTTY;
        mutex_lock(&vfs_mutex);p->locked=!!value;mutex_unlock(&vfs_mutex);return 0;
    }
    if(request==0x5441) {
        if(!f->pty_master)return -E_NOTTY;
        if((arg&3)!=O_RDWR || arg&~(uint64_t)(3u|O_NOCTTY|O_CLOEXEC|O_NONBLOCK))return -E_INVAL;
        unsigned fd=0;while(fd<OS_FD_MAX && t->fds[fd])fd++;
        if(fd==OS_FD_MAX)return -E_MFILE;
        mutex_lock(&vfs_mutex);int error=private_pty_slave(t,p,arg,&t->fds[fd]);
        if(!error)t->fd_flags[fd]=(arg&O_CLOEXEC)?FD_CLOEXEC:0;
        mutex_unlock(&vfs_mutex);return error?error:(long)fd;
    }
    if(request==0x540e) { /* TIOCSCTTY: no forced stealing, session leader only. */
        if(f->pty_master || arg || t->context->sid!=t->tgid)return -E_PERM;
        uint64_t irq=spin_lock_irqsave(&p->lock);long result=-E_PERM;
        if(!p->session || p->session==t->context->sid) {
            p->session=t->context->sid;p->foreground=t->context->pgid;result=0;
        }
        spin_unlock_irqrestore(&p->lock,irq);return result;
    }
    if(request==0x5429) { /* TIOCGSID */
        uint64_t irq=spin_lock_irqsave(&p->lock);unsigned sid=p->session;spin_unlock_irqrestore(&p->lock,irq);
        return sid?(user_copy_out(&t->space,arg,&sid,4)?0:-E_FAULT):-E_NOTTY;
    }
    return 1;
}
