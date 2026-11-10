#include <credentials.h>
#include <accounts.h>
#include <string.h>
#include <userns.h>
unsigned credentials_ruid(const struct task *t) {return t&&t->user&&t->context?(t->userns?credentials_uid(t):t->context->ruid):0;}
unsigned credentials_rgid(const struct task *t) {return t&&t->user&&t->context?(t->userns?credentials_gid(t):t->context->rgid):0;}
void credentials_all_ids(struct task *t,unsigned uid,unsigned gid) {
    t->context->ruid=t->context->suid=t->context->fsuid=uid;
    t->context->rgid=t->context->sgid=t->context->fsgid=gid;
    __atomic_store_n(&t->context->gid,gid,__ATOMIC_RELEASE);
    __atomic_store_n(&t->context->uid,uid,__ATOMIC_RELEASE);
}
void credentials_inherit(struct task_context *c,const struct task *parent) {
    if(!parent || !parent->user || !parent->context)return;
    const struct task_context *p=parent->context;
    c->uid=p->uid;c->gid=p->gid;c->ruid=p->ruid;c->suid=p->suid;c->fsuid=p->fsuid;
    c->rgid=p->rgid;c->sgid=p->sgid;c->fsgid=p->fsgid;c->group_count=p->group_count;
    c->browser_network=p->browser_network;
    memcpy(c->groups,p->groups,sizeof(c->groups));
}
bool credentials_in_group(const struct task *t,unsigned gid) {
    if(gid==credentials_fsgid(t))return true;
    if(t && t->context && !t->context->desktop_fsuid)
        for(unsigned i=0;i<t->context->group_count;i++)if(t->context->groups[i]==gid)return true;
    return false;
}
long credentials_posix_ids(struct task *t,register_t *r) {
    struct task_context *c=t->context;bool root=!credentials_uid(t);
    bool group=r->rax==SYS_setgid || r->rax==SYS_setregid || r->rax==SYS_getresgid || r->rax==SYS_setresgid || r->rax==SYS_setfsgid;
    unsigned real=group?c->rgid:c->ruid,effective=group?c->gid:c->uid,saved=group?c->sgid:c->suid;
    if(r->rax==SYS_getgroups) {
        unsigned size=(unsigned)r->rdi;if((int)size<0)return -E_INVAL;
        if(!size)return c->group_count;
        if(size<c->group_count)return -E_INVAL;
        return user_copy_out(&t->space,r->rsi,c->groups,c->group_count*4)?(long)c->group_count:-E_FAULT;
    }
    if(r->rax==SYS_setgroups) {
        if(!root)return -E_PERM;
        if(r->rdi>32)return -E_INVAL;
        unsigned groups[32];if(r->rdi && !user_copy_in(&t->space,groups,r->rsi,r->rdi*4))return -E_FAULT;
        for(unsigned i=0;i<r->rdi;i++)if(groups[i]==UINT32_MAX)return -E_INVAL;
        for(unsigned i=1;i<r->rdi;i++) {unsigned v=groups[i],j=i;while(j && groups[j-1]>v){groups[j]=groups[j-1];j--;}groups[j]=v;}
        memcpy(c->groups,groups,r->rdi*4);c->group_count=r->rdi;return 0;
    }
    if(r->rax==SYS_getresuid || r->rax==SYS_getresgid) {
        if(!user_access(&t->space,r->rdi,4,true) || !user_access(&t->space,r->rsi,4,true) || !user_access(&t->space,r->rdx,4,true))return -E_FAULT;
        return user_copy_out(&t->space,r->rdi,&real,4) && user_copy_out(&t->space,r->rsi,&effective,4) && user_copy_out(&t->space,r->rdx,&saved,4)?0:-E_FAULT;
    }
    unsigned a=r->rdi,b=r->rsi,d=r->rdx;
    if(r->rax==SYS_setfsuid || r->rax==SYS_setfsgid) {
        unsigned old=group?c->fsgid:c->fsuid;
        if(a!=UINT32_MAX && (root || a==real || a==effective || a==saved || a==old)) {
            if(group)c->fsgid=a;else c->fsuid=a;
        }
        return old;
    }
    if(r->rax==SYS_setuid || r->rax==SYS_setgid) {
        if(a==UINT32_MAX)return -E_INVAL;
        if(root)real=effective=saved=a;
        else if(a==real || a==saved)effective=a;
        else return -E_PERM;
    } else if(r->rax==SYS_setreuid || r->rax==SYS_setregid) {
        if(!root && ((a!=UINT32_MAX && a!=real && a!=effective) || (b!=UINT32_MAX && b!=real && b!=effective && b!=saved)))return -E_PERM;
        if(a!=UINT32_MAX || (b!=UINT32_MAX && b!=real))saved=b==UINT32_MAX?effective:b;
        if(a!=UINT32_MAX)real=a;
        if(b!=UINT32_MAX)effective=b;
    } else {
        unsigned values[3]={a,b,d};
        for(unsigned i=0;i<3;i++)if(values[i]!=UINT32_MAX && !root && values[i]!=real && values[i]!=effective && values[i]!=saved)return -E_PERM;
        if(a!=UINT32_MAX)real=a;
        if(b!=UINT32_MAX)effective=b;
        if(d!=UINT32_MAX)saved=d;
    }
    if(group) {c->rgid=real;c->sgid=saved;c->fsgid=effective;__atomic_store_n(&c->gid,effective,__ATOMIC_RELEASE);}
    else {
        if(real!=c->ruid && real) {c->account_uid=real>=1000 && real<1032?real:0;c->account_session=t->tgid;}
        c->ruid=real;c->suid=saved;c->fsuid=effective;__atomic_store_n(&c->uid,effective,__ATOMIC_RELEASE);}
    return 0;
}
void credentials_exec_prepare(const struct task *t,const struct vnode *node,struct exec_credentials *out) {
    *out=(struct exec_credentials){.ruid=credentials_ruid(t),.uid=credentials_uid(t),.rgid=credentials_rgid(t),.gid=credentials_gid(t)};
    struct vnode fresh=*node; /* ELF loader holds vfs_mutex. */
    int error=fresh.mount->ops->stat?fresh.mount->ops->stat(&fresh):0;
    if(!error && t && !t->userns && !t->browser_sandbox && !t->context->web_role && !t->context->account_nnp) {
        if(fresh.mode&04000)out->uid=fresh.uid;
        if(fresh.mode&02000)out->gid=fresh.gid;
    }
    out->secure=out->ruid!=out->uid || out->rgid!=out->gid;
}
void credentials_exec_commit(struct task *t,const struct exec_credentials *ids) {
    t->context->ruid=ids->ruid;t->context->rgid=ids->rgid;
    t->context->suid=t->context->fsuid=ids->uid;t->context->sgid=t->context->fsgid=ids->gid;
    __atomic_store_n(&t->context->uid,ids->uid,__ATOMIC_RELEASE);
    __atomic_store_n(&t->context->gid,ids->gid,__ATOMIC_RELEASE);
}

unsigned credentials_mode(const struct vnode *node,unsigned mode) {
    mode&=07777;struct task *t=sched_ready()?this_cpu()->current:NULL;
    if(credentials_fsuid(t) && !credentials_in_group(t,node->gid))mode&=~02000u;
    return mode;
}
int credentials_modified(struct vnode *node) {
    struct task *t=sched_ready()?this_cpu()->current:NULL;
    if(!credentials_fsuid(t) || !(node->mode&06000))return 0;
    return node->mount->ops->chmod?node->mount->ops->chmod(node,node->mode&~06000u):-E_NOTSUP;
}

int credentials_chown(struct vnode *node,unsigned uid,unsigned gid) {
    struct task *t=sched_ready()?this_cpu()->current:NULL;
    if(!credentials_fsuid(t))return 0;
    if(node->mount->ops->stat) {int e=node->mount->ops->stat(node);if(e)return e;}
    if(node->uid!=credentials_fsuid(t) || (uid!=UINT32_MAX && uid!=node->uid) || (gid!=UINT32_MAX && !credentials_in_group(t,gid)))return -E_PERM;
    return 0;
}
