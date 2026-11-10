/* Inode-based Linux notifications. VFS serializes registration, mutation and
   queue ownership; poll only loads the published byte count. No global path
   authority is granted: add_watch performs normal path/sandbox/read checks. */
#include <fs/inotify.h>
#include <filesystem.h>
#include <credentials.h>
#include <sched.h>
#include <sandbox.h>
#include <mm/pmm.h>
#include <mm/user.h>
#include <string.h>
enum { INSTANCE_LIMIT=16,WATCH_LIMIT=1024,QUEUE_BYTES=16384,HASH_SIZE=256 };
struct watch {
    struct inode_watch_queue *owner;
    struct mount *mount;
    uint64_t inode;
    struct watch *next;
    uint32_t mask,wd;
};
struct inode_watch_queue {
    struct user_budget *budget;
    bool active,overflow;
    uint32_t bytes,last,next_wd;
    struct watch watches[WATCH_LIMIT];
    uint8_t queue[QUEUE_BYTES];
};
enum { CHARGE_PAGES=(sizeof(struct inode_watch_queue)+4095)/4096 };
_Static_assert(CHARGE_PAGES<=15,"notification metadata charge bound");
static struct inode_watch_queue instances[INSTANCE_LIMIT];
_Static_assert(sizeof(instances)<=1024*1024,"notification pool bound");
static struct watch *buckets[HASH_SIZE];
/* Protected by the VFS mutex, like the watch table. A desktop interested in
   mutations must not force parent-path resolution for every loader/font read. */
static unsigned mask_users[12], active_masks;
static void change_mask(unsigned old,unsigned next) {
    for(unsigned bit=0;bit<12;bit++) {
        unsigned flag=1u<<bit;
        if(!((old^next)&flag))continue;
        if(next&flag){mask_users[bit]++;active_masks|=flag;}
        else if(!--mask_users[bit])active_masks&=~flag;
    }
}
static unsigned bucket(const struct vnode *v) {return ((v->id>>4)^(uintptr_t)v->mount)&(HASH_SIZE-1);}
static void publish(struct inode_watch_queue *q,unsigned n) {__atomic_store_n(&q->bytes,n,__ATOMIC_RELEASE);}
static void enqueue(struct inode_watch_queue *q,int wd,unsigned mask,unsigned cookie,const char *name) {
    struct {int wd;unsigned mask,cookie,len;} header={wd,mask,cookie,0};
    if(name && *name)header.len=(strlen(name)+1+3)&~3u;
    if(header.len>OS_NAME_MAX+4)return;
    uint8_t record[16+OS_NAME_MAX+4]={0};memcpy(record,&header,16);
    if(header.len)memcpy(record+16,name,strlen(name));
    unsigned length=16+header.len,used=q->bytes;
    if(used && q->last<used && used-q->last==length && !memcmp(q->queue+q->last,record,length))return;
    if(used+length>QUEUE_BYTES-16) {
        if(q->overflow)return;
        header=(typeof(header)){-1,IN_Q_OVERFLOW,0,0};length=16;
        memcpy(record,&header,16);q->overflow=true;
    }
    q->last=used;memcpy(q->queue+used,record,length);publish(q,used+length);
}
static void drop(struct watch *w,bool ignored) {
    struct vnode node={.mount=w->mount,.id=w->inode};
    struct watch **slot=&buckets[bucket(&node)];
    while(*slot && *slot!=w)slot=&(*slot)->next;
    if(*slot)*slot=w->next;
    if(ignored)enqueue(w->owner,w->wd,IN_IGNORED,0,NULL);
    change_mask(w->mask,0);
    memset(w,0,sizeof(*w));
    if(node.mount->ops->release)node.mount->ops->release(&node);
}
struct inode_watch_queue *inotify_allocate(void) {
    struct task *t=this_cpu()->current;
    struct user_budget *budget=t->space.vm?t->space.vm->budget:NULL;
    for(unsigned i=0;i<INSTANCE_LIMIT;i++)if(!instances[i].active) {
        if(budget && !user_budget_charge(budget,CHARGE_PAGES))return NULL;
        struct inode_watch_queue *q=&instances[i];memset(q,0,sizeof(*q));
        q->active=true;q->budget=budget;q->next_wd=1;if(budget)user_budget_get(budget);return q;
    }
    return NULL;
}
void inotify_release(struct inode_watch_queue *q) {
    for(unsigned i=0;i<WATCH_LIMIT;i++)if(q->watches[i].owner)drop(&q->watches[i],false);
    struct user_budget *budget=q->budget;memset(q,0,sizeof(*q));
    if(budget){user_budget_uncharge(budget,CHARGE_PAGES);user_budget_put(budget);}
}
bool inotify_mount_busy(const struct mount *m) {
    for(unsigned i=0;i<HASH_SIZE;i++)for(struct watch *w=buckets[i];w;w=w->next)if(w->mount==m)return true;
    return false;
}
void inotify_emit(const struct vnode *node,uint32_t mask,uint32_t cookie,const char *name) {
    unsigned hash=bucket(node);
    for(struct watch *w=buckets[hash],*next;w;w=next) {
        next=w->next;if(w->mount!=node->mount || w->inode!=node->id)continue;
        unsigned selected=mask&w->mask&0xfff;
        if(selected)enqueue(w->owner,w->wd,selected|(mask&IN_ISDIR),cookie,name);
        if((mask&IN_DELETE_SELF) || (selected && (w->mask&0x80000000u)))drop(w,true);
    }
}
uint32_t inotify_cookie(void) {static uint32_t cookie;if(!++cookie)cookie=1;return cookie;}
void inotify_file(const struct file *f,uint32_t mask) {
    if(!f->node.mount || (f->flags&O_PATH))return;
    if(!(active_masks&mask&0xfff))return;
    if((f->node.mode&0170000)==S_IFDIR)mask|=IN_ISDIR;
    inotify_emit(&f->node,mask,0,NULL);
    if(f->unlinked || f->path[0]!='/')return;
    char path[OS_PATH_MAX];strcpy(path,f->path);size_t at=strlen(path);
    while(at && path[at-1]!='/')at--;
    if(!at || !path[at])return;
    char *slash=path+at-1;
    char name[OS_NAME_MAX+1];strcpy(name,slash+1);if(slash==path)slash[1]=0;else *slash=0;
    struct vnode parent;if(!vfs_lookup(path,&parent))inotify_emit(&parent,mask,0,name);
}
long inotify_control(struct task *t,void *registers) {
    register_t *r=registers;
    if(r->rax==SYS_inotify_init || r->rax==SYS_inotify_init1) {
        uint64_t flags=r->rax==SYS_inotify_init1?r->rdi:0;
        if(flags&~(uint64_t)(O_CLOEXEC|O_NONBLOCK))return -E_INVAL;
        unsigned fd=0;while(fd<OS_FD_MAX && t->fds[fd])fd++;
        if(fd==OS_FD_MAX)return -E_MFILE;
        mutex_lock(&vfs_mutex);int e=vfs_inotify(flags,&t->fds[fd]);
        if(!e)t->fd_flags[fd]=flags&O_CLOEXEC?FD_CLOEXEC:0;
        mutex_unlock(&vfs_mutex);return e?e:(long)fd;
    }
    if(r->rdi>=OS_FD_MAX || !t->fds[r->rdi])return -E_BADF;
    struct file *f=t->fds[r->rdi];if(!f->inotify)return -E_INVAL;
    char path[OS_PATH_MAX];unsigned mask=r->rdx;struct vnode node;
    if(r->rax==SYS_inotify_add_watch) {
        if(r->rdx>>32 || !(mask&0xfff) || mask&~0xb7000fffu ||
           ((mask&0x30000000u)==0x30000000u))return -E_INVAL;
        char raw[OS_PATH_MAX];int e=user_string(&t->space,raw,r->rsi,sizeof(raw));if(e)return e;
        if(raw[0]!='/' && (e=s1_cwd_check(t)))return e;
        e=vfs_normalize(t->cwd,raw,path);if(e)return e;
    }
    mutex_lock(&vfs_mutex);long result=sandbox_file(f,SANDBOX_READ);
    struct inode_watch_queue *q=f->inotify;
    if(result)goto out;
    if(r->rax==SYS_inotify_rm_watch) {
        result=-E_INVAL;
        for(unsigned i=0;i<WATCH_LIMIT;i++)if(q->watches[i].owner && q->watches[i].wd==r->rsi) {
            drop(&q->watches[i],true);result=0;break;
        }
        goto out;
    }
    result=sandbox_path(path,SANDBOX_READ);if(result)goto out;
    result=(mask&0x02000000u)?vfs_lstat(path,&node):vfs_lookup(path,&node);if(result)goto out;
    result=credentials_node(&node,4);if(result)goto out;
    if((mask&0x01000000u) && (node.mode&0170000)!=S_IFDIR){result=-E_NOTDIR;goto out;}
    if(!node.mount || !node.mount->ops->retain){result=-E_NOTSUP;goto out;}
    struct watch *empty=NULL;
    for(unsigned i=0;i<WATCH_LIMIT;i++) {
        struct watch *w=&q->watches[i];if(!w->owner){if(!empty)empty=w;continue;}
        if(w->mount!=node.mount || w->inode!=node.id)continue;
        if(mask&0x10000000u){result=-E_EXIST;goto out;}
        unsigned next=(mask&0x20000000u)?w->mask|mask:mask;
        change_mask(w->mask,next);w->mask=next;result=w->wd;goto out;
    }
    if(!empty || q->next_wd>INT32_MAX){result=-E_NOSPC;goto out;}
    node.mount->ops->retain(&node);
    *empty=(struct watch){.owner=q,.mount=node.mount,.inode=node.id,.mask=mask,.wd=q->next_wd++, .next=buckets[bucket(&node)]};
    change_mask(0,mask);
    buckets[bucket(&node)]=empty;result=empty->wd;
out:mutex_unlock(&vfs_mutex);return result;
}
unsigned inotify_poll(struct file *f,unsigned events) {
    return __atomic_load_n(&f->inotify->bytes,__ATOMIC_ACQUIRE)?events&POLLIN:0;
}
long inotify_read(struct task *t,struct file *f,uint64_t pointer,size_t size) {
    if(!size)return 0;
    if(!user_access(&t->space,pointer,size,true))return -E_FAULT;
    for(;;) {
        mutex_lock(&vfs_mutex);struct inode_watch_queue *q=f->inotify;
        long result=sandbox_file(f,SANDBOX_READ);unsigned count=0;bool overflow_read=false;
        if(!result)while(count<q->bytes) {
            uint32_t length;memcpy(&length,q->queue+count+12,4);length+=16;
            if(count+length>size)break;
            uint32_t mask;memcpy(&mask,q->queue+count+4,4);
            overflow_read|=(mask&IN_Q_OVERFLOW)!=0;
            count+=length;
        }
        if(!result && q->bytes && !count)result=-E_INVAL;
        if(!result && count) {
            if(!user_copy_out(&t->space,pointer,q->queue,count))result=-E_FAULT;
            else {
                memmove(q->queue,q->queue+count,q->bytes-count);
                q->last=q->last>=count?q->last-count:0;
                unsigned remaining=q->bytes-count;publish(q,remaining);
                if(!remaining || overflow_read)q->overflow=false;
                result=count;
            }
        }
        mutex_unlock(&vfs_mutex);if(result)return result;
        if(f->flags&O_NONBLOCK)return -E_AGAIN;
        if(task_pending_signal(t) || signal_pending(t))return -E_INTR;
        sched_sleep(1000);
    }
}
