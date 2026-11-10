/* Account-local pathname streams for the admitted ordinary Electron process.
 * No internet/abstract sockets or namespace callers. Path names are real RAMFS socket
 * inodes directly in the canonical private runtime directory. Lookup requires
 * both inode identity and the retained process-tree budget. Existing socketpair
 * queues, rights-cycle checks, half close and reference ownership carry data.
 * Every live slot (including its pair and pending references) charges one page.
 * All table/queue mutations hold vfs_mutex; publication is transactional.
 */
enum { PRIVATE_STREAM_LIMIT=128,PRIVATE_BACKLOG=4,ACCOUNT_STREAM_LIMIT=16 };
struct private_stream {
    struct unix_pair *pair;
    struct user_budget *budget;
    unsigned state,backlog,pending_count; /* 0 fresh, 1 bound, 2 listener, 3 connected */
    unsigned family;
    bool account;
    unsigned owner_uid;
    uint16_t ports[2];uint8_t options[2];
    uint16_t keepalive[2][3];
    uint8_t tos[2];
    struct vnode node;
    char names[2][108];
    struct unix_peer_credentials peers[2];
    struct file *pending[PRIVATE_BACKLOG];
};
_Static_assert(sizeof(struct private_stream)+sizeof(struct unix_pair)<PAGE_SIZE,"private socket metadata charge");
static struct private_stream private_streams[PRIVATE_STREAM_LIMIT];
static struct private_stream *private_find(struct unix_pair *pair) {
    for(unsigned i=0;i<PRIVATE_STREAM_LIMIT;i++)if(private_streams[i].pair==pair)return &private_streams[i];
    return NULL;
}
static bool private_admitted(const struct task *t) {
    return CONFIG_ELECTRON_PROFILE && t && credentials_uid(t)==1000 && credentials_gid(t)==1000 &&
           !t->browser_sandbox && !t->context->web_role && !pidns_active(t) && userns_admitted_account(t);
}
/* Native account IPC has normal pathname DAC. It is a separate class from
   the existing editor process-tree capability and cannot be used by browser,
   user-namespace or PID-namespace processes to escape their private paths. */
static bool account_admitted(const struct task *t) {
    return t && t->user && t->context && !t->browser_sandbox && !t->context->web_role &&
           !t->userns && !pidns_active(t);
}
static bool private_owner(const struct task *t,const struct private_stream *s) {
    if(s->account)return account_admitted(t); /* caller already owns this FD */
    return private_admitted(t) && t->space.vm->budget==s->budget;
}
unsigned unix_vector_limit(struct task *t,struct file *file) {
    if(!file || !file->unix_socket)return 16;
    mutex_lock(&vfs_mutex);
    struct private_stream *s=private_find(file->unix_socket->pair);
    unsigned limit=s && private_owner(t,s) && s->state==3?1024:16;
    mutex_unlock(&vfs_mutex);return limit;
}
static void private_pair_release(struct unix_pair *pair) {
    struct private_stream *s=private_find(pair);if(!s)return;
    struct file *pending[PRIVATE_BACKLOG];memcpy(pending,s->pending,sizeof(pending));
    struct user_budget *budget=s->budget;
    struct vnode node=s->node;
    memset(s,0,sizeof(*s));
    for(unsigned i=0;i<PRIVATE_BACKLOG;i++)if(pending[i])vfs_close(pending[i]);
    if(node.mount && node.mount->ops->release)node.mount->ops->release(&node);
    user_budget_uncharge(budget,1);user_budget_put(budget);
}
#include "private_loopback.inc"
bool unix_private_call(struct task *t,const void *registers) {
    const register_t *r=registers;
    bool account=account_admitted(t);
    if(!private_admitted(t) && !account)return false;
    if(r->rax==SYS_socket)return (r->rdi==1 || (!account && r->rdi==2)) &&
        (r->rsi&~(uint64_t)(SOCK_NONBLOCK|SOCK_CLOEXEC))==1 &&
        (!r->rdx || (r->rdi==2 && r->rdx==6));
    mutex_lock(&t->context->syscall_lock);mutex_lock(&vfs_mutex);
    struct file *f=r->rdi<OS_FD_MAX?t->fds[r->rdi]:NULL;
    struct private_stream *s=f && f->unix_socket?private_find(f->unix_socket->pair):NULL;
    bool allowed=s && private_owner(t,s);
    mutex_unlock(&vfs_mutex);mutex_unlock(&t->context->syscall_lock);return allowed;
}
long unix_stream_socket(struct task *t,void *registers) {
    register_t *r=registers;
    bool account=account_admitted(t);
    if(!private_admitted(t) && !account)return -E_AFNOSUPPORT;
    if((r->rdi!=1 && (account || r->rdi!=2)) || (r->rsi&~(uint64_t)(SOCK_NONBLOCK|SOCK_CLOEXEC))!=1 ||
       (r->rdx && (r->rdi!=2 || r->rdx!=6)))return -E_PROTONOSUPPORT;
    mutex_lock(&t->context->syscall_lock);mutex_lock(&vfs_mutex);
    unsigned fd=0;while(fd<OS_FD_MAX && t->fds[fd])fd++;
    struct private_stream *s=NULL;
    for(unsigned i=0;i<PRIVATE_STREAM_LIMIT;i++)if(!private_streams[i].pair){s=&private_streams[i];break;}
    unsigned owned=0,private_count=0;
    for(unsigned i=0;i<PRIVATE_STREAM_LIMIT;i++)if(private_streams[i].pair) {
        if(!private_streams[i].account)private_count++;
        else if(private_streams[i].owner_uid==credentials_uid(t))owned++;
    }
    long result=-E_NOBUFS;
    if(fd==OS_FD_MAX)result=-E_MFILE;
    else if(account && owned>=ACCOUNT_STREAM_LIMIT)result=-E_NOBUFS;
    else if(!account && private_count>=32)result=-E_NOBUFS;
    else if(s && pair_count<SOCKET_LIMIT) {
        struct user_budget *budget=t->space.vm->budget;
        if(!user_budget_charge(budget,1))result=-E_NOMEM;
        else {
            struct unix_pair *p=kmallocz(sizeof(*p));
            if(!p){user_budget_uncharge(budget,1);result=-E_NOMEM;}
            else {
                p->type=1;p->peer=(struct unix_peer_credentials){(int32_t)t->tgid,credentials_uid(t),credentials_gid(t)};
                p->sockets[0]=(struct unix_socket){.pair=p,.side=0};
                p->sockets[1]=(struct unix_socket){.pair=p,.side=1};
                result=vfs_unix_socket(&p->sockets[0],r->rsi,&t->fds[fd]);
                if(result){kfree(p);user_budget_uncharge(budget,1);}
                else {
                    user_budget_get(budget);*s=(struct private_stream){.pair=p,.budget=budget,.family=r->rdi,
                        .account=account,.owner_uid=credentials_uid(t)};
                    p->open[0]=true;pair_count++;
                    t->fd_flags[fd]=(r->rsi&SOCK_CLOEXEC)?FD_CLOEXEC:0;result=fd;
                }
            }
        }
    }
    mutex_unlock(&vfs_mutex);mutex_unlock(&t->context->syscall_lock);return result;
}
/* vfs_mutex held; no user pointer or lexical prefix grants path authority. */
static int private_path(struct task *t,uint64_t pointer,uint64_t length,char out[108],bool account,char display[108]) {
    struct {uint16_t family;char path[109];} address={0};
    if(length<3 || length>110)return -E_INVAL;
    if(!user_copy_in(&t->space,&address,pointer,length))return -E_FAULT;
    if(address.family!=1 || !address.path[0])return -E_ACCES;
    if(strlen(address.path)>=108)return -E_NAMETOOLONG;
    strcpy(display,address.path);
    char normal[OS_PATH_MAX],parent_path[OS_PATH_MAX],canonical[OS_PATH_MAX];
    int error=vfs_normalize(t->cwd,address.path,normal);if(error)return error;
    size_t slash=strlen(normal);while(slash && normal[slash-1]!='/')slash--;
    if(!slash || !normal[slash])return -E_ACCES;
    memcpy(parent_path,normal,slash);parent_path[slash]=0;
    error=vfs_realpath(parent_path,canonical);if(error)return error;
    if(!account && strcmp(canonical,"/home/port-test/runtime"))return -E_ACCES;
    int size=snprintf(out,108,"%s/%s",canonical,normal+slash);
    return size<0 || size>=108?-E_NAMETOOLONG:0;
}
static long private_name(struct task *t,register_t *r,const char *name) {
    uint32_t length;if(!user_copy_in(&t->space,&length,r->rdx,4))return -E_FAULT;
    uint32_t actual=*name?2+strlen(name)+1:2;
    struct {uint16_t family;char path[108];} address={.family=1};strcpy(address.path,name);
    if(!user_access(&t->space,r->rsi,min(length,actual),true) || !user_access(&t->space,r->rdx,4,true))return -E_FAULT;
    return user_copy_out(&t->space,r->rsi,&address,min(length,actual)) && user_copy_out(&t->space,r->rdx,&actual,4)?0:-E_FAULT;
}
static long private_control(struct task *t,register_t *r,struct file *f) {
    for(;;) {
        if(task_pending_signal(t))return -E_INTR;
        mutex_lock(&t->context->syscall_lock);mutex_lock(&vfs_mutex);
        struct unix_pair *p=f->unix_socket->pair;struct private_stream *s=private_find(p);
        long result=-E_PERM;
        if(!s || !private_owner(t,s))goto done;
        char path[108],display[108];
        switch(r->rax) {
        case SYS_bind:
            if(s->family==2){result=private_inet_bind(t,r,s);break;}
            if(s->state){result=-E_INVAL;break;}
            result=private_path(t,r->rsi,r->rdx,path,s->account,display);if(result)break;
            result=s->account?vfs_socket_node_mode(path,0777&~s1_mask(t,false,0),&s->node):vfs_socket_node(path,&s->node);
            if(result==-E_EXIST)result=-E_ADDRINUSE;
            if(!result){if(s->node.mount->ops->retain)s->node.mount->ops->retain(&s->node);strcpy(s->names[0],s->account?display:path);s->state=1;}
            break;
        case SYS_listen:
            if(s->state!=1 && s->state!=2){result=-E_INVAL;break;}
            s->backlog=(int)r->rsi<0?PRIVATE_BACKLOG:min(max((unsigned)r->rsi,1u),(unsigned)PRIVATE_BACKLOG);
            s->peers[0]=(struct unix_peer_credentials){(int32_t)t->tgid,credentials_uid(t),credentials_gid(t)};s->state=2;result=0;break;
        case SYS_connect: {
            if(s->family==2){result=private_inet_connect(t,r,s);break;}
            if(s->state>=2){result=s->state==3?-E_ISCONN:-E_INVAL;break;}
            result=private_path(t,r->rsi,r->rdx,path,s->account,display);if(result)break;
            struct vnode node;result=s->account?vfs_lookup(path,&node):vfs_lstat(path,&node);if(result)break;
            if((node.mode&0170000)!=S_IFSOCK){result=-E_CONNREFUSED;break;}
            if(s->account){result=credentials_node(&node,2);if(result)break;}
            struct private_stream *listener=NULL;
            for(unsigned i=0;i<PRIVATE_STREAM_LIMIT;i++) {
                struct private_stream *q=&private_streams[i];
                if(q->pair && q->account==s->account && (s->account || q->budget==s->budget) && q->state==2 && q->node.mount==node.mount && q->node.id==node.id && q->node.aux[3]==node.aux[3]){listener=q;break;}
            }
            if(!listener){result=-E_CONNREFUSED;break;}
            if(listener->pending_count>=listener->backlog){result=-E_AGAIN;break;}
            struct file *accepted=NULL;result=vfs_unix_socket(&p->sockets[1],0,&accepted);if(result)break;
            p->open[1]=true;s->state=3;
            s->peers[0]=listener->peers[0];s->peers[1]=(struct unix_peer_credentials){(int32_t)t->tgid,credentials_uid(t),credentials_gid(t)};
            strcpy(s->names[1],listener->names[0]);
            listener->pending[listener->pending_count++]=accepted;result=0;break;
        }
        case SYS_accept:case SYS_accept4: {
            unsigned flags=r->rax==SYS_accept4?r->r10:0;
            if(flags&~(SOCK_CLOEXEC|SOCK_NONBLOCK)){result=-E_INVAL;break;}
            if(s->state!=2){result=-E_INVAL;break;}
            if(!s->pending_count){result=-E_AGAIN;break;}
            unsigned fd=0;while(fd<OS_FD_MAX && t->fds[fd])fd++;
            if(fd==OS_FD_MAX){result=-E_MFILE;break;}
            struct file *accepted=s->pending[0];
            if(r->rsi) {
                struct private_stream *client=private_find(accepted->unix_socket->pair);
                result=client->family==2?private_inet_name(t,r,client->ports[0]):private_name(t,r,client->names[0]);if(result)break;
            }
            for(unsigned i=1;i<s->pending_count;i++)s->pending[i-1]=s->pending[i];
            s->pending[--s->pending_count]=NULL;accepted->flags=O_RDWR|(flags&SOCK_NONBLOCK);
            t->fds[fd]=accepted;t->fd_flags[fd]=(flags&SOCK_CLOEXEC)?FD_CLOEXEC:0;result=fd;break;
        }
        default:result=-E_NOTSUP;
        }
done:
        mutex_unlock(&vfs_mutex);mutex_unlock(&t->context->syscall_lock);
        if(result!=-E_AGAIN || (f->flags&O_NONBLOCK))return result;
        sched_sleep(1000);
    }
}
