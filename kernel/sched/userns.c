#include <userns.h>
#include <credentials.h>
#include <sched.h>
#include <fs/vfs.h>
#include <mm/pmm.h>
#include <string.h>
#include <panic.h>
#include <printk.h>
#include <seccomp.h>
#include <seccomp_bpf.h>
#include <landlock.h>
#include <pidns.h>
#include <filesystem.h>
#include <fs/memfd.h>

/* Experimental, default-off C1 principal only. No Linux-global root or
 * capability shortcuts: all object authorization continues to use outer IDs.
 * Fixed arena plus a conservative page for embedded task/context additions
 * stay within a bounded150-page total; all are charged to Electron. */
#define C2_MAX_METADATA_PAGES 150
#define C2_NAMESPACES 32
/* Combined language services, Markdown and repeated search reached192 tasks
 * before spawning a search helper. Admit206 with32 native/helper slots;
 * this fits the bounded216-KiB filter and150-page total metadata ceilings. */
#define C2_TASKS 206
#define C2_PROCESSES 32
#define C2_DEPTH 4
#define C2_CAPS ((1ULL << 41)-1)
#define C2_CAP_VERSION 0x20080522u
struct c2_namespace {
    uint64_t id;
    unsigned references,depth;
    struct c2_namespace *parent;
    bool uid_mapped,gid_mapped,groups_denied;
};
struct c2_domain;
struct f1_filter;struct f1_arena;
struct p1_identity;struct p1_arena;
struct userns_credential {
    struct c2_domain *domain;
    struct c2_namespace *ns;
    uint64_t serial,group,effective,permitted,inheritable,bounding;
    unsigned pid,tgid;
    /* Per task, not task_context/thread group. Occupies existing tail padding;
       cred_allocate_locked copies the actual creating task under c2_lock. */
    bool used,live,no_new_privs;
    struct f1_filter *filter;
    uint8_t seccomp_frames;
#if CONFIG_PID_NAMESPACE_TEST
    struct p1_identity *pid_identity;
#endif
};
struct c2_domain {
    uintptr_t physical;
    unsigned references,tasks,processes,namespaces,pending_budget;
    unsigned peak_tasks,peak_processes,peak_namespaces;
    uint64_t serial, file_origin;
    struct user_budget *budget;
    bool budget_committed;
    struct f1_arena *filters;
#if CONFIG_PID_NAMESPACE_TEST
    struct p1_arena *pids;
#endif
    struct c2_namespace ns[C2_NAMESPACES];
    struct userns_credential creds[C2_TASKS];
};
#if CONFIG_PID_NAMESPACE_TEST
#include "pidns_types.inc"
#endif
_Static_assert(sizeof(struct c2_domain)<=24576,"C2 arena bound");
enum { C2_ARENA_PAGES=(sizeof(struct c2_domain)+PAGE_SIZE-1)/PAGE_SIZE,
       C2_EMBEDDED_BYTES=C2_TASKS*(sizeof(struct task)-9872)+C2_PROCESSES*(align(sizeof(struct task_context),16)-4560),
       C2_EMBEDDED_PAGES=(C2_EMBEDDED_BYTES+PAGE_SIZE-1)/PAGE_SIZE,
       C2_CHARGED_PAGES=C2_ARENA_PAGES+C2_EMBEDDED_PAGES };
/* Baseline receipts: C1 task9872/context4560; heap payloads round to16.
 * Account the new embedded fields too, rather than spending the whole cap
 * on an arena and leaving those bytes outside its declared ceiling. */
_Static_assert(sizeof(struct task)>=9872 && sizeof(struct task_context)>=4560,"C1 layout baseline");
_Static_assert(C2_EMBEDDED_PAGES<=68,"C2 embedded metadata bound");
_Static_assert(C2_CHARGED_PAGES<=C2_MAX_METADATA_PAGES,"C2 metadata ceiling");
static spinlock_t c2_lock;
static uint64_t next_object=2;
/* Immutable, globally nonreusable domain identity; inode tags never retain
   a pointer or reference to a dead domain. Exhaustion fails closed. */
static uint64_t next_file_origin=1;
#ifndef CONFIG_USERNS_VERIFY
#define CONFIG_USERNS_VERIFY 0
#endif
#if CONFIG_USERNS_VERIFY
/* Boot selftest only. No user-facing fault controls; absent from final build. */
static bool selftest_fail_allocation;
#endif
static struct task *current(void) { return sched_ready()?this_cpu()->current:NULL; }
bool userns_enabled(const struct task *t) { return CONFIG_USERNS_TEST && t && t->userns; }
bool userns_admitted_account(const struct task *t) {
    if(!userns_enabled(t) || !t->space.vm || !t->space.vm->budget)return false;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct c2_domain *d=t->userns->domain;
    bool admitted=d->budget_committed && d->budget==t->space.vm->budget;
    spin_unlock_irqrestore(&c2_lock,irq);return admitted;
}
bool userns_browser_network(const struct task *t) {
    if(!userns_enabled(t) || !t->context->browser_network ||
       credentials_uid(t)!=1000 || credentials_gid(t)!=1000 ||
       !t->space.vm || !t->space.vm->budget)return false;
    uint64_t irq=spin_lock_irqsave(&c2_lock);
    struct userns_credential *r=t->userns;
    bool allowed=!r->ns && r->domain->budget_committed &&
                 r->domain->budget==t->space.vm->budget;
    spin_unlock_irqrestore(&c2_lock,irq);return allowed;
}
uint64_t userns_file_origin(const struct task *t) {
    if(!CONFIG_RAMFS_SHARED_WRITE_TEST || !userns_enabled(t) ||
       credentials_uid(t)!=1000 || credentials_gid(t)!=1000 ||
       !t->space.vm || !t->space.vm->budget)return 0;
    uint64_t irq=spin_lock_irqsave(&c2_lock);
    const struct c2_domain *d=t->userns->domain;
    uint64_t token=d->budget_committed && d->budget==t->space.vm->budget?d->file_origin:0;
    spin_unlock_irqrestore(&c2_lock,irq);return token;
}
bool userns_newuser_clone(const struct task *t,uint64_t flags) {
    return userns_enabled(t) && flags==(C2_CLONE_NEWUSER|17);
}
static void domain_destroy(struct c2_domain *);
static unsigned p1_metadata_pages(const struct c2_domain *d) {
#if CONFIG_PID_NAMESPACE_TEST
    return d->pids?P1_CHARGED_PAGES:0;
#else
    (void)d;return 0;
#endif
}
static void p1_arena_destroy(struct c2_domain *);
#include "seccomp.inc"
static void ns_put_locked(struct c2_domain *d,struct c2_namespace *n) {
    while(n) {
        kassert(n->references);
        if(--n->references)return;
        struct c2_namespace *parent=n->parent;
        memset(n,0,sizeof(*n));kassert(d->namespaces);d->namespaces--;n=parent;
    }
}
static void domain_destroy(struct c2_domain *d) {
    kassert(!d->references && !d->tasks && !d->processes && !d->namespaces && !d->pending_budget);
    if(d->budget) {
        user_budget_uncharge(d->budget,c2_metadata_pages(d));user_budget_put(d->budget);
    }
    uintptr_t physical=d->physical;
    p1_arena_destroy(d);
    f1_arena_destroy(d);
    pr_info("C2_DOMAIN_RELEASE arena_pages=%u charged_metadata_pages=%u peak_namespaces=%u peak_processes=%u peak_tasks=%u no_live_refs=1\n",
        C2_ARENA_PAGES,C2_CHARGED_PAGES,d->peak_namespaces,d->peak_processes,d->peak_tasks);
    memset(d,0,C2_ARENA_PAGES*PAGE_SIZE);pmm_free_pages(physical,C2_ARENA_PAGES);
}

#if CONFIG_FILESYSTEM_SANDBOX_TEST
static void s1_group_free_locked(struct c2_domain *,struct p1_group *);
#endif
#include "pidns.inc"
#include "filesystem.inc"
static struct userns_credential *cred_allocate_locked(struct c2_domain *d,struct task *t,
                                                     const struct userns_credential *parent,bool thread) {
    if(d->tasks>=C2_TASKS || (!thread && d->processes>=C2_PROCESSES) ||
       next_object>UINT64_MAX/256 || !t->pid) {
        pr_warn("C2_CREDENTIAL_DENIED tasks=%u task_limit=%u processes=%u process_limit=%u thread=%u\n",d->tasks,C2_TASKS,d->processes,C2_PROCESSES,thread);
        return NULL;
    }
    struct userns_credential *r=NULL;
    for(unsigned i=0;i<C2_TASKS;i++)if(!d->creds[i].used){r=&d->creds[i];break;}
    if(!r)return NULL;
    if(parent)*r=*parent;else memset(r,0,sizeof(*r));
    if(parent)f1_get_locked(r->filter);
    if(thread)r->seccomp_frames=0;
#if CONFIG_PID_NAMESPACE_TEST
    r->pid_identity=NULL;
#endif
    r->domain=d;r->serial=next_object++;r->group=thread?parent->group:r->serial;
    r->pid=t->pid;r->tgid=t->tgid;r->used=true;r->live=false;
    if(!parent)r->bounding=C2_CAPS;
    if(r->ns)r->ns->references++;
    d->references++;d->tasks++;if(!thread)d->processes++;
    d->peak_tasks=max(d->peak_tasks,d->tasks);d->peak_processes=max(d->peak_processes,d->processes);
    return r;
}
static int new_namespace_locked(struct userns_credential *r) {
#if CONFIG_USERNS_VERIFY
    if(selftest_fail_allocation)return -E_NOMEM;
#endif
    struct c2_domain *d=r->domain;struct c2_namespace *parent=r->ns,*n=NULL;
    if(parent && (!parent->uid_mapped || !parent->gid_mapped))return -E_PERM;
    if((parent && parent->depth>=C2_DEPTH) || d->namespaces+p1_ns_count(d)>=C2_NAMESPACES || next_object>UINT64_MAX/256)return -E_NOSPC;
    for(unsigned i=0;i<C2_NAMESPACES;i++)if(!d->ns[i].references){n=&d->ns[i];break;}
    if(!n)return -E_NOSPC;
    *n=(struct c2_namespace){.id=next_object++,.references=1,
        .depth=parent?parent->depth+1:1,.parent=parent,
        .groups_denied=parent && parent->groups_denied};
    if(parent)parent->references++;
    d->namespaces++;
    d->peak_namespaces=max(d->peak_namespaces,d->namespaces);
    r->ns=n;r->effective=r->permitted=r->bounding=C2_CAPS;r->inheritable=0;
    ns_put_locked(d,parent);
    return 0;
}
int userns_enroll(struct task *t) {
    if(!CONFIG_USERNS_TEST)return 0;
    if(t->userns || t->pid==1 || t->browser_sandbox || t->context->web_role)return -E_PERM;
    struct user_budget *inherited=t->space.vm?t->space.vm->budget:NULL;
    if(inherited && !user_budget_charge(inherited,C2_CHARGED_PAGES))return -E_NOMEM;
    user_budget_get(inherited);
#if CONFIG_USERNS_VERIFY
    uintptr_t physical=selftest_fail_allocation?0:pmm_alloc_pages(C2_ARENA_PAGES);
#else
    uintptr_t physical=pmm_alloc_pages(C2_ARENA_PAGES);
#endif
    if(!physical) {
        user_budget_uncharge(inherited,C2_CHARGED_PAGES);user_budget_put(inherited);return -E_NOMEM;
    }
    struct c2_domain *d=phys_ptr(physical);memset(d,0,C2_ARENA_PAGES*PAGE_SIZE);d->physical=physical;
    d->budget=inherited;d->budget_committed=inherited!=NULL;
    uint64_t irq=spin_lock_irqsave(&c2_lock);
    struct userns_credential *r=NULL;
    if(!CONFIG_RAMFS_SHARED_WRITE_TEST || next_file_origin!=UINT64_MAX) {
        if(CONFIG_RAMFS_SHARED_WRITE_TEST)d->file_origin=next_file_origin++;
        r=cred_allocate_locked(d,t,NULL,false);
    }
    if(r){r->live=true;t->userns=r;}
    spin_unlock_irqrestore(&c2_lock,irq);
    if(!r){domain_destroy(d);return -E_NOSPC;}
    return 0;
}
int userns_inherit_fs(struct task *child,const struct task *parent,bool thread,bool new_ns,bool share_fs) {
    generic_seccomp_inherit(child,parent);
    landlock_task_inherit(child,parent);
    if(!userns_enabled(parent))return (new_ns||share_fs)?-E_PERM:0;
    uint64_t irq=spin_lock_irqsave(&c2_lock);
    struct userns_credential *r=cred_allocate_locked(parent->userns->domain,child,parent->userns,thread);
    int e=r?0:-E_NOSPC;
    if(r){child->userns=r;if(new_ns)e=new_namespace_locked(r);if(!e)e=p1_inherit_locked(child,parent);if(!e)e=s1_inherit_locked(child,parent,thread,share_fs);}
    spin_unlock_irqrestore(&c2_lock,irq);return e;
}
int userns_inherit(struct task *c,const struct task *p,bool thread,bool ns){return userns_inherit_fs(c,p,thread,ns,false);}
void userns_task_live(struct task *t,bool live) {
    if(!userns_enabled(t))return;
    uint64_t irq=spin_lock_irqsave(&c2_lock);t->userns->live=live;spin_unlock_irqrestore(&c2_lock,irq);
}
void userns_task_free(struct task *t) {
    if(!userns_enabled(t))return;
    uint64_t irq=spin_lock_irqsave(&c2_lock);
    struct userns_credential *r=t->userns;struct c2_domain *d=r->domain;
    uint64_t group=r->group;t->userns=NULL;
    p1_credential_free_locked(r);
    ns_put_locked(d,r->ns);f1_put_locked(d,r->filter);memset(r,0,sizeof(*r));
    f1_empty_locked(d);
    kassert(d->tasks && d->references);d->tasks--;d->references--;
    bool found=false;for(unsigned i=0;i<C2_TASKS;i++)if(d->creds[i].used && d->creds[i].group==group)found=true;
    if(!found){p1_group_free_locked(d,group);kassert(d->processes);d->processes--;}
    bool destroy=!d->references;
    spin_unlock_irqrestore(&c2_lock,irq);if(destroy)domain_destroy(d);
}
void userns_exec_commit(struct task *t) {
    if(!userns_enabled(t))return;
    uint64_t irq=spin_lock_irqsave(&c2_lock);
    /* The loader never applies set-ID bits/file capabilities. This is also
       the common spawn/exec authority boundary: exec drops namespace caps,
       keeps outer IDs/domain/descriptor restrictions, and NEVER clears NNP.
       Creating a child namespace outside exec remains correctly scoped C2
       authority, not a no_new_privs bypass. */
    t->userns->effective=t->userns->permitted=t->userns->inheritable=0;
    t->userns->seccomp_frames=0;
    spin_unlock_irqrestore(&c2_lock,irq);
}
static unsigned view(const struct task *t,unsigned id,bool group) {
    if(!userns_enabled(t))return id;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct c2_namespace *n=t->userns->ns;
    bool mapped=true;
    for(;n;n=n->parent)if(!(group?n->gid_mapped:n->uid_mapped)){mapped=false;break;}
    unsigned result=!t->userns->ns?id:(mapped && id==1000?1000:65534);
    spin_unlock_irqrestore(&c2_lock,irq);return result;
}
unsigned userns_uid_view(const struct task *t,unsigned id) { return view(t,id,false); }
unsigned userns_gid_view(const struct task *t,unsigned id) { return view(t,id,true); }
struct user_budget *userns_budget_existing(struct task *t) {
    if(!userns_enabled(t))return NULL;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct user_budget *b=t->userns->domain->budget;
    user_budget_get(b);spin_unlock_irqrestore(&c2_lock,irq);return b;
}
int userns_budget_begin(struct task *t,struct user_budget *budget,struct userns_budget_claim *claim) {
    *claim=(struct userns_budget_claim){0};
    if(!userns_enabled(t) || !budget)return 0;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct c2_domain *d=t->userns->domain;
    int e=0;
    if(d->budget && d->budget!=budget)e=-E_PERM;
    else if(!d->budget) {
        if(!user_budget_charge(budget,c2_metadata_pages(d)))e=-E_NOMEM;
        else {user_budget_get(budget);d->budget=budget;}
    }
    if(!e){d->pending_budget++;claim->domain=d;claim->active=true;}
    spin_unlock_irqrestore(&c2_lock,irq);return e;
}
void userns_budget_finish(struct userns_budget_claim *claim,bool success) {
    if(!claim->active)return;
    struct c2_domain *d=claim->domain;struct user_budget *release=NULL;unsigned pages=0;
    uint64_t irq=spin_lock_irqsave(&c2_lock);kassert(d->pending_budget);d->pending_budget--;
    d->budget_committed|=success;
    if(!d->pending_budget && !d->budget_committed){release=d->budget;pages=c2_metadata_pages(d);d->budget=NULL;}
    claim->active=false;spin_unlock_irqrestore(&c2_lock,irq);
    if(release){user_budget_uncharge(release,pages);user_budget_put(release);}
}
struct cap_header {uint32_t version;int32_t pid;};
struct cap_data {uint32_t effective,permitted,inheritable;};
static long caps(struct task *t,register_t *r) {
    struct cap_header h;
    if(!user_copy_in(&t->space,&h,r->rdi,sizeof(h)))return -E_FAULT;
    if(h.version!=C2_CAP_VERSION) {
        h.version=C2_CAP_VERSION;
        if(!user_copy_out(&t->space,r->rdi,&h,sizeof(h)))return -E_FAULT;
        return r->rax==SYS_capget && !r->rsi?0:-E_INVAL;
    }
    if(r->rax==SYS_capget && !r->rsi)return 0;
    if(h.pid && h.pid!=(int)pidns_view(t,t->pid))return -E_PERM;
    struct cap_data data[2];
    if(r->rax==SYS_capset && !user_copy_in(&t->space,data,r->rsi,sizeof(data)))return -E_FAULT;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct userns_credential *c=t->userns;
    long e=0;
    if(r->rax==SYS_capset) {
        uint64_t effective=data[0].effective|((uint64_t)data[1].effective<<32);
        uint64_t permitted=data[0].permitted|((uint64_t)data[1].permitted<<32);
        uint64_t inheritable=data[0].inheritable|((uint64_t)data[1].inheritable<<32);
        if((effective&~permitted) || (permitted&~c->permitted) || (inheritable&~c->inheritable))e=-E_PERM;
        else {c->effective=effective;c->permitted=permitted;c->inheritable=inheritable;}
    } else for(unsigned i=0;i<2;i++)data[i]=(struct cap_data){c->effective>>(i*32),c->permitted>>(i*32),c->inheritable>>(i*32)};
    spin_unlock_irqrestore(&c2_lock,irq);
    if(e || r->rax==SYS_capset)return e;
    return user_copy_out(&t->space,r->rsi,data,sizeof(data))?0:-E_FAULT;
}
long userns_call(struct task *t,register_t *r) {
    if(!CONFIG_USERNS_TEST)return -E_NOSYS;
    if(r->rax==SYS_prctl && (r->rdi==36 || r->rdi==37))return task_subreaper(t,r);
    if(!userns_enabled(t))return -E_PERM;
    if(r->rax==SYS_seccomp || (r->rax==SYS_prctl && (r->rdi==21 || r->rdi==22)))
        return seccomp_call(t,r);
    if(r->rax==SYS_capget || r->rax==SYS_capset)return caps(t,r);
    if(r->rax==SYS_setgroups) {
        if(r->rdi)return -E_PERM;
        uint64_t irq=spin_lock_irqsave(&c2_lock);struct userns_credential *c=t->userns;
        long e=!c->ns || c->ns->groups_denied || !(c->effective&(1ULL<<6))?-E_PERM:0;
        spin_unlock_irqrestore(&c2_lock,irq);return e;
    }
    if(r->rax==SYS_unshare) {
        if(r->rdi!=C2_CLONE_NEWUSER)return -E_NOTSUP;
        if(!userns_single_live(t))return -E_PERM;
        uint64_t irq=spin_lock_irqsave(&c2_lock);int e=new_namespace_locked(t->userns);
        spin_unlock_irqrestore(&c2_lock,irq);return e;
    }
    if(r->rax==SYS_prctl) {
        if(r->rdi==38 || r->rdi==39) {
            if(!CONFIG_NO_NEW_PRIVS_TEST)return -E_NOSYS;
            /* PR_SET/GET_NO_NEW_PRIVS: exact Linux arguments, self only.
               There is no target PID and no reset operation. */
            if(r->rsi!=(r->rdi==38?1u:0u) || r->rdx || r->r10 || r->r8)return -E_INVAL;
            uint64_t irq=spin_lock_irqsave(&c2_lock);
            struct userns_credential *c=t->userns;
            if(r->rdi==38)c->no_new_privs=true;
            long e=r->rdi==38?0:c->no_new_privs;
            spin_unlock_irqrestore(&c2_lock,irq);return e;
        }
        if(r->rdi!=23 && r->rdi!=24)return -E_NOSYS;
        /* Linux consumes only arg2 for these two operations. libc's variadic
           prctl wrapper need not zero the unused argument registers. */
        if(r->rsi>40)return -E_INVAL;
        uint64_t irq=spin_lock_irqsave(&c2_lock);struct userns_credential *c=t->userns;
        long e=(c->bounding>>r->rsi)&1;
        if(r->rdi==24) {
            e=!c->ns || !(c->effective&(1ULL<<8))?-E_PERM:0;
            if(!e)c->bounding&=~(1ULL<<r->rsi);
        }
        spin_unlock_irqrestore(&c2_lock,irq);return e;
    }
    return -E_NOSYS;
}

/* Only virtual proc map controls get this narrow C1 write-path exception. */
bool userns_proc_write_path(const char *path) {
    struct task *t=current();if(!userns_enabled(t))return false;
    char numeric[64];snprintf(numeric,sizeof(numeric),"/proc/%u/",pidns_view(t,t->tgid));
    const char *leaf=NULL;
    if(!strncmp(path,"/proc/self/",11))leaf=path+11;
    else if(!strncmp(path,numeric,strlen(numeric)))leaf=path+strlen(numeric);
    return leaf && (!strcmp(leaf,"uid_map") || !strcmp(leaf,"gid_map") || !strcmp(leaf,"setgroups"));
}

/* Proc object helpers are below. VFS owns file-description lifetime; each
 * open proc description retains its real arena/namespace, never a PID lookup. */
enum {P_ROOT=1,P_SELF,P_PROCESS,P_TASKS,P_THREAD,P_UID,P_GID,P_GROUPS,P_NS,P_NS_LINK,P_NS_OBJECT,P_PID_LINK,P_PID_OBJECT,P_STATUS,P_FDS,P_FDINFO,P_FDLINK,P_FDLEAF};
static const struct fs_ops proc_ops;
#include "pidns_proc.inc"
bool userns_proc_node(const struct vnode *n) {return CONFIG_USERNS_TEST && n && n->mount && n->mount->ops==&proc_ops;}
static struct c2_namespace *ns_find(struct c2_domain *d,uint64_t id) {
    if(id==1)return NULL;
    for(unsigned i=0;i<C2_NAMESPACES;i++)if(d->ns[i].references && d->ns[i].id==id)return &d->ns[i];
    return NULL;
}
static int proc_authorize(const struct vnode *node,struct userns_credential **out) {
    struct task *t=current();
    if(node->aux[1]==P_ROOT && !node->aux[0]){*out=NULL;return userns_enabled(t)?-E_ACCES:0;}
    if(!userns_enabled(t) || node->aux[0]!=(uintptr_t)t->userns->domain)return -E_ACCES;
    if(node->aux[2] && node->aux[2]!=t->userns->group &&
       !(node->aux[1]==P_FDINFO && s1_root_matches(node)))return -E_ACCES;
    *out=t->userns;return 0;
}
int userns_proc_file(const struct file *f) {
    if(!userns_proc_node(&f->node))return 0;
    struct userns_credential *r;return proc_authorize(&f->node,&r);
}
static void make_node(struct mount *m,struct c2_domain *d,unsigned kind,uint64_t group,uint64_t ns,struct vnode *n) {
    *n=(struct vnode){.mount=m,.id=((ns?ns:group)<<8)|kind,.nlink=1,.uid=d?1000:0,.gid=d?1000:0};
    n->aux[0]=(uintptr_t)d;n->aux[1]=kind;n->aux[2]=group;n->aux[3]=ns;
    if(kind==P_ROOT || kind==P_PROCESS || kind==P_TASKS || kind==P_THREAD || kind==P_NS || kind==P_FDS || kind==P_FDINFO){n->mode=S_IFDIR|0555;n->nlink=2;}
    else if(kind==P_SELF || kind==P_NS_LINK || kind==P_PID_LINK || kind==P_FDLINK)n->mode=S_IFLNK|0777;
    else n->mode=S_IFREG|((kind==P_UID || kind==P_GID || kind==P_GROUPS)?0600:0444);
    if((kind==P_NS_OBJECT || kind==P_PID_OBJECT) && ns==1)n->uid=n->gid=0;
}
static int proc_lookup(const struct vnode *,const char *,struct vnode *);
static int proc_stat(struct vnode *);
#include "filesystem_proc.inc"
static int proc_root(struct mount *m,struct vnode *n) {
    struct task *t=current();make_node(m,userns_enabled(t)?t->userns->domain:NULL,P_ROOT,0,0,n);return 0;
}
static unsigned live_threads_locked(struct userns_credential *r) {
    unsigned count=0;
    for(unsigned i=0;i<C2_TASKS;i++)if(r->domain->creds[i].used && r->domain->creds[i].live && r->domain->creds[i].group==r->group)count++;
    return count;
}
static int proc_stat(struct vnode *n) {
    struct userns_credential *r;int e=proc_authorize(n,&r);if(e)return e;
    if(r && s1_proc_kind(n->aux[1]) && n->aux[1]>=P_FDLINK) {
        struct file *target;int error=s1_target(n,&target);if(error)return error;
        if(n->aux[1]==P_FDLINK)n->size=strlen(target->path)+(target->unlinked?10:0);
    }
    if(r && n->aux[1]==P_ROOT)n->nlink=3;
    if(r && n->aux[1]==P_PROCESS)n->nlink=4;
    if(n->aux[1]==P_TASKS) {
        uint64_t irq=spin_lock_irqsave(&c2_lock);n->nlink=2+live_threads_locked(r);spin_unlock_irqrestore(&c2_lock,irq);
    }
    if(r && n->aux[1]==P_THREAD) {
        bool found=false;uint64_t irq=spin_lock_irqsave(&c2_lock);
        for(unsigned i=0;i<C2_TASKS;i++)if(r->domain->creds[i].used && r->domain->creds[i].live &&
            r->domain->creds[i].serial==n->aux[3] && r->domain->creds[i].group==r->group)found=true;
        spin_unlock_irqrestore(&c2_lock,irq);if(!found)return -E_NOENT;
    }
    if(r && (n->aux[1]==P_SELF || n->aux[1]==P_NS_LINK || n->aux[1]==P_PID_LINK)) {
        char text[64];
        n->size=n->aux[1]==P_SELF?snprintf(text,sizeof(text),"%u",pidns_view(current(),r->tgid)):
            snprintf(text,sizeof(text),n->aux[1]==P_PID_LINK?"pid:[%llu]":"user:[%llu]",(unsigned long long)n->aux[3]);
    }
    return 0;
}
static bool numeric(const char *p,unsigned *value) {
    if(!*p || (*p=='0' && p[1]))return false;
    unsigned n=0;
    while(*p){if(*p<'0' || *p>'9' || n>(UINT32_MAX-(unsigned)(*p-'0'))/10)return false;n=n*10+(*p++-'0');}
    *value=n;return true;
}
static int proc_lookup(const struct vnode *dir,const char *name,struct vnode *out) {
    struct userns_credential *r;int e=proc_authorize(dir,&r);if(e)return e;
    if(!strcmp(name,".")){*out=*dir;return proc_stat(out);}
    if(!r)return -E_NOENT;
    unsigned kind=0,pid=0;uint64_t group=dir->aux[2],ns=0;
    if(dir->aux[1]==P_ROOT) {
        if(!strcmp(name,"self"))kind=P_SELF;
        else if(numeric(name,&pid) && pid==pidns_view(current(),r->tgid)){kind=P_PROCESS;group=r->group;}
    } else if(dir->aux[1]==P_PROCESS) {
        if(s1_proc_available() && (!strcmp(name,"fd") || !strcmp(name,"fdinfo")))
            return s1_proc_node(dir->mount,!strcmp(name,"fd")?P_FDS:P_FDINFO,0,out);
        if(!strcmp(name,"task"))kind=P_TASKS;
        if(!strcmp(name,"status") && pidns_proc_available(current()))kind=P_STATUS;
        if(!strcmp(name,"uid_map"))kind=P_UID;
        if(!strcmp(name,"gid_map"))kind=P_GID;
        if(!strcmp(name,"setgroups"))kind=P_GROUPS;
        if(!strcmp(name,"ns"))kind=P_NS;
        if(kind==P_UID || kind==P_GID || kind==P_GROUPS)ns=r->ns?r->ns->id:1;
    } else if(dir->aux[1]==P_FDS || dir->aux[1]==P_FDINFO) {
        if(!s1_owner_alive(dir) || !numeric(name,&pid) || pid>=OS_FD_MAX || !current()->fds[pid])return -E_NOENT;
        return s1_proc_node(dir->mount,dir->aux[1]==P_FDS?P_FDLINK:P_FDLEAF,pid,out);
    } else if(dir->aux[1]==P_TASKS && numeric(name,&pid)) {
        uint64_t irq=spin_lock_irqsave(&c2_lock);
        for(unsigned i=0;i<C2_TASKS;i++) {
            struct userns_credential *thread=&r->domain->creds[i];
            if(thread->used && thread->live && thread->group==r->group && p1_view_outer_locked(current(),thread->pid)==pid){kind=P_THREAD;ns=thread->serial;break;}
        }
        spin_unlock_irqrestore(&c2_lock,irq);
    } else if(dir->aux[1]==P_NS && !strcmp(name,"user")){kind=P_NS_LINK;ns=r->ns?r->ns->id:1;}
    else if(dir->aux[1]==P_NS && (!strcmp(name,"pid") || !strcmp(name,"pid_for_children")) && pidns_proc_available(current())) {
        kind=P_PID_LINK;uint64_t irq=spin_lock_irqsave(&c2_lock);ns=p1_namespace_serial(r);spin_unlock_irqrestore(&c2_lock,irq);
    }
    if(!kind)return -E_NOENT;
    make_node(dir->mount,r->domain,kind,group,ns,out);return proc_stat(out);
}
static long proc_read(struct vnode *node,uint64_t offset,void *out,size_t size) {
    struct userns_credential *r;int e=proc_authorize(node,&r);if(e)return e;
    if(!r)return -E_NOENT;
    char text[512];int length=0;unsigned kind=node->aux[1];
    if(kind==P_FDLINK || kind==P_FDLEAF)return s1_proc_read(node,offset,out,size);
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct c2_namespace *n=ns_find(r->domain,node->aux[3]);
    if(kind==P_PID_OBJECT)e=-E_INVAL;
    else if(kind==P_PID_LINK)length=snprintf(text,sizeof(text),"pid:[%llu]",(unsigned long long)node->aux[3]);
    else if(kind==P_STATUS)length=p1_proc_status(current(),text,sizeof(text));
    else if(node->aux[3]>1 && !n)e=-E_NOENT;
    else if(kind==P_SELF)length=snprintf(text,sizeof(text),"%u",p1_view_outer_locked(current(),r->tgid));
    else if(kind==P_NS_LINK)length=snprintf(text,sizeof(text),"user:[%llu]",(unsigned long long)node->aux[3]);
    else if(kind==P_UID || kind==P_GID) {
        if(!n)length=snprintf(text,sizeof(text),"0 0 4294967295\n");
        else if(kind==P_UID?n->uid_mapped:n->gid_mapped)length=snprintf(text,sizeof(text),"1000 1000 1\n");
    } else if(kind==P_GROUPS)length=snprintf(text,sizeof(text),"%s\n",n && n->groups_denied?"deny":"allow");
    else e=-E_INVAL;
    spin_unlock_irqrestore(&c2_lock,irq);
    if(e)return e;
    if(offset>=(unsigned)length)return 0;
    size=min(size,(size_t)length-offset);memcpy(out,text+offset,size);return size;
}
static long proc_write(struct vnode *node,uint64_t offset,const void *data,size_t size) {
    struct userns_credential *r;int e=proc_authorize(node,&r);if(e)return e;
    if(s1_proc_kind(node->aux[1]))return -E_ROFS;
    if(!r || !r->ns || node->aux[3]!=r->ns->id)return -E_PERM;
    if(offset || !size || size>64)return -E_INVAL;
    char text[65];memcpy(text,data,size);text[size]=0;
    unsigned kind=node->aux[1];
    bool deny=(size==4 && !memcmp(text,"deny",4)) || (size==5 && !memcmp(text,"deny\n",5));
    bool mapping=(size==11 && !memcmp(text,"1000 1000 1",11)) || (size==12 && !memcmp(text,"1000 1000 1\n",12));
    if((kind==P_GROUPS && !deny) || ((kind==P_UID || kind==P_GID) && !mapping))return -E_PERM;
    uint64_t irq=spin_lock_irqsave(&c2_lock);struct c2_namespace *n=r->ns;
    if(kind==P_GROUPS) {
        if(n->gid_mapped)e=-E_PERM;else n->groups_denied=true;
    } else if(kind==P_UID || kind==P_GID) {
        bool *mapped=kind==P_UID?&n->uid_mapped:&n->gid_mapped;
        if(*mapped || (kind==P_GID && !n->groups_denied) ||
           (n->parent && !(kind==P_UID?n->parent->uid_mapped:n->parent->gid_mapped)))e=-E_PERM;
        else *mapped=true;
    } else e=-E_PERM;
    spin_unlock_irqrestore(&c2_lock,irq);return e?e:(long)size;
}
static int proc_readdir(const struct vnode *node,uint64_t *offset,struct directory_entry *out) {
    struct userns_credential *r;int e=proc_authorize(node,&r);if(e)return e;
    char name[24]={0};uint64_t index=*offset;unsigned kind=node->aux[1];
    if(index<2)strcpy(name,index?"..":".");
    else if(r && (kind==P_FDS || kind==P_FDINFO) && s1_owner_alive(node)) {
        unsigned seen=2;for(unsigned fd=0;fd<OS_FD_MAX;fd++)
            if(current()->fds[fd] && seen++==index){snprintf(name,sizeof(name),"%u",fd);break;}
    }
    else if(r && kind==P_PROCESS && (index==8 || index==9) && s1_proc_available())strcpy(name,index==8?"fd":"fdinfo");
    else if(r && kind==P_ROOT && index==2)strcpy(name,"self");
    else if(r && kind==P_ROOT && index==3)snprintf(name,sizeof(name),"%u",pidns_view(current(),r->tgid));
    else if(r && kind==P_PROCESS && index<7) {
        static const char *entries[]={"task","uid_map","gid_map","setgroups","ns"};strcpy(name,entries[index-2]);
    } else if(r && kind==P_PROCESS && index==7 && pidns_proc_available(current()))strcpy(name,"status");
    else if(r && kind==P_NS && index==2)strcpy(name,"user");
    else if(r && kind==P_NS && (index==3 || index==4) && pidns_proc_available(current()))strcpy(name,index==3?"pid":"pid_for_children");
    else if(r && kind==P_TASKS) {
        unsigned seen=2;uint64_t irq=spin_lock_irqsave(&c2_lock);
        for(unsigned i=0;i<C2_TASKS;i++) {
            struct userns_credential *thread=&r->domain->creds[i];
            if(thread->used && thread->live && thread->group==r->group && seen++==index){snprintf(name,sizeof(name),"%u",p1_view_outer_locked(current(),thread->pid));break;}
        }
        spin_unlock_irqrestore(&c2_lock,irq);
    }
    if(!name[0])return 0;
    unsigned mode=S_IFDIR;
    if(index>=2) {
        if((kind==P_ROOT && index==2) || kind==P_NS)mode=S_IFLNK;
        if(kind==P_PROCESS && index>=3 && index<=5)mode=S_IFREG;
    }
    uint64_t inode=node->id;
    if(index==1) {
        inode=kind==P_TASKS || kind==P_NS?(node->aux[2]<<8)|P_PROCESS:
              kind==P_THREAD?(node->aux[2]<<8)|P_TASKS:1;
    } else if(index>=2) {
        struct vnode child;e=proc_lookup(node,name,&child);if(e)return e;
        inode=child.id;mode=child.mode&0170000;
    }
    *out=(struct directory_entry){.id=inode,.mode=mode};strcpy(out->name,name);(*offset)++;return 1;
}
static void proc_retain(struct vnode *node) {
    struct c2_domain *d=(void *)node->aux[0];if(!d)return;
    uint64_t irq=spin_lock_irqsave(&c2_lock);d->references++;
    unsigned kind=node->aux[1];
    if(kind==P_PID_LINK || kind==P_PID_OBJECT)p1_proc_retain(d,node->aux[3]);
#if CONFIG_FILESYSTEM_SANDBOX_TEST
    if(s1_proc_kind(kind)) {
        struct p1_identity *id=p1_find(d->pids,node->aux[3]);kassert(id);p1_get(d->pids,id);node->cache[0]=(uintptr_t)id;
    }
#endif
    if(kind==P_UID || kind==P_GID || kind==P_GROUPS || kind==P_NS_LINK || kind==P_NS_OBJECT) {
        struct c2_namespace *n=ns_find(d,node->aux[3]);
        if(node->aux[3]>1){kassert(n);n->references++;}
    }
    spin_unlock_irqrestore(&c2_lock,irq);
}
static void proc_release(struct vnode *node) {
    struct c2_domain *d=(void *)node->aux[0];if(!d)return;
    uint64_t irq=spin_lock_irqsave(&c2_lock);
    unsigned kind=node->aux[1];
    if(kind==P_PID_LINK || kind==P_PID_OBJECT)p1_proc_release(d,node->aux[3]);
#if CONFIG_FILESYSTEM_SANDBOX_TEST
    if(s1_proc_kind(kind) && node->cache[0])p1_put(d->pids,(void *)(uintptr_t)node->cache[0]);
#endif
    if(kind==P_UID || kind==P_GID || kind==P_GROUPS || kind==P_NS_LINK || kind==P_NS_OBJECT) {
        struct c2_namespace *n=ns_find(d,node->aux[3]);
        if(node->aux[3]>1){kassert(n);ns_put_locked(d,n);}
    }
    kassert(d->references);bool destroy=!--d->references;
    spin_unlock_irqrestore(&c2_lock,irq);if(destroy)domain_destroy(d);
}
static int proc_follow(const struct vnode *node,struct vnode *out) {
    if(s1_fd_magic(node))return s1_fd_stat(node,out);
    if(node->aux[1]!=P_NS_LINK && node->aux[1]!=P_PID_LINK)return -E_NOTSUP;
    struct userns_credential *r;int e=proc_authorize(node,&r);if(e)return e;
    make_node(node->mount,r->domain,node->aux[1]==P_PID_LINK?P_PID_OBJECT:P_NS_OBJECT,node->aux[2],node->aux[3],out);return 0;
}
static const struct fs_ops proc_ops={.root=proc_root,.lookup=proc_lookup,.read=proc_read,.write=proc_write,
    .readdir=proc_readdir,.stat=proc_stat,.retain=proc_retain,.release=proc_release,.follow=proc_follow};
#if CONFIG_USERNS_VERIFY
static void accounting_selftest(void) {
    /* Actual accounting/object functions with synthetic kernel test records;
       not fabricated credentials presented to an upstream process. */
    struct task *t=kmallocz(sizeof(*t));struct task_context *context=kmallocz(sizeof(*context));
    kassert(t && context);t->pid=t->tgid=77;t->user=true;t->context=context;
    struct user_budget *failed=user_budget_create(C2_CHARGED_PAGES+1);kassert(failed);
    struct user_vm vm={.budget=failed};t->space.vm=&vm;
    selftest_fail_allocation=true;
    kassert(userns_enroll(t)==-E_NOMEM && !t->userns && !user_budget_pages(failed));
    kassert(user_budget_peak(failed)==C2_CHARGED_PAGES);user_budget_put(failed);
    failed=user_budget_create(C2_CHARGED_PAGES-1);kassert(failed);vm.budget=failed;
    selftest_fail_allocation=false;
    kassert(userns_enroll(t)==-E_NOMEM && !t->userns && !user_budget_pages(failed));
    user_budget_put(failed);t->space.vm=NULL;
    selftest_fail_allocation=false;kassert(!userns_enroll(t));
    struct c2_domain *d=t->userns->domain;
    uint64_t irq=spin_lock_irqsave(&c2_lock);selftest_fail_allocation=true;
    kassert(new_namespace_locked(t->userns)==-E_NOMEM && !t->userns->ns && !d->namespaces);
    selftest_fail_allocation=false;spin_unlock_irqrestore(&c2_lock,irq);
    struct user_budget *small=user_budget_create(C2_CHARGED_PAGES-1);
    struct userns_budget_claim a={0},b={0};kassert(small);
    kassert(userns_budget_begin(t,small,&a)==-E_NOMEM && !a.active && !d->budget && !user_budget_pages(small));
    user_budget_put(small);
    struct user_budget *budget=user_budget_create(C2_CHARGED_PAGES+1);kassert(budget);
    kassert(!userns_budget_begin(t,budget,&a) && !userns_budget_begin(t,budget,&b));
    kassert(user_budget_pages(budget)==C2_CHARGED_PAGES && d->pending_budget==2);
    userns_budget_finish(&a,false);kassert(user_budget_pages(budget)==C2_CHARGED_PAGES && d->pending_budget==1);
    userns_budget_finish(&b,false);kassert(!user_budget_pages(budget) && !d->budget && !d->pending_budget);
    kassert(!userns_budget_begin(t,budget,&a));userns_budget_finish(&a,true);
    kassert(d->budget_committed && user_budget_pages(budget)==C2_CHARGED_PAGES);
    userns_task_free(t);kassert(!t->userns && !user_budget_pages(budget) && user_budget_peak(budget)==C2_CHARGED_PAGES);
    user_budget_put(budget);kfree(t);kfree(context);
    pr_info("C2_SELFTEST_PASS injected_allocation_failure_atomic shared_metadata_charge rollback commit_and_cleanup\n");
}
#endif
#include "pidns_selftest.inc"
bool userns_proc_mount(struct mount *m) {
    if(!CONFIG_USERNS_TEST)return false;
#if CONFIG_USERNS_VERIFY
    accounting_selftest();
    f1_accounting_selftest();
#if CONFIG_PRIVATE_FORK_VERIFY
    private_fork_barrier_selftest();
    private_fork_scope_selftest();
#endif
#if CONFIG_PID_NAMESPACE_VERIFY
    p1_selftest();
#if CONFIG_FILESYSTEM_SANDBOX_VERIFY
    s1_verifier();
#endif
#endif
#endif
    strcpy(m->path,"/proc");strcpy(m->type,"c2proc");m->ops=&proc_ops;return true;
}
