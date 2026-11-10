#include <landlock.h>
#include <credentials.h>
#include <sched.h>
#include <fs/vfs.h>
#include <string.h>
#include <panic.h>
#include <mm/pmm.h>

#define LL_MAX_RULES 256
struct landlock_rule {struct vnode anchor;uint64_t access;};
struct landlock_ruleset {
    uint64_t handled;
    unsigned count,capacity;
    struct landlock_rule *rules;
};
struct landlock_domain {
    struct landlock_domain *parent;
    unsigned references,count,depth;
    uint64_t handled;
    struct landlock_rule rules[];
};
static struct task *landlock_caller(void) {
    return sched_ready()?this_cpu()->current:NULL;
}
bool landlock_active(void) {
    struct task *task=landlock_caller();
    return task && task->user && task->landlock_domain;
}
static bool same_node(const struct vnode *a,const struct vnode *b) {
    return a->mount==b->mount && a->id==b->id && a->aux[0]==b->aux[0] &&
           a->aux[2]==b->aux[2] && a->aux[3]==b->aux[3];
}
/* All anchor reference operations run under vfs_mutex. Mount references
 * prevent a security policy from retaining a recycled mount object. */
static void anchor_get(struct vnode *node) {
    kassert(node->mount && node->mount->security_references<UINT32_MAX);
    node->mount->security_references++;
    if(node->mount->ops->retain)node->mount->ops->retain(node);
}
static void anchor_put(struct vnode *node) {
    kassert(node->mount && node->mount->security_references);
    node->mount->security_references--;
    if(node->mount->ops->release)node->mount->ops->release(node);
}
void landlock_ruleset_release(struct landlock_ruleset *set) {
    if(!set)return;
    for(unsigned i=0;i<set->count;i++)anchor_put(&set->rules[i].anchor);
    kfree(set->rules);kfree(set);
}
static void domain_get(struct landlock_domain *domain) {
    if(domain)__atomic_fetch_add(&domain->references,1,__ATOMIC_RELAXED);
}
static void domain_put(struct landlock_domain *domain) {
    while(domain && __atomic_fetch_sub(&domain->references,1,__ATOMIC_ACQ_REL)==1) {
        struct landlock_domain *parent=domain->parent;
        for(unsigned i=0;i<domain->count;i++)anchor_put(&domain->rules[i].anchor);
        kfree(domain);domain=parent;
    }
}
void landlock_task_inherit(struct task *child,const struct task *parent) {
    if(!parent || !parent->user)return;
    child->landlock_domain=parent->landlock_domain;domain_get(child->landlock_domain);
}
void landlock_task_release(struct task *task) {
    struct landlock_domain *domain=task->landlock_domain;
    task->landlock_domain=NULL;
    if(domain){mutex_lock(&vfs_mutex);domain_put(domain);mutex_unlock(&vfs_mutex);}
}
void landlock_walk_begin(struct landlock_walk *walk) {
    memset(walk,0,sizeof(*walk));
    struct task *task=landlock_caller();
    for(const struct landlock_domain *domain=task?task->landlock_domain:NULL;
        domain && walk->count<LL_MAX_LAYERS;domain=domain->parent)
        walk->layers[walk->count++]=domain;
}
void landlock_walk_visit(struct landlock_walk *walk,const struct vnode *node) {
    for(unsigned i=0;i<walk->count;i++) {
        const struct landlock_domain *layer=walk->layers[i];
        for(unsigned j=0;j<layer->count;j++)
            if(same_node(node,&layer->rules[j].anchor))walk->allowed[i]|=layer->rules[j].access;
    }
}
int landlock_walk_check(const struct landlock_walk *walk,uint64_t access) {
    for(unsigned i=0;i<walk->count;i++)
        if(access&walk->layers[i]->handled&~walk->allowed[i])return -E_ACCES;
    return 0;
}
int landlock_path_access(const char *path,uint64_t access,bool nofollow) {
    if(!landlock_active())return 0;
    struct landlock_walk walk;struct vnode node;
    int error=vfs_landlock_walk(path,nofollow,&walk,&node);
    return error?error:landlock_walk_check(&walk,access);
}
static int parent_name(const char *path,char out[OS_PATH_MAX]) {
    int error=vfs_normalize("/",path,out);if(error)return error;
    char *slash=strrchr(out,'/');
    if(!slash)return -E_INVAL;
    if(slash==out)out[1]=0;else *slash=0;
    return 0;
}
int landlock_parent_access(const char *path,uint64_t access) {
    if(!landlock_active())return 0;
    char parent[OS_PATH_MAX];int error=parent_name(path,parent);
    return error?error:landlock_path_access(parent,access,false);
}
uint64_t landlock_make_access(const struct vnode *node) {
    switch(node->mode&0170000) {
    case S_IFDIR:return LL_MAKE_DIR;
    case S_IFLNK:return LL_MAKE_SYM;
    case 0010000:return LL_MAKE_FIFO;
    case 0020000:return LL_MAKE_CHAR;
    case 0060000:return LL_MAKE_BLOCK;
    case 0140000:return LL_MAKE_SOCK;
    default:return LL_MAKE_REG;
    }
}
int landlock_reparent(const char *source,const char *target,const struct vnode *object) {
    if(!landlock_active())return 0;
    char oldname[OS_PATH_MAX],newname[OS_PATH_MAX];
    int error=parent_name(source,oldname);if(!error)error=parent_name(target,newname);if(error)return error;
    struct landlock_walk old,new;struct vnode a,b;
    error=vfs_landlock_walk(oldname,false,&old,&a);
    if(!error)error=vfs_landlock_walk(newname,false,&new,&b);
    if(error)return error;
    if(same_node(&a,&b))return 0;
    landlock_walk_visit(&old,object);landlock_walk_visit(&new,object);
    for(unsigned i=0;i<old.count;i++) {
        uint64_t handled=old.layers[i]->handled;
        if(!(handled&LL_REFER) || !(old.allowed[i]&LL_REFER) || !(new.allowed[i]&LL_REFER))return -E_XDEV;
        if(new.allowed[i]&handled&~old.allowed[i])return -E_XDEV;
    }
    return 0;
}
static struct landlock_ruleset *ruleset_fd(struct task *task,unsigned fd,long *error) {
    if(fd>=OS_FD_MAX || !task->fds[fd]){*error=-E_BADF;return NULL;}
    if(!task->fds[fd]->landlock_ruleset){*error=-E_BADFD;return NULL;}
    return task->fds[fd]->landlock_ruleset;
}
static long create_ruleset(struct task *task,register_t *r) {
    if(r->rdx) {
        if(r->rdx!=1 || r->rdi || r->rsi)return -E_INVAL;
        return 3; /* Exactly the filesystem rights implemented through ABI v3. */
    }
    if(r->rsi<8)return -E_INVAL;
    if(r->rsi>PAGE_SIZE)return -E_2BIG;
    uint8_t *attributes=kmalloc(r->rsi);if(!attributes)return -E_NOMEM;
    long error=0;uint64_t handled=0;
    if(!user_copy_in(&task->space,attributes,r->rdi,r->rsi))error=-E_FAULT;
    if(!error) {
        memcpy(&handled,attributes,8);
        for(unsigned i=8;i<r->rsi;i++)if(attributes[i]){error=-E_2BIG;break;}
        if(!error && (handled&~LL_FS_ALL))error=-E_INVAL;
        if(!error && !handled)error=-E_NOMSG;
    }
    kfree(attributes);if(error)return error;
    struct landlock_ruleset *set=kmallocz(sizeof(*set));if(!set)return -E_NOMEM;
    set->handled=handled;
    mutex_lock(&vfs_mutex);unsigned fd=0;
    while(fd<OS_FD_MAX && task->fds[fd])fd++;
    if(fd==OS_FD_MAX)error=-E_MFILE;
    else {
        struct file *file=NULL;
        error=vfs_memfd(0,0,&file);
        if(!error) {
            file->landlock_ruleset=set;strcpy(file->path,"landlock-ruleset:");
            task->fds[fd]=file;task->fd_flags[fd]=FD_CLOEXEC;
        }
    }
    mutex_unlock(&vfs_mutex);
    if(error){kfree(set);return error;}
    return fd;
}
static long add_rule(struct task *task,register_t *r) {
    if(r->r10 || r->rsi!=1)return -E_INVAL;
    struct {uint64_t access;int32_t parent_fd;} __attribute__((packed)) attributes;
    if(!user_copy_in(&task->space,&attributes,r->rdx,sizeof(attributes)))return -E_FAULT;
    mutex_lock(&vfs_mutex);long error=0;
    struct landlock_ruleset *set=ruleset_fd(task,(unsigned)r->rdi,&error);
    struct file *file=NULL;
    if(!error && (attributes.parent_fd<0 || (unsigned)attributes.parent_fd>=OS_FD_MAX ||
                  !(file=task->fds[attributes.parent_fd])))error=-E_BADF;
    if(!error && (!file->node.mount || file->landlock_ruleset ||
                 (file->node.mode&0170000)==S_IFLNK))error=-E_BADFD;
    if(!error && !attributes.access)error=-E_NOMSG;
    if(!error && (attributes.access&~set->handled))error=-E_INVAL;
    if(!error && (file->node.mode&0170000)!=S_IFDIR &&
       (attributes.access&~(LL_EXECUTE|LL_WRITE_FILE|LL_READ_FILE|LL_TRUNCATE)))error=-E_INVAL;
    if(!error) {
        unsigned index=0;
        for(;index<set->count;index++)if(same_node(&set->rules[index].anchor,&file->node))break;
        if(index<set->count)set->rules[index].access|=attributes.access;
        else if(set->count==LL_MAX_RULES)error=-E_NOSPC;
        else {
            if(set->count==set->capacity) {
                unsigned capacity=set->capacity?set->capacity*2:4;
                struct landlock_rule *rules=kmalloc(capacity*sizeof(*rules));
                if(!rules)error=-E_NOMEM;
                else {memcpy(rules,set->rules,set->count*sizeof(*rules));kfree(set->rules);set->rules=rules;set->capacity=capacity;}
            }
            if(!error) {
                struct landlock_rule *rule=&set->rules[set->count++];
                rule->anchor=file->node;rule->access=attributes.access;anchor_get(&rule->anchor);
            }
        }
    }
    mutex_unlock(&vfs_mutex);return error;
}
static long restrict_self(struct task *task,register_t *r) {
    if(r->rsi)return -E_INVAL;
    bool nnp=task->context->account_nnp;
    if(task->userns) {
        register_t query={.rax=SYS_prctl,.rdi=39};nnp=userns_call(task,&query)==1;
    }
    if(credentials_uid(task) && !nnp)return -E_PERM;
    mutex_lock(&vfs_mutex);long error=0;
    struct landlock_ruleset *set=ruleset_fd(task,(unsigned)r->rdi,&error);
    struct landlock_domain *parent=task->landlock_domain;
    if(!error && parent && parent->depth==LL_MAX_LAYERS)error=-E_2BIG;
    struct landlock_domain *domain=NULL;
    if(!error) {
        domain=kmallocz(sizeof(*domain)+set->count*sizeof(struct landlock_rule));
        if(!domain)error=-E_NOMEM;
    }
    if(!error) {
        domain->references=1;domain->parent=parent;domain->depth=parent?parent->depth+1:1;
        domain->handled=set->handled;domain->count=set->count;
        for(unsigned i=0;i<set->count;i++){domain->rules[i]=set->rules[i];anchor_get(&domain->rules[i].anchor);}
        /* Transfer this task's parent reference into the immutable new layer. */
        task->landlock_domain=domain;
    }
    mutex_unlock(&vfs_mutex);return error;
}
long landlock_call(struct task *task,register_t *r) {
    switch(r->rax) {
    case SYS_landlock_create_ruleset:return create_ruleset(task,r);
    case SYS_landlock_add_rule:return add_rule(task,r);
    case SYS_landlock_restrict_self:return restrict_self(task,r);
    default:return -E_NOSYS;
    }
}
