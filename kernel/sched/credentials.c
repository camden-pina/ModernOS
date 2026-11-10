#include <credentials.h>
#include <accounts.h>
#include <waitid.h>
#include <sandbox.h>
#include <string.h>
#include <printk.h>
#include <userns.h>
#include <pidns.h>
#include <fs/unix_socket.h>
#include <pty.h>
#include <uapi/window.h>
#define TEST_ID 1000u
#define TEST_HOME "/home/port-test"
#ifndef CONFIG_ZYGOTE_TRACE
#define CONFIG_ZYGOTE_TRACE 0
#endif
unsigned credentials_uid(const struct task *t) { return t && t->user && t->context ? __atomic_load_n(&t->context->uid,__ATOMIC_ACQUIRE) : 0; }
unsigned credentials_gid(const struct task *t) { return t && t->user && t->context ? __atomic_load_n(&t->context->gid,__ATOMIC_ACQUIRE) : 0; }
unsigned credentials_fsuid(const struct task *t) {unsigned uid=credentials_uid(t);return !uid && t && t->context && t->context->desktop_fsuid?t->context->desktop_fsuid:t && t->context?t->context->fsuid:uid;}
unsigned credentials_fsgid(const struct task *t) {return !credentials_uid(t) && t && t->context && t->context->desktop_fsuid?t->context->desktop_fsuid:t && t->context?t->context->fsgid:credentials_gid(t);}
bool credentials_fs_restricted(const struct task *t) {return credentials_fsuid(t)!=0;}
bool credentials_restricted(const struct task *t) { return credentials_uid(t)!=0; }
bool credentials_native_network(const struct task *t) {
    return t && t->user && t->context && !t->browser_sandbox &&
        !t->context->web_role && !pidns_active(t) && t->space.vm &&
        ((!t->userns && !t->space.vm->budget) || userns_browser_network(t));
}
static struct task *caller(void) { return sched_ready()?this_cpu()->current:NULL; }
/* Ordinary files use inode ownership and mode checks. Namespace roots,
   browser policies and private descriptors enforce their own boundaries. */
int credentials_node(struct vnode *node,unsigned mode) {
    struct task *t=caller();
    if(!credentials_fs_restricted(t))return 0;
    if(node->mount && node->mount->ops->stat) {
        int error=node->mount->ops->stat(node);if(error)return error;
    }
    unsigned bits=node->mode>>(node->uid==credentials_fsuid(t)?6:credentials_in_group(t,node->gid)?3:0);
    return (bits&mode)==mode?0:-E_ACCES;
}
int credentials_owner(struct vnode *node) {
    struct task *t=caller();
    if(!credentials_fs_restricted(t))return 0;
    if(node->mount && node->mount->ops->stat) {
        int error=node->mount->ops->stat(node);if(error)return error;
    }
    return node->uid==credentials_fsuid(t)?0:-E_PERM;
}
int credentials_file(const struct file *f) {
    int proc=userns_proc_file(f);if(proc)return proc;
    if(!credentials_restricted(caller()))return 0;
    unsigned kind=f->node.mode&0170000;
    if(pty_private_file(caller(),f) || accounts_terminal(caller(),f) || accounts_console(caller(),f))return 0;
    if(f->socket && credentials_native_network(caller()))return 0;
    if(f->tty || f->pty || f->socket || f->capability_domain || f->capability_renderer ||
       kind==0060000 || (kind==S_IFCHR && !f->device && !f->event && !f->epoll && !f->memory && !f->inotify))return -E_PERM;
    return 0;
}
static int ready(struct task *t) {
    if(t->tgid==1 || t->browser_sandbox || t->context->web_role ||
       !credentials_private_transition(t))return -E_NOTSUP;
    if(strcmp(t->cwd,TEST_HOME))return -E_ACCES;
    /* Anonymous deliberately inherited channels are capabilities, never
       ambient filesystem/terminal/display authority. The QA launcher closes
       extraneous descriptors before attempting this irreversible drop. */
    for(unsigned i=0;i<OS_FD_MAX;i++) {
        struct file *f=t->fds[i];
        if(f && (!f->pipe && !f->unix_socket && !f->memory))return -E_PERM;
        if(f && (f->capability_domain || f->capability_renderer))return -E_PERM;
    }
    if(t->context->job_terminal)return -E_PERM;
    mutex_lock(&vfs_mutex);struct vnode home;
    int e=vfs_lookup(TEST_HOME,&home);
    if(!e && (strcmp(home.mount->type,"ramfs") || home.uid!=TEST_ID ||
              home.gid!=TEST_ID || (home.mode&0777)!=0700 || (home.mode&0170000)!=S_IFDIR))e=-E_ACCES;
    mutex_unlock(&vfs_mutex);return e;
}
static long credentials_legacy_ids(struct task *t,register_t *r) {
    if(!CONFIG_CREDENTIALS_TEST)return -E_NOSYS;
    bool group=r->rax==SYS_getresgid || r->rax==SYS_setresgid || r->rax==SYS_getgroups || r->rax==SYS_setgroups;
    unsigned current=group?credentials_gid(t):credentials_uid(t);
    if(r->rax==SYS_getgroups)return r->rdi>0x7fffffff?-E_INVAL:0;
    if(r->rax==SYS_setgroups) {
        if(userns_enabled(t))return userns_call(t,r);
        return r->rdi?-E_PERM:credentials_restricted(t)?-E_PERM:ready(t);
    }
    if(r->rax==SYS_getresuid || r->rax==SYS_getresgid) {
        current=group?userns_gid_view(t,current):userns_uid_view(t,current);
        if(!user_access(&t->space,r->rdi,4,true) || !user_access(&t->space,r->rsi,4,true) ||
           !user_access(&t->space,r->rdx,4,true))return -E_FAULT;
        return user_copy_out(&t->space,r->rdi,&current,4) &&
            user_copy_out(&t->space,r->rsi,&current,4) && user_copy_out(&t->space,r->rdx,&current,4)?0:-E_FAULT;
    }
    uint64_t values[3]={r->rdi,r->rsi,r->rdx};bool unchanged=true,drop=true;
    for(unsigned i=0;i<3;i++) {
        if(values[i]>>32 && values[i]!=UINT64_MAX)return -E_INVAL;
        unsigned v=values[i];unchanged&=v==UINT32_MAX || v==current;drop&=v==TEST_ID;
    }
    if(userns_enabled(t)) {
        unsigned visible=group?userns_gid_view(t,current):userns_uid_view(t,current);
        for(unsigned i=0;i<3;i++)if((unsigned)values[i]!=UINT32_MAX && (unsigned)values[i]!=visible)return -E_PERM;
        return 0;
    }
    if(unchanged)return 0;
    if(!drop || credentials_uid(t) || (group && t->context->gid_dropped))return -E_PERM;
    if(!group && (credentials_gid(t)!=TEST_ID || !t->context->gid_dropped))return -E_PERM;
    int e=ready(t);if(e)return e;
    if(group){t->context->rgid=t->context->sgid=t->context->fsgid=TEST_ID;__atomic_store_n(&t->context->gid,TEST_ID,__ATOMIC_RELEASE);t->context->gid_dropped=true;}
    else {
        e=userns_enroll(t);if(e)return e;
        t->context->ruid=t->context->suid=t->context->fsuid=TEST_ID;
        __atomic_store_n(&t->context->uid,TEST_ID,__ATOMIC_RELEASE);
    }
    return 0;
}
long credentials_ids(struct task *t,register_t *r) {
    if(userns_enabled(t)) {
        if(r->rax==SYS_setuid || r->rax==SYS_setgid || r->rax==SYS_setreuid || r->rax==SYS_setregid || r->rax==SYS_setfsuid || r->rax==SYS_setfsgid)return -E_NOTSUP;
        return credentials_legacy_ids(t,r);
    }
    bool drop=(r->rax==SYS_setresuid || r->rax==SYS_setresgid) && r->rdi==TEST_ID && r->rsi==TEST_ID && r->rdx==TEST_ID;
    if(!t->context->account_uid && !credentials_uid(t) && !strcmp(t->cwd,TEST_HOME) && (drop || r->rax==SYS_setgroups))return credentials_legacy_ids(t,r);
    return credentials_posix_ids(t,r);
}
int credentials_syscall(struct task *t,const register_t *r) {
    if(!credentials_restricted(t))return 0;
    /* Clipboard still checks foreground terminal or owned window focus. */
    if(r->rax==SYS_clipboard && accounts_window_client(t))return 0;
    if(r->rax==SYS_window && accounts_window_client(t)) {
        switch(r->rdi) {
        case OS_WINDOW_CREATE:case OS_WINDOW_CLIENT_CLOSE:case OS_WINDOW_PRESENT:
        case OS_WINDOW_INPUT:case OS_WINDOW_KEYBOARD_STATE:case OS_WINDOW_GEOMETRY:
        case OS_WINDOW_REQUEST_STATE:case OS_WINDOW_CURSOR:case OS_WINDOW_PRESENT_DAMAGE:return 0;
        default:return -E_PERM;
        }
    }
    if(r->rax>=SYS_spawn && r->rax<=SYS_window && r->rax!=SYS_spawn &&
       r->rax!=SYS_spawn_io && r->rax!=SYS_spawnve && r->rax!=SYS_memory_usage)return -E_PERM;
    if(t->userns && (r->rax==SYS_chmod || r->rax==SYS_fchmod) && (r->rsi&06000))return -E_NOTSUP;
    switch(r->rax) {
    case SYS_ioctl:
        /* Per-descriptor CLOEXEC is the same authority as F_SETFD. */
        if(r->rsi==0x5450 || r->rsi==0x5451)return 0;
        if((r->rsi==0x541b || r->rsi==0x5421) && credentials_native_network(t) &&
           r->rdi<OS_FD_MAX && t->fds[r->rdi] && t->fds[r->rdi]->socket)return 0;
        if(pty_private_ioctl(t,r->rdi,r->rsi))return 0;
        if(r->rdi<OS_FD_MAX && (accounts_terminal(t,t->fds[r->rdi]) || accounts_console(t,t->fds[r->rdi]))) {
            switch(r->rsi) {
            case TCGETS:case TCSETS:case TCSETSW:case TCSETSF:case TCFLSH:
            case TIOCGPGRP:case TIOCSPGRP:case TIOCGWINSZ:case TIOCSWINSZ:case FIONREAD:case 0x5421:return 0;
            default:return -E_PERM;
            }
        }
        /* Anonymous pipes/Unix endpoints already carry descriptor authority;
           FIONBIO changes only O_NONBLOCK, just like F_SETFL. */
        if(r->rsi==0x5421 && r->rdi<OS_FD_MAX && t->fds[r->rdi] &&
           (t->fds[r->rdi]->pipe || t->fds[r->rdi]->unix_socket))return 0;
        /* Preserve the existing admitted libuv memory-channel branch. */
        if(r->rsi==0x5421 && r->rdi<OS_FD_MAX && t->fds[r->rdi] &&
           (t->fds[r->rdi]->pipe || t->fds[r->rdi]->unix_socket) &&
           userns_admitted_account(t))return 0;
        return -E_PERM;
    case SYS_chroot:
        /* Chromium's namespace helper must reach the existing S1 handler,
           which admits only its verified proc root and rejects other paths. */
        return t->userns?0:-E_PERM;
    case SYS_mount:case SYS_umount2:case SYS_sync:
        return -E_PERM;
    case SYS_chown:case SYS_fchown:case SYS_lchown:case SYS_fchownat:
        return t->userns?-E_PERM:0;
    case SYS_socket:case SYS_connect:case SYS_bind:case SYS_listen:case SYS_accept:case SYS_accept4:
        if(credentials_native_network(t))return 0;
        return unix_private_call(t,r)?0:-E_PERM;
    default:return 0;
    }
}
/* Bounded diagnostic evidence only for the admitted Electron account. No
   syscall result, namespace/capability/filter policy or errno is changed. */
void credentials_trace(struct task *t,const register_t *r,long result) {
    if(!CONFIG_CREDENTIALS_TEST || !credentials_restricted(t) || !t->space.vm->budget)return;
    if(result<0 && (r->rax==SYS_mmap || r->rax==SYS_mprotect || r->rax==SYS_mremap ||
                   r->rax==SYS_socketpair || r->rax==SYS_epoll_create1))
        pr_warn("ELECTRON_ABI_DENIAL pid=%u tid=%u number=%llu arg0=%llx arg1=%llx arg2=%llx result=%ld\n",
            t->tgid,t->pid,(unsigned long long)r->rax,(unsigned long long)r->rdi,
            (unsigned long long)r->rsi,(unsigned long long)r->rdx,result);
    if(CONFIG_ZYGOTE_TRACE && r->rax==SYS_setsockopt && r->rsi==1 && r->rdx==16)
        pr_info("C2_SOCKET_OPTION fd=%llu level=1 option=16 size=%llu result=%ld\n",
            (unsigned long long)r->rdi,(unsigned long long)r->r8,result);
    if(CONFIG_ZYGOTE_TRACE && (r->rax==56 || r->rax==125 || r->rax==126 || r->rax==157 || r->rax==272 || r->rax==317))
        pr_info("C1 ABI number=%llu arg0=%llx arg1=%llx result=%ld\n",
            (unsigned long long)r->rax,(unsigned long long)r->rdi,(unsigned long long)r->rsi,result);
}
/* Saturating metadata-only diagnostic, no allocation or per-task fields.
   No payload, path, kernel pointer or user-visible policy/result is changed. */
#if CONFIG_ZYGOTE_TRACE
unsigned credentials_zygote_record(const struct task *t) {
    if(!CONFIG_ZYGOTE_TRACE || !CONFIG_CREDENTIALS_TEST || !t || !t->userns ||
       !credentials_restricted(t) || !t->space.vm || !t->space.vm->budget)return 0;
    static unsigned count;
    unsigned n=__atomic_load_n(&count,__ATOMIC_RELAXED);
    while(n<1024)if(__atomic_compare_exchange_n(&count,&n,n+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {
        if(n==1023)pr_info("G1_TRACE_LIMIT events=1024 subsequent_metadata_omitted=1\n");
        return n+1;
    }
    return 0;
}
void credentials_zygote_syscall(const struct task *t,const register_t *r,uint64_t nr,long result,bool before) {
    bool selected=nr==SYS_clone || nr==SYS_fork || nr==SYS_vfork || nr==SYS_execve ||
        nr==SYS_wait4 || (CONFIG_WAITID_COMPAT_TEST && nr==SYS_waitid) || nr==SYS_close_range || nr==SYS_dup2 || nr==SYS_dup3 ||
        nr==SYS_fcntl || nr==SYS_poll || nr==SYS_ppoll || nr==SYS_recvmsg || nr==SYS_sendmsg ||
        nr==SYS_sendto || nr==SYS_recvfrom || nr==SYS_unshare ||
        (CONFIG_PID_NAMESPACE_TEST && (nr==SYS_getpid || nr==SYS_getppid || nr==SYS_gettid || nr==SYS_set_tid_address));
    if(!selected)return;
    unsigned seq=credentials_zygote_record(t);if(!seq)return;
    /* Raw registers may change at exec; nr is the separately saved entry
       number. Pointers are deliberately never printed. */
    uint64_t arg=nr==SYS_poll || nr==SYS_ppoll?r->rsi:(nr==SYS_execve || nr==SYS_getpid || nr==SYS_getppid || nr==SYS_gettid || nr==SYS_set_tid_address)?0:r->rdi;
    uint64_t flags=nr==SYS_sendmsg || nr==SYS_recvmsg?r->rdx:nr==SYS_fcntl?r->rsi:nr==SYS_waitid?(unsigned)r->r10:0;
    if(nr==SYS_waitid && before)pr_info("W1_WAITID_ARGS seq=%u pid=%u which=%d id=%d options=%x info_nonnull=%u usage_nonnull=%u\n",
        seq,t->tgid,(int)r->rdi,(int)r->rsi,(unsigned)r->r10,r->rdx!=0,r->r8!=0);
    pr_info("G1_CALL seq=%u pid=%u tid=%u phase=%s nr=%llu arg=%lld flags=%llx result=%ld\n",
        seq,t->tgid,t->pid,before?"begin":"end",(unsigned long long)nr,(long long)arg,(unsigned long long)flags,result);
}
#endif
