/* Guest-local account policy. The desktop is a trusted root session broker;
 * ordinary terminal processes receive irreversible, distinct credentials.
 * Sudo is a protected set-ID front end; environment grants and Electron
 * namespace exceptions cannot authorize elevation.
 */
#include <accounts.h>
#include <credentials.h>
#include <uapi/account.h>
#include <crypto/sha256.h>
#include <random.h>
#include <string.h>
#include <printk.h>
#include <tsc.h>
#include <tty.h>
#define ROUNDS 600000u
#define POLICY_PATH "/etc/coast-accounts"
struct account {
    uint32_t uid, flags, timeout, rounds;
    uint8_t salt[16], digest[32];
};
struct database { uint32_t magic, version; struct account root,users[OS_ACCOUNT_COUNT]; };
static struct database policy;
static struct file *store;
static mutex_t lock;
static bool loaded;
static struct { unsigned failures; uint64_t blocked_until; } attempts[OS_ACCOUNT_COUNT+1];
/* Authorization caches belong to an exact terminal endpoint, originating
 * session and policy generation. Never a user's environment or PID alone. */
static struct { unsigned uid, sid, generation; void *tty; uint64_t until; } tickets[64];
static unsigned generation=1,console_uid=OS_ACCOUNT_FIRST;
static void erase(void *p,size_t n) { volatile uint8_t *b=p;while(n--)*b++=0; }
static bool valid_uid(unsigned uid) { return uid>=OS_ACCOUNT_FIRST && uid<OS_ACCOUNT_FIRST+OS_ACCOUNT_COUNT; }
static struct account *find(unsigned uid) {
    if(!uid)return &policy.root;
    if(!valid_uid(uid))return NULL;
    struct account *a=&policy.users[uid-OS_ACCOUNT_FIRST];return a->uid==uid?a:NULL;
}
static bool terminated(const char *p) { for(unsigned i=0;i<128;i++)if(!p[i])return true;return false; }
/* PBKDF2-HMAC-SHA256, one 256-bit block. SHA256 states after ipad/opad are
 * copied, avoiding repeated key processing; comparison is constant-time. */
static void derive(const char *password,const uint8_t salt[16],uint8_t out[32]) {
    uint8_t key[64]={0}, pad[64], u[32], block[20];
    size_t n=strlen(password);
    if(n>64) { struct sha256 s;sha256_init(&s);sha256_update(&s,password,n);sha256_final(&s,key); }
    else memcpy(key,password,n);
    struct sha256 inner,outer,s;
    for(unsigned i=0;i<64;i++)pad[i]=key[i]^0x36;
    sha256_init(&inner);sha256_update(&inner,pad,64);
    for(unsigned i=0;i<64;i++)pad[i]=key[i]^0x5c;
    sha256_init(&outer);sha256_update(&outer,pad,64);
    memcpy(block,salt,16);block[16]=block[17]=block[18]=0;block[19]=1;
    s=inner;sha256_update(&s,block,20);sha256_final(&s,u);
    s=outer;sha256_update(&s,u,32);sha256_final(&s,u);memcpy(out,u,32);
    for(unsigned j=1;j<ROUNDS;j++) {
        s=inner;sha256_update(&s,u,32);sha256_final(&s,u);
        s=outer;sha256_update(&s,u,32);sha256_final(&s,u);
        for(unsigned i=0;i<32;i++)out[i]^=u[i];
    }
    erase(key,sizeof(key));erase(pad,sizeof(pad));erase(u,sizeof(u));
    erase(&inner,sizeof(inner));erase(&outer,sizeof(outer));erase(&s,sizeof(s));
}
static int authenticate(struct account *a,const char *password,bool allow_unset) {
    if(!a)return -E_ACCES;
    unsigned i=a->uid?1+a->uid-OS_ACCOUNT_FIRST:0;
    if(attempts[i].blocked_until>tsc_uptime_us())return -E_AGAIN;
    if(!(a->flags&OS_ACCOUNT_PASSWORD))return allow_unset?0:-E_ACCES;
    uint8_t digest[32];derive(password,a->salt,digest);unsigned diff=0;
    for(unsigned j=0;j<32;j++)diff|=digest[j]^a->digest[j];
    erase(digest,sizeof(digest));
    if(diff) {
        if(++attempts[i].failures>=5) {attempts[i].blocked_until=tsc_uptime_us()+30000000;attempts[i].failures=0;}
        return -E_ACCES;
    }
    attempts[i].failures=0;attempts[i].blocked_until=0;return 0;
}
static int persist(void) {
    mutex_lock(&vfs_mutex);store->offset=0;
    long n=vfs_write(store,&policy,sizeof(policy));
    int e=n==(long)sizeof(policy)?0:n<0?n:-E_IO;
    if(!e && store->node.mount->ops->sync)e=store->node.mount->ops->sync(store->node.mount);
    mutex_unlock(&vfs_mutex);return e;
}
static int load_policy(void) {
    if(loaded)return 0;
    mutex_lock(&vfs_mutex);
    int e=vfs_mkdir("/etc",0755);if(e==-E_EXIST)e=0;
    if(!e) {e=vfs_mkdir("/root",0700);if(e==-E_EXIST)e=0;}
    bool fresh=false;
    if(!e) {e=vfs_open(POLICY_PATH,O_RDWR|O_NOFOLLOW,0,&store);if(e==-E_NOENT) {fresh=true;e=vfs_open(POLICY_PATH,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW,0600,&store);}}
    if(!e) {
        struct vnode *n=&store->node;
        if(n->uid || n->gid || (n->mode&0177777)!=(S_IFREG|0600) || strcmp(n->mount->type,"ramfs"))e=-E_ACCES;
        if(!e && !fresh) {
            long size=vfs_read(store,&policy,sizeof(policy));uint8_t extra;
            if(size!=(long)sizeof(policy) || vfs_read(store,&extra,1)!=0 || policy.magic!=0x41434354 || policy.version!=2)e=-E_INVAL;
            if(!e && (policy.root.uid || policy.root.flags&~3u || !(policy.root.flags&OS_ACCOUNT_ADMIN) || policy.root.rounds!=ROUNDS))e=-E_INVAL;
            for(unsigned i=0;i<OS_ACCOUNT_COUNT && !e;i++) {
                struct account *a=&policy.users[i];
                if(a->uid && (a->uid!=OS_ACCOUNT_FIRST+i || (a->flags&~3u) || a->timeout>15 || a->rounds!=ROUNDS))e=-E_INVAL;
            }
        }
    }
    if(e && store){vfs_close(store);store=NULL;}
    mutex_unlock(&vfs_mutex);
    if(e)return e;
    if(fresh) {memset(&policy,0,sizeof(policy));policy.magic=0x41434354;policy.version=2;policy.root=(struct account){.flags=OS_ACCOUNT_ADMIN,.rounds=ROUNDS};e=persist();}
    if(e && store){mutex_lock(&vfs_mutex);vfs_close(store);store=NULL;mutex_unlock(&vfs_mutex);}
    loaded=!e;return e;
}
bool accounts_console(const struct task *t,const struct file *f) {
    return t && t->context && t->context->account_console && f && f->tty;
}
bool accounts_window_client(const struct task *t) {
    return t && t->context && !t->userns && !t->browser_sandbox && !t->context->web_role &&
        valid_uid(credentials_uid(t)) && credentials_uid(t)==credentials_ruid(t) &&
        t->context->account_uid==credentials_uid(t) && credentials_uid(t)==__atomic_load_n(&console_uid,__ATOMIC_ACQUIRE);
}
/* Only this immutable root-owned native executable is a desktop broker.
 * No interpreter, namespace, NNP task, user-writable file or path alias is
 * admitted. Arguments cannot select a program, library or privileged script. */
void accounts_broker_commit(struct task *t,const char *path,const struct file *identity) {
    struct task *origin=this_cpu()->current;
    if(!origin || origin->userns || origin->browser_sandbox || origin->context->web_role || origin->context->account_nnp)return;
    if(strcmp(path,"/bin/gui") || !identity || strcmp(identity->path,"/bin/gui") ||
       identity->node.uid || identity->node.gid || (identity->node.mode&0022) ||
       (identity->node.mode&0170000)!=S_IFREG || t->userns || t->browser_sandbox ||
       t->context->web_role || t->context->account_nnp)return;
    unsigned login=t->context->account_uid?t->context->account_uid:OS_ACCOUNT_FIRST;
    if(!valid_uid(login))return;
    t->context->desktop_login_uid=login;t->context->desktop_fsuid=login;
    t->context->account_uid=0;t->context->account_session=0;t->context->account_owner=0;t->context->account_console=false;
    credentials_all_ids(t,0,0);
}
bool accounts_terminal(const struct task *t,const struct file *f) {
    return t && t->context && t->context->account_uid && f && f->pty && !f->pty_master;
}
long accounts_prctl(struct task *t,const register_t *r) {
    if(r->rdi!=38 && r->rdi!=39)return -E_NOSYS;
    if(r->rsi!=(r->rdi==38?1u:0u) || r->rdx || r->r10 || r->r8)return -E_INVAL;
    if(r->rdi==38)t->context->account_nnp=true;
    return r->rdi==38?0:t->context->account_nnp;
}
static void *terminal(struct task *t) {
    for(unsigned i=0;i<3;i++)if(accounts_terminal(t,t->fds[i]))return t->fds[i]->pty;
    return t->context->account_console?(void *)1:NULL;
}
static int authorize(struct task *t,struct account *a,const char *password,bool elevate) {
    if(!a || !(a->flags&OS_ACCOUNT_ADMIN) || !(a->flags&OS_ACCOUNT_PASSWORD) ||
       t->context->account_nnp || t->userns || t->browser_sandbox || t->context->web_role ||
       !credentials_private_transition(t))return -E_PERM;
    void *tty=terminal(t);uint64_t now=tsc_uptime_us();bool cached=false;
    if(tty)for(unsigned i=0;i<64;i++)cached|=tickets[i].uid==a->uid && tickets[i].sid==t->context->account_session &&
        tickets[i].tty==tty && tickets[i].generation==generation && tickets[i].until>now;
    int e=cached?0:password[0]?authenticate(a,password,false):-E_ACCES;if(e)return e;
    if(tty && a->timeout) {
        unsigned slot=64;for(unsigned i=0;i<64;i++)if(tickets[i].until<=now || (tickets[i].uid==a->uid && tickets[i].sid==t->context->account_session && tickets[i].tty==tty)){slot=i;break;}
        if(slot<64) {tickets[slot].uid=a->uid;tickets[slot].sid=t->context->account_session;tickets[slot].tty=tty;tickets[slot].generation=generation;tickets[slot].until=now+(uint64_t)a->timeout*60000000;}
    }
    if(elevate) {
        t->context->desktop_fsuid=0;
        credentials_all_ids(t,0,0);t->context->group_count=0;
        pr_info("ACCOUNT_SUDO uid=%u pid=%u authenticated=1\n",a->uid,t->tgid);
    }
    return 0;
}
long accounts_call(struct task *t,register_t *r) {
    if(t->browser_sandbox || t->context->web_role || t->userns)return -E_PERM;
    unsigned op=r->rdi;struct os_account_request q;
    if(!user_copy_in(&t->space,&q,r->rsi,sizeof(q)))return -E_FAULT;
    if(q.reserved || !terminated(q.password) || !terminated(q.new_password)) {erase(&q,sizeof(q));return -E_INVAL;}
    bool root=!credentials_uid(t);int e=0;mutex_lock(&lock);
    if(op==OS_ACCOUNT_FILE_SCOPE) {
        long result=-E_PERM;
        if(root && (!q.uid || find(q.uid))) {
            if(q.uid && t->context->desktop_login_uid && q.uid!=t->context->desktop_login_uid)t->context->desktop_fsuid=t->context->desktop_login_uid;
            else {result=t->context->desktop_fsuid;t->context->desktop_fsuid=q.uid;}
        }
        mutex_unlock(&lock);erase(&q,sizeof(q));return result;
    }
    if(!loaded) e=root?load_policy():-E_ACCES;
    if(!e && op==OS_ACCOUNT_CONSOLE_SYNC) {
        if(t->tgid!=1 || !t->context->account_console)e=-E_PERM;
        else {
            credentials_all_ids(t,console_uid,console_uid);t->context->account_uid=console_uid;
            t->context->account_owner=console_uid;t->context->account_session=t->tgid;
            t->context->desktop_fsuid=0;
        }
        mutex_unlock(&lock);erase(&q,sizeof(q));return e;
    }
    if(!e && op==OS_ACCOUNT_CURRENT)q.uid=t->context->desktop_login_uid?t->context->desktop_login_uid:t->context->account_uid?t->context->account_uid:console_uid;
    struct account *a=find(q.uid);
    if(!e && op==OS_ACCOUNT_ENSURE) {
        if(!root || !valid_uid(q.uid))e=-E_PERM;
        else if(!a && t->context->desktop_login_uid && !(policy.users[0].flags&OS_ACCOUNT_PASSWORD))e=-E_ACCES;
        else if(!a) {
            bool first=true;for(unsigned i=0;i<OS_ACCOUNT_COUNT;i++)first&=!policy.users[i].uid;
            if(first && q.uid!=OS_ACCOUNT_FIRST)e=-E_PERM;
            else {a=&policy.users[q.uid-OS_ACCOUNT_FIRST];*a=(struct account){.uid=q.uid,.flags=first?OS_ACCOUNT_ADMIN:0,.timeout=5,.rounds=ROUNDS};e=persist();if(e)memset(a,0,sizeof(*a));}
        }
    } else if(!e && (op==OS_ACCOUNT_QUERY || op==OS_ACCOUNT_CURRENT)) {
        if(!a)e=-E_NOENT;
        else if(!root && q.uid!=t->context->account_uid)e=-E_PERM;
    } else if(!e && op==OS_ACCOUNT_UPDATE) {
        bool self_password=!root && q.uid==credentials_uid(t) && q.actor==q.uid && q.changes==OS_ACCOUNT_SET_PASSWORD;
        if((!root && !self_password) || !a || !q.changes || (q.changes&~7u) || (q.flags&~OS_ACCOUNT_ADMIN) || q.timeout_minutes>15 ||
           (t->context->desktop_login_uid && q.actor!=t->context->desktop_login_uid))e=-E_PERM;
        struct account *actor=find(q.actor);
        if(!e && q.actor) {
            if(!actor || (q.uid!=q.actor && !(actor->flags&OS_ACCOUNT_ADMIN)) ||
               ((q.changes&OS_ACCOUNT_ROLE) && !(actor->flags&OS_ACCOUNT_ADMIN)))e=-E_PERM;
            else e=authenticate(actor,q.password,true);
        }
        if(!e && (q.changes&OS_ACCOUNT_SET_PASSWORD) && strlen(q.new_password)<8)e=-E_INVAL;
        if(!e && (q.changes&OS_ACCOUNT_ROLE) && !(q.flags&OS_ACCOUNT_ADMIN) && (a->flags&OS_ACCOUNT_ADMIN)) {
            unsigned admins=0;for(unsigned i=0;i<OS_ACCOUNT_COUNT;i++)admins+=(policy.users[i].flags&OS_ACCOUNT_ADMIN)!=0;
            if(admins<2)e=-E_PERM;
        }
        if(!e) {
            struct account old=*a;
            if(q.changes&OS_ACCOUNT_ROLE)a->flags=(a->flags&OS_ACCOUNT_PASSWORD)|q.flags;
            if(q.changes&OS_ACCOUNT_TIMEOUT)a->timeout=q.timeout_minutes;
            if(q.changes&OS_ACCOUNT_SET_PASSWORD) {
                if(random_bytes(a->salt,16)!=16)e=-E_IO;
                else {derive(q.new_password,a->salt,a->digest);a->flags|=OS_ACCOUNT_PASSWORD;}
            }
            if(!e)e=persist();
            if(e)*a=old;
            else {generation++;pr_info("ACCOUNT_POLICY uid=%u admin=%u password_set=%u timeout=%u\n",a->uid,!!(a->flags&OS_ACCOUNT_ADMIN),!!(a->flags&OS_ACCOUNT_PASSWORD),a->timeout);}
            erase(&old,sizeof(old));
        }
    } else if(!e && op==OS_ACCOUNT_AUTH) {
        t->context->pending_login_uid=0;t->context->pending_login_generation=0;
        e=root?authenticate(a,q.password,true):-E_PERM;
        if(!e && (a->flags&OS_ACCOUNT_ADMIN) && !(a->flags&OS_ACCOUNT_PASSWORD) && t->context->desktop_login_uid && t->context->desktop_login_uid!=a->uid)e=-E_ACCES;
        if(!e) {t->context->pending_login_uid=q.uid;t->context->pending_login_generation=generation;}
    } else if(!e && op==OS_ACCOUNT_LOGIN) {
        if(!root || !a || !t->context->desktop_login_uid || t->context->pending_login_uid!=q.uid || t->context->pending_login_generation!=generation)e=-E_PERM;
        else {
            task_account_revoke(t->context->desktop_login_uid);
            t->context->desktop_login_uid=q.uid;t->context->desktop_fsuid=q.uid;
            t->context->pending_login_uid=0;t->context->pending_login_generation=0;
            __atomic_store_n(&console_uid,q.uid,__ATOMIC_RELEASE);generation++;
        }
    } else if(!e && op==OS_ACCOUNT_CONSOLE_LOGIN) {
        if(!t->context->account_console || t->context->account_nnp || !tty_is_foreground(task_input_identity(t)) || !credentials_private_transition(t))e=-E_PERM;
        else {
            e=authenticate(a,q.password,true);
            if(!e && (a->flags&OS_ACCOUNT_ADMIN) && !(a->flags&OS_ACCOUNT_PASSWORD) && a->uid!=credentials_uid(t))e=-E_ACCES;
            if(!e){__atomic_store_n(&console_uid,q.uid,__ATOMIC_RELEASE);generation++;}
        }
    } else if(!e && op==OS_ACCOUNT_ENTER) {
        if(!root || !a || t->context->account_uid || (t->context->desktop_login_uid && t->context->desktop_login_uid!=q.uid) || !credentials_private_transition(t))e=-E_PERM;
        else {
            t->context->account_uid=q.uid;t->context->account_owner=q.uid;t->context->account_session=t->tgid;
            t->context->account_console=t->tgid==1;
            t->context->desktop_login_uid=0;t->context->desktop_fsuid=0;
            credentials_all_ids(t,q.uid,q.uid);
            t->context->creation_mask=0077;
        }
    } else if(!e && (op==OS_ACCOUNT_SUDO || op==OS_ACCOUNT_VALIDATE)) {
        struct file *executable=t->context->executable;
        if(q.uid!=t->context->account_uid || credentials_ruid(t)!=q.uid || credentials_uid(t)!=0 || !executable ||
           strcmp(executable->path,"/bin/sudo") || executable->node.uid || !(executable->node.mode&04000))e=-E_PERM;
        else e=authorize(t,a,q.password,op==OS_ACCOUNT_SUDO);
    } else if(!e && op==OS_ACCOUNT_BROWSER_LAUNCH) {
        struct file *f=t->context->executable;
        if(!root || !credentials_private_transition(t) || !f ||
           strcmp(f->path,"/bin/chromium-wayland") || f->node.uid || f->node.gid ||
           (f->node.mode&0170000)!=S_IFREG || (f->node.mode&0022) ||
           q.uid!=OS_ACCOUNT_FIRST || t->context->account_nnp)e=-E_PERM;
        else t->context->browser_network=true;
    } else if(!e && op==OS_ACCOUNT_CODE_LAUNCH) {
        struct file *f=t->context->executable;unsigned real=credentials_ruid(t);
        bool frontend=f && (!strcmp(f->path,"/bin/code") || !strcmp(f->path,"/usr/bin/code")) &&
            !f->node.uid && !f->node.gid && (f->node.mode&0170000)==S_IFREG &&
            (f->node.mode&04000) && !(f->node.mode&0022);
        if(!root || !frontend || t->context->account_nnp || !credentials_private_transition(t) ||
           (real && (real!=OS_ACCOUNT_FIRST || t->context->account_uid!=real || __atomic_load_n(&console_uid,__ATOMIC_ACQUIRE)!=real)))e=-E_PERM;
        else {
            /* The static frontend execs one fixed bridge with fresh env/FDs.
               Retain logout ownership, but prepare its existing C2 enrollment. */
            credentials_all_ids(t,0,0);t->context->group_count=0;
            t->context->account_uid=0;t->context->account_session=0;
            t->context->desktop_fsuid=0;t->context->desktop_login_uid=0;t->context->account_console=false;
        }
    } else if(!e && op==OS_ACCOUNT_FORGET) {
        for(unsigned i=0;i<64;i++)if(tickets[i].uid==t->context->account_uid && tickets[i].sid==t->context->account_session && tickets[i].tty==terminal(t))tickets[i].until=0;
    } else if(!e)e=-E_INVAL;
    if(!e && (op==OS_ACCOUNT_QUERY || op==OS_ACCOUNT_CURRENT || op==OS_ACCOUNT_ENSURE)) {
        memset(&q,0,sizeof(q));q.uid=a->uid;q.flags=a->flags;q.timeout_minutes=a->timeout;
        if(!user_copy_out(&t->space,r->rsi,&q,sizeof(q)))e=-E_FAULT;
    }
    mutex_unlock(&lock);erase(&q,sizeof(q));return e;
}
