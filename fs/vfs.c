#include <filesystem.h>
#include <credentials.h>
#include <accounts.h>
#include <userns.h>
#include <pidns.h>
#include <fs/vfs.h>
#include <drivers/block.h>
#include <string.h>
#include <panic.h>
#include <console.h>
#include <sched.h>
#include <tty.h>
#include <pty.h>
#include <mm/pmm.h>
#include <random.h>
#include <net/network.h>
#include <fs/eventfd.h>
#include <fs/inotify.h>
#include <fs/unix_socket.h>
#include <fs/memfd.h>
#include <fs/epoll.h>
#include <sandbox.h>
#include <landlock.h>
#include <printk.h>

#define MOUNT_MAX 64
#define FILE_MAX 1024

static struct mount mounts[MOUNT_MAX];
static unsigned mount_count;
static uint64_t null_device, zero_device, random_device, urandom_device;
static struct file files[FILE_MAX];
_Static_assert(sizeof(files)<=1024*1024,"global open-file description pool bound");
static int file_limit_error(void) {
    struct task *task=sched_ready()?this_cpu()->current:NULL;
    if(CONFIG_ELECTRON_PROFILE && task && task->space.vm && task->space.vm->budget) {
        static unsigned reports;
        unsigned n=__atomic_load_n(&reports,__ATOMIC_RELAXED);
        while(n<16)if(__atomic_compare_exchange_n(&reports,&n,n+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {
            unsigned used=0,regular=0,pipes=0,unixs=0,nets=0,memory=0,epolls=0,ptys=0,other=0;
            for(unsigned i=0;i<FILE_MAX;i++)if(files[i].references) {
                struct file *f=&files[i];used++;
                if(f->pipe)pipes++;
                else if(f->unix_socket)unixs++;
                else if(f->socket)nets++;
                else if(f->memory)memory++;
                else if(f->epoll)epolls++;
                else if(f->pty)ptys++;
                else if((f->node.mode&0170000)==S_IFREG)regular++;
                else other++;
            }
            pr_warn("VFS_GLOBAL_FILE_LIMIT pid=%u tid=%u used=%u limit=%u regular=%u pipes=%u unix=%u net=%u memfd=%u epoll=%u pty=%u other=%u\n",
                    task->tgid,task->pid,used,FILE_MAX,regular,pipes,unixs,nets,memory,epolls,ptys,other);
            break;
        }
    }
    return -E_MFILE;
}
static void file_owner(struct file *f) {
    struct task *t=sched_ready()?this_cpu()->current:NULL;
    f->node.uid=credentials_uid(t);f->node.gid=credentials_gid(t);
}

/* Endpoint file descriptions, and their dup/inheritance reference counts,
   belong to the VFS. Blocking pipe transfers never hold vfs_mutex. */
#define PIPE_SIZE 16384
#define PIPE_MAX_SIZE (256 * 1024)

struct pipe_buffer {
    spinlock_t lock;
    unsigned head, tail, count, readers, writers, capacity;
    uint8_t *bytes;
};

mutex_t vfs_mutex;

int vfs_eventfd(uint32_t initial, unsigned flags, struct file **out) {
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (!files[i].references) {
            struct event_counter *counter = eventfd_allocate(initial, flags & 1);
            if (!counter)
                return -E_NOMEM;
            files[i] = (struct file){
                .references = 1, .flags = O_RDWR | (flags & O_NONBLOCK), .event = counter};
            struct task *t = this_cpu()->current;
            files[i].capability_domain = t->context->web_domain;
            files[i].capability_renderer = t->context->web_role == OS_WEB_RENDERER ? t->tgid : 0;
            files[i].node.mode = S_IFCHR | 0600;
            file_owner(&files[i]);
            files[i].node.aux[3] = i + 1;
            strcpy(files[i].path, "eventfd:");
            *out = &files[i];
            return 0;
        }
    return file_limit_error();
}

int vfs_inotify(unsigned flags,struct file **out) {
    for(unsigned i=0;i<FILE_MAX;i++)if(!files[i].references) {
        struct inode_watch_queue *queue=inotify_allocate();if(!queue)return -E_NOSPC;
        files[i]=(struct file){.references=1,.flags=O_RDONLY|(flags&O_NONBLOCK),.inotify=queue};
        struct task *t=this_cpu()->current;files[i].capability_domain=t->context->web_domain;
        files[i].capability_renderer=t->context->web_role==OS_WEB_RENDERER?t->tgid:0;
        files[i].node.mode=S_IFCHR|0600;file_owner(&files[i]);files[i].node.aux[3]=i+1;
        strcpy(files[i].path,"inotify:");*out=&files[i];return 0;
    }
    return file_limit_error();
}

int vfs_socket(struct net_socket *socket, unsigned flags, struct file **out) {
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (!files[i].references) {
            files[i] = (struct file){
                .references = 1, .flags = O_RDWR | (flags & O_NONBLOCK), .socket = socket};
            files[i].node.mode = S_IFSOCK | 0600;
            file_owner(&files[i]);
            files[i].node.aux[3] = i + 1;
            strcpy(files[i].path, "socket:");
            *out = &files[i];
            return 0;
        }
    return file_limit_error();
}

int vfs_unix_socket(struct unix_socket *socket, unsigned flags, struct file **out) {
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (!files[i].references) {
            files[i] = (struct file){
                .references = 1, .flags = O_RDWR | (flags & O_NONBLOCK), .unix_socket = socket};
            files[i].node.mode = S_IFSOCK | 0600;
            file_owner(&files[i]);
            files[i].node.aux[3] = i + 1;
            strcpy(files[i].path, "unix:");
            *out = &files[i];
            return 0;
        }
    return file_limit_error();
}

int vfs_memfd(unsigned flags, unsigned domain, struct file **out) {
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (!files[i].references) {
            struct memory_object *m = memfd_create(flags, domain);
            if (!m)
                return -E_NOMEM;
            files[i] = (struct file){.references = 1, .flags = O_RDWR, .memory = m};
            files[i].node.mode = S_IFREG | 0600;
            file_owner(&files[i]);
            files[i].node.aux[3] = i + 1;
            strcpy(files[i].path, "memfd:");
            *out = &files[i];
            return 0;
        }
    return file_limit_error();
}

int vfs_epoll(struct file **out) {
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (!files[i].references) {
            struct event_poll *p = epoll_allocate();
            if (!p)
                return -E_NOMEM;
            files[i] = (struct file){.references = 1, .flags = O_RDWR, .epoll = p};
            files[i].node.mode = S_IFCHR | 0600;
            file_owner(&files[i]);
            files[i].node.aux[3] = i + 1;
            strcpy(files[i].path, "epoll:");
            *out = &files[i];
            return 0;
        }
    return file_limit_error();
}

int vfs_pty(struct file *out[2], unsigned flags) {
    struct file *ends[2] = {0};
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (!files[i].references) {
            if (!ends[0])
                ends[0] = &files[i];
            else {
                ends[1] = &files[i];
                break;
            }
        }
    if (!ends[1])
        return file_limit_error();
    struct pty *p = pty_allocate();
    if (!p)
        return -E_NOMEM;
    for (unsigned i = 0; i < 2; i++) {
        *ends[i] = (struct file){
            .references = 1, .flags = O_RDWR | (flags & O_NONBLOCK), .pty = p, .pty_master = !i};
        ends[i]->node.mode = S_IFCHR | 0600;
        file_owner(ends[i]);
        strcpy(ends[i]->path, i ? "/dev/pts/session" : "/dev/ptmx");
        out[i] = ends[i];
    }
    return 0;
}

int vfs_pty_endpoint(struct pty *p,bool master,unsigned flags,const char *path,struct file **out) {
    for(unsigned i=0;i<FILE_MAX;i++)if(!files[i].references) {
        files[i]=(struct file){.references=1,.flags=O_RDWR|(flags&O_NONBLOCK),.pty=p,.pty_master=master};
        files[i].node.mode=S_IFCHR|0600;file_owner(&files[i]);
        strcpy(files[i].path,path);*out=&files[i];return 0;
    }
    return file_limit_error();
}

int vfs_pipe(struct file *out[2], unsigned flags) {
    struct file *ends[2] = {0};
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (!files[i].references) {
            if (!ends[0])
                ends[0] = &files[i];
            else {
                ends[1] = &files[i];
                break;
            }
        }
    if (!ends[1])
        return file_limit_error();
    struct pipe_buffer *p = kmalloc(sizeof(*p));
    if (!p)
        return -E_NOMEM;
    memset(p, 0, sizeof(*p));
    p->readers = p->writers = 1;
    p->capacity = PIPE_SIZE;
    p->bytes = kmalloc(p->capacity);
    if (!p->bytes) {
        kfree(p);
        return -E_NOMEM;
    }
    for (unsigned i = 0; i < 2; i++) {
        *ends[i] = (struct file){
            .references = 1, .flags = (i ? O_WRONLY : O_RDONLY) | (flags & O_NONBLOCK), .pipe = p};
        ends[i]->node.mode = S_IFIFO | 0600;
        file_owner(ends[i]);
        out[i] = ends[i];
    }
    return 0;
}

static void pipe_check_signal(void) {
    unsigned signal = task_pending_signal(this_cpu()->current);
    if (signal)
        task_exit(signal);
}

long pipe_capacity(struct file *f, bool resize, uint64_t requested) {
    if (!f->pipe)
        return -E_INVAL;
    struct pipe_buffer *p = f->pipe;
    if (!resize) {
        uint64_t irq = spin_lock_irqsave(&p->lock);
        unsigned capacity = p->capacity;
        spin_unlock_irqrestore(&p->lock, irq);
        return capacity;
    }
    if (requested > PIPE_MAX_SIZE)
        return -E_PERM;
    unsigned capacity = 4096;
    while (capacity < requested)
        capacity *= 2;
    uint8_t *bytes = kmalloc(capacity);
    if (!bytes)
        return -E_NOMEM;
    uint64_t irq = spin_lock_irqsave(&p->lock);
    if (p->count > capacity) {
        spin_unlock_irqrestore(&p->lock, irq);
        kfree(bytes);
        return -E_BUSY;
    }
    size_t first = min((size_t)p->count, (size_t)(p->capacity - p->tail));
    memcpy(bytes, p->bytes + p->tail, first);
    memcpy(bytes + first, p->bytes, p->count - first);
    uint8_t *previous = p->bytes;
    p->bytes = bytes;
    p->capacity = capacity;
    p->tail = 0;
    p->head = p->count % capacity;
    spin_unlock_irqrestore(&p->lock, irq);
    kfree(previous);
    return capacity;
}

long pipe_read(struct file *f, void *data, size_t size) {
    if (!size)
        return 0;
    struct pipe_buffer *p = f->pipe;
    for (;;) {
        pipe_check_signal();
        if (signal_pending(this_cpu()->current))
            return -E_INTR;
        uint64_t irq = spin_lock_irqsave(&p->lock);
        if (p->count) {
            size_t n = min(size, (size_t)p->count);
            size_t first = min(n, (size_t)(p->capacity - p->tail));
            memcpy(data, p->bytes + p->tail, first);
            memcpy((uint8_t *)data + first, p->bytes, n - first);
            p->tail = (p->tail + n) % p->capacity;
            p->count -= n;
            spin_unlock_irqrestore(&p->lock, irq);
            return n;
        }
        bool eof = !p->writers;
        spin_unlock_irqrestore(&p->lock, irq);
        if (eof)
            return 0;
        if (f->flags & O_NONBLOCK)
            return -E_AGAIN;
        sched_sleep(1000);
    }
}

long pipe_write(struct file *f, const void *data, size_t size) {
    if (!size)
        return 0;
    struct pipe_buffer *p = f->pipe;
    for (;;) {
        pipe_check_signal();
        if (signal_pending(this_cpu()->current))
            return -E_INTR;
        uint64_t irq = spin_lock_irqsave(&p->lock);
        if (!p->readers) {
            spin_unlock_irqrestore(&p->lock, irq);
            return -E_PIPE;
        }
        size_t room = p->capacity - p->count;
        if (room && (size > 4096 || room >= size)) {
            size_t n = min(size, room);
            size_t first = min(n, (size_t)(p->capacity - p->head));
            memcpy(p->bytes + p->head, data, first);
            memcpy(p->bytes, (const uint8_t *)data + first, n - first);
            p->head = (p->head + n) % p->capacity;
            p->count += n;
            spin_unlock_irqrestore(&p->lock, irq);
            return n;
        }
        spin_unlock_irqrestore(&p->lock, irq);
        if (f->flags & O_NONBLOCK)
            return -E_AGAIN;
        sched_sleep(1000);
    }
}

int vfs_normalize(const char *cwd, const char *path, char out[OS_PATH_MAX]) {
    if (!path || !path[0])
        return -E_NOENT;
    char joined[OS_PATH_MAX];
    int len = path[0] == '/' ? snprintf(joined, sizeof(joined), "%s", path)
                             : snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
    if (len < 0 || len >= OS_PATH_MAX)
        return -E_NAMETOOLONG;
    unsigned offsets[OS_PATH_MAX / 2], depth = 0;
    size_t used = 1;
    out[0] = '/';
    for (size_t i = 0; joined[i];) {
        while (joined[i] == '/')
            i++;
        size_t first = i;
        while (joined[i] && joined[i] != '/')
            i++;
        size_t n = i - first;
        if (!n)
            break;
        if (n == 1 && joined[first] == '.')
            continue;
        if (n == 2 && joined[first] == '.' && joined[first + 1] == '.') {
            if (depth)
                used = offsets[--depth];
            continue;
        }
        if (n > OS_NAME_MAX || used + n + 1 >= OS_PATH_MAX)
            return -E_NAMETOOLONG;
        offsets[depth++] = used;
        if (used > 1)
            out[used++] = '/';
        memcpy(out + used, joined + first, n);
        used += n;
    }
    out[used] = 0;
    return 0;
}

static bool prefix(const char *path, const char *mount) {
    size_t n = strlen(mount);
    return !strncmp(path, mount, n) && (n == 1 || !path[n] || path[n] == '/');
}

static int resolve_access(const char *path, struct vnode *out, unsigned depth, bool nofollow,
                   unsigned access, char *real,bool fd_metadata,struct landlock_walk *walk) {
    if (depth > 8)
        return -E_LOOP;
    int permission = sandbox_path(path, access);
    if (permission)
        return permission;
    struct mount *m = &mounts[0];
    for (unsigned i = 1; i < mount_count; i++)
        if (mounts[i].ops && prefix(path, mounts[i].path) &&
            strlen(mounts[i].path) > strlen(m->path))
            m = &mounts[i];
    /* C1 is deliberately confined to the boot RAMFS and read-only kernel
       metadata. Do not claim mounted
       filesystem DAC while resolution selects mounts ahead of parent traversal.
       Root/default applications retain their existing mount behavior. */
    if(credentials_fs_restricted(sched_ready()?this_cpu()->current:NULL) && m!=&mounts[0] &&
       (!CONFIG_USERNS_TEST || strcmp(m->type,"c2proc")) &&
       !(sysfs_readonly_mount(m) && !(access&SANDBOX_WRITE)))return -E_ACCES;
    struct vnode node;
    bool rooted=s1_root(&node);
    int e=0;
    if(rooted)m=node.mount;
    else e=m->ops->root(m,&node);
    if(e)return e;
    if(walk) {
        landlock_walk_begin(walk);
        if(!rooted && m!=&mounts[0]) {
            /* Resolve the parent mount as well: a directory grant follows its
               hierarchy across a mounted filesystem, but an unrelated sibling
               grant must never leak into it. */
            char ancestor[OS_PATH_MAX];strcpy(ancestor,m->path);
            char *slash=strrchr(ancestor,'/');
            if(slash==ancestor)ancestor[1]=0;else *slash=0;
            struct vnode ignored;
            e=resolve_access(ancestor,&ignored,depth+1,false,SANDBOX_META,NULL,false,walk);
            if(e)return e;
        }
        landlock_walk_visit(walk,&node);
    }
    size_t position=rooted?1:strlen(m->path);
    while (path[position]) {
        while (path[position] == '/')
            position++;
        size_t start = position;
        while (path[position] && path[position] != '/')
            position++;
        size_t len = position - start;
        if (!len)
            break;
        char name[OS_NAME_MAX + 1];
        memcpy(name, path + start, len);
        name[len] = 0;
        if ((node.mode & 0170000) != S_IFDIR)
            return -E_NOTDIR;
        e=credentials_node(&node,1);if(e)return e;
        e = m->ops->lookup(&node, name, &node);
        if (e)
            return e;
        if(walk)landlock_walk_visit(walk,&node);
        if(s1_fd_magic(&node) && (path[position] || (!nofollow && !fd_metadata)))return -E_ACCES;
        if ((node.mode & 0170000) == S_IFLNK && (!nofollow || path[position])) {
            if(m->ops->follow) {
                struct vnode target;
                int follow=m->ops->follow(&node,&target);
                if(!follow){
                    node=target;
                    /* A magic link is not a child of its textual directory.
                       Only an explicit target inode grant may authorize it. */
                    if(walk){landlock_walk_begin(walk);landlock_walk_visit(walk,&node);}
                    continue;
                }
                if(follow!=-E_NOTSUP)return follow;
            }
            if (!node.size || node.size >= OS_PATH_MAX || !m->ops->read)
                return -E_IO;
            char target[OS_PATH_MAX], base[OS_PATH_MAX], joined[OS_PATH_MAX], normal[OS_PATH_MAX];
            long n = m->ops->read(&node, 0, target, node.size);
            if (n != (long)node.size)
                return n < 0 ? n : -E_IO;
            target[n] = 0;
            size_t dirname = start > 1 ? start - 1 : 1;
            memcpy(base, path, dirname);
            base[dirname] = 0;
            e = snprintf(joined, sizeof(joined), "%s%s", target, path + position);
            if (e >= OS_PATH_MAX || e < 0)
                return -E_NAMETOOLONG;
            e = vfs_normalize(base, joined, normal);
            if (e)
                return e;
            return resolve_access(normal, out, depth + 1, nofollow, access, real,fd_metadata,walk);
        }
    }
    e=credentials_node(&node,0);if(e)return e;
    *out = node;
    if (real)
        strcpy(real, path);
    return 0;
}

static int resolve(const char *path,struct vnode *out,unsigned depth,bool nofollow,
                   unsigned access,char *real,bool fd_metadata) {
    return resolve_access(path,out,depth,nofollow,access,real,fd_metadata,NULL);
}

int vfs_landlock_walk(const char *path,bool nofollow,struct landlock_walk *walk,struct vnode *node) {
    char normal[OS_PATH_MAX];int error=vfs_normalize("/",path,normal);
    return error?error:resolve_access(normal,node,0,nofollow,SANDBOX_META,NULL,false,walk);
}

int vfs_lookup(const char *path, struct vnode *out) {
    char normal[OS_PATH_MAX];
    int e = vfs_normalize("/", path, normal);
    return e ? e : resolve(normal, out, 0, false, SANDBOX_META, NULL,false);
}

int vfs_lookup_stat(const char *path,struct vnode *out) {
    char normal[OS_PATH_MAX];int e=vfs_normalize("/",path,normal);
    return e?e:resolve(normal,out,0,false,SANDBOX_META,NULL,true);
}

int vfs_lstat(const char *path, struct vnode *out) {
    char normal[OS_PATH_MAX];
    int e = vfs_normalize("/", path, normal);
    return e ? e : resolve(normal, out, 0, true, SANDBOX_META, NULL,false);
}

int vfs_realpath(const char *path, char out[OS_PATH_MAX]) {
    char normal[OS_PATH_MAX];
    struct vnode node;
    int e = vfs_normalize("/", path, normal);
    return e ? e : resolve(normal, &node, 0, false, SANDBOX_META, out,false);
}

int vfs_lookup_write(const char *path, struct vnode *out) {
    char normal[OS_PATH_MAX];
    int e = vfs_normalize("/", path, normal);
    return e ? e : resolve(normal, out, 0, false, SANDBOX_WRITE, NULL,false);
}

long vfs_readlink(const char *path, void *out, size_t size) {
    struct vnode node;
    int e = vfs_lstat(path, &node);
    if (e)
        return e;
    if ((node.mode & 0170000) != S_IFLNK)
        return -E_INVAL;
    return node.mount->ops->read ? node.mount->ops->read(&node, 0, out, min(size, node.size))
                                 : -E_NOTSUP;
}

unsigned vfs_poll(struct file *f, unsigned events) {
    if(f->inotify)return inotify_poll(f,events);
    if (f->epoll)
        return epoll_poll(f, events);
    if (f->unix_socket)
        return unix_poll(f, events);
    if (f->event)
        return eventfd_poll(f, events);
    if (f->socket)
        return network_poll(f, events);
    if (f->pty)
        return pty_poll(f, events);
    if (f->pipe) {
        struct pipe_buffer *p = f->pipe;
        uint64_t irq = spin_lock_irqsave(&p->lock);
        unsigned ready = 0;
        if (f->flags & 3) {
            if (!p->readers)
                ready |= POLLERR;
            if (p->count < p->capacity)
                ready |= events & POLLOUT;
        } else {
            if (p->count || !p->writers)
                ready |= events & POLLIN;
            if (!p->writers)
                ready |= POLLHUP;
        }
        spin_unlock_irqrestore(&p->lock, irq);
        return ready;
    }
    if (f->tty)
        return ((tty_available() ? POLLIN : 0) | POLLOUT) & events;
    return events & (POLLIN | POLLOUT);
}

static int parent(const char *path, struct vnode *out, char name[OS_NAME_MAX + 1]) {
    char normal[OS_PATH_MAX];
    int e = vfs_normalize("/", path, normal);
    if (e)
        return e;
    e = sandbox_path(normal, SANDBOX_WRITE | SANDBOX_CREATE);
    if (e)
        return e;
    size_t len = strlen(normal), slash = len;
    while (slash && normal[slash - 1] != '/')
        slash--;
    if (len == 1 || !slash)
        return -E_BUSY;
    strcpy(name, normal + slash);
    normal[slash == 1 ? 1 : slash - 1] = 0;
    char real[OS_PATH_MAX], target[OS_PATH_MAX];
    e = resolve(normal, out, 0, false, SANDBOX_META, real,false);
    if (e)
        return e;
    e = vfs_normalize(real, name, target);
    if (e)
        return e;
    e = sandbox_path(target, SANDBOX_WRITE | SANDBOX_CREATE);
    if (e)
        return e;
    if ((out->mode & 0170000) != S_IFDIR)
        return -E_NOTDIR;
    e=credentials_node(out,3);if(e)return e;
    if(credentials_fs_restricted(sched_ready()?this_cpu()->current:NULL) && strcmp(out->mount->type,"ramfs") && !userns_proc_node(out))return -E_ROFS;
    return 0;
}

static bool busy(struct vnode *node) {
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (files[i].references && files[i].node.mount == node->mount &&
            files[i].node.id == node->id)
            return true;
    return false;
}

static bool mountpoint(const char *path) {
    for (unsigned i = 0; i < mount_count; i++)
        if (mounts[i].ops && !strcmp(path, mounts[i].path))
            return true;
    return false;
}

static int open_access(const char *path, unsigned flags, unsigned mode, struct file **out,bool executable) {
    if (flags & O_PATH)
        flags &= O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    if ((flags & 3) == 3 || flags & ~(3u | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_DIRECTORY |
                                      O_CLOEXEC | O_NONBLOCK | O_NOCTTY | O_LARGEFILE | O_NOFOLLOW | O_PATH))
        return -E_INVAL;

    struct file *file = NULL;
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (!files[i].references) {
            file = &files[i];
            break;
        }
    if (!file)
        return file_limit_error();

    char normal[OS_PATH_MAX], real[OS_PATH_MAX];
    int e = vfs_normalize("/", path, normal);
    if (e)
        return e;
    strcpy(real, normal);

    struct task *pty_caller=sched_ready()?this_cpu()->current:NULL;
    int private_pty=pty_private_open(pty_caller,normal,flags,out);
    if(private_pty!=1)return private_pty;

    unsigned access = (flags & O_PATH) ? SANDBOX_META :
                      (flags & 3) == O_WRONLY ? 0 : SANDBOX_READ;
    if ((flags & 3) || (flags & (O_CREAT | O_TRUNC)))
        access |= SANDBOX_WRITE;
    if (flags & O_CREAT)
        access |= SANDBOX_CREATE;

    struct vnode node;
    bool created = false;
    /* An ordinary admitted process may discard output through the real null
       device. Probe without write authority, then verify the immutable boot
       device identity; no pathname-only grant to other devices or files. */
    bool null_access = CONFIG_ELECTRON_PROFILE && pty_caller &&
        credentials_uid(pty_caller)==1000 && credentials_gid(pty_caller)==1000 &&
        !pty_caller->browser_sandbox && !pty_caller->context->web_role &&
        !pidns_active(pty_caller) && userns_admitted_account(pty_caller) &&
        (access&SANDBOX_WRITE) && !strcmp(normal,"/dev/null");
    e = resolve(normal, &node, 0, flags & O_NOFOLLOW,
                null_access ? SANDBOX_READ : access, real,false);
    if(!e && null_access && (node.mount!=&mounts[0] || node.id!=null_device ||
                            (node.mode&0170000)!=S_IFCHR))return -E_ACCES;
    if (!e && (flags & O_NOFOLLOW) && !(flags & O_PATH) && (node.mode & 0170000) == S_IFLNK)
        return -E_LOOP;
    if (e == -E_NOENT && (flags & O_CREAT)) {
        struct vnode dir;
        char name[OS_NAME_MAX + 1];
        e = parent(path, &dir, name);
        if (e)
            return e;
        if (dir.mount->readonly)
            return -E_ROFS;
        if (!dir.mount->ops->create)
            return -E_NOTSUP;
        e=landlock_parent_access(path,LL_MAKE_REG);if(e)return e;
        e = dir.mount->ops->create(&dir, name, S_IFREG | (mode & 0777), &node);
        created = !e;
        if(!e)inotify_emit(&dir,IN_CREATE,0,name);
    } else if (!e && (flags & O_CREAT) && (flags & O_EXCL))
        return -E_EXIST;
    if (e)
        return e;

    if(s1_fd_magic(&node))return -E_NOTSUP;
    bool character = (node.mode & 0170000) == S_IFCHR;
    unsigned device = character && node.mount == &mounts[0] ? node.id == null_device      ? 1
                                                              : node.id == zero_device    ? 2
                                                              : node.id == random_device  ? 3
                                                              : node.id == urandom_device ? 4
                                                                                          : 0
                                                            : 0;
    bool tty = character && !device && !(flags & O_PATH);
    struct task *console_caller=sched_ready()?this_cpu()->current:NULL;
    bool active_console=console_caller && console_caller->context && console_caller->context->account_console;
    if(credentials_fs_restricted(sched_ready()?this_cpu()->current:NULL)) {
        bool own_console=active_console && !strcmp(real,"/dev/tty");
        if((character && !device && !own_console) || (node.mode&0170000)==0060000)return -E_ACCES;
        if((flags&3) && strcmp(node.mount->type,"ramfs") && !userns_proc_node(&node))return -E_ROFS;
        if(!(flags&O_PATH) && !created) {
            e=credentials_node(&node,((flags&3)==O_WRONLY?0:4)|((flags&3)?2:0));if(e)return e;
        }
    }
    if (flags & O_PATH)
        device = 0;
    struct task *active = sched_ready() ? this_cpu()->current : NULL;
    if (active && active->browser_sandbox && (tty || (node.mode & 0170000) == 0060000))
        return -E_ACCES;
    if ((flags & O_DIRECTORY) && (node.mode & 0170000) != S_IFDIR)
        return -E_NOTDIR;
    if(!(flags&O_PATH)) {
        uint64_t rights=executable?(LL_EXECUTE|LL_READ_FILE):
            (flags&3)==O_WRONLY?0:(node.mode&0170000)==S_IFDIR?LL_READ_DIR:LL_READ_FILE;
        if(flags&3)rights|=LL_WRITE_FILE;
        if(flags&O_TRUNC)rights|=LL_TRUNCATE;
        e=landlock_path_access(real,rights,flags&O_NOFOLLOW);if(e)return e;
    }
    if ((flags & 3) && !character) {
        if (node.mount->readonly)
            return -E_ROFS;
        if ((node.mode & 0170000) == S_IFDIR)
            return -E_ISDIR;
        /* The creating descriptor may initialize a read-only file; its mode
           governs subsequent opens, just as O_CREAT's mode argument intends. */
        if (!(node.mode & 0222) && !created)
            return -E_ACCES;
    }

    if ((flags & O_TRUNC) && !character) {
        if(node.mount->readonly)return -E_ROFS;
        if((node.mode&0170000)!=S_IFREG)return -E_ISDIR;
        if(!created){e=credentials_node(&node,2);if(e)return e;}
        if (busy(&node))
            return -E_BUSY;
        if (!node.mount->ops->truncate)
            return -E_NOTSUP;
        e = node.mount->ops->truncate(&node);
        if (e)
            return e;
        inotify_emit(&node,IN_MODIFY,0,NULL);
    }

    *file = (struct file){.node = node,
                          .flags = flags & (3u | O_DIRECTORY | O_APPEND | O_NONBLOCK | O_PATH | O_NOFOLLOW),
                          .references = 1,
                          .landlock_deny_truncate = landlock_path_access(real,LL_TRUNCATE,false)==-E_ACCES,
                          .tty = tty,
                          .device = device};
    if (node.mount->ops->retain)
        node.mount->ops->retain(&file->node);
    strcpy(file->path, real);
    *out = file;
    inotify_file(file,IN_OPEN);
    if(flags&O_TRUNC)inotify_file(file,IN_MODIFY);
    return 0;
}

int vfs_open(const char *path,unsigned flags,unsigned mode,struct file **out) {
    return open_access(path,flags,mode,out,false);
}
int vfs_open_exec(const char *path,struct file **out) {
    return open_access(path,O_RDONLY,0,out,true);
}

int vfs_open_proc_node(const struct vnode *node,unsigned flags,struct file **out) {
    struct task *t=sched_ready()?this_cpu()->current:NULL;
    if(!s1_restricted(t) || !userns_proc_node(node) || s1_fd_magic(node))return -E_ACCES;
    if(flags&(3u|O_CREAT|O_TRUNC|O_APPEND))return -E_ACCES;
    if(flags&~(O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW|O_PATH|O_NONBLOCK|O_LARGEFILE))return -E_INVAL;
    if((flags&O_DIRECTORY) && (node->mode&0170000)!=S_IFDIR)return -E_NOTDIR;
    struct vnode checked=*node;int e=credentials_node(&checked,flags&O_PATH?0:4);if(e)return e;
    struct file *f=NULL;for(unsigned i=0;i<FILE_MAX;i++)if(!files[i].references){f=&files[i];break;}
    if(!f)return file_limit_error();
    *f=(struct file){.node=checked,.references=1,.flags=flags&(O_PATH|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK)};
    if(f->node.mount->ops->retain)f->node.mount->ops->retain(&f->node);
    strcpy(f->path,"[s1:owned-proc]");
    e=sandbox_file(f,SANDBOX_META|SANDBOX_READ);
    if(e){if(f->node.mount->ops->release)f->node.mount->ops->release(&f->node);memset(f,0,sizeof(*f));return e;}
    *out=f;return 0;
}

void vfs_close(struct file *f) {
    if (!f || !f->references)
        return;
    f->references--;
    if (f->references && f->references == f->watcher_references)
        epoll_forget(f);
    if (f->references)
        return;
    if(f->landlock_ruleset){landlock_ruleset_release(f->landlock_ruleset);f->landlock_ruleset=NULL;}
    if(f->inotify){inotify_release(f->inotify);f->inotify=NULL;return;}
    inotify_file(f,(f->flags&3)?IN_CLOSE_WRITE:IN_CLOSE_NOWRITE);
    if (f->epoll) {
        epoll_release(f->epoll);
        f->epoll = NULL;
        return;
    }
    if (f->unix_socket) {
        unix_close(f);
        return;
    }
    if (f->memory) {
        memfd_release(f->memory);
        f->memory = NULL;
        return;
    }
    if (f->socket) {
        network_close(f);
        return;
    }
    if (f->event) {
        eventfd_release(f->event);
        f->event = NULL;
        return;
    }
    if (f->pty) {
        pty_close(f);
        return;
    }
    if (f->pipe) {
        struct pipe_buffer *p = f->pipe;
        uint64_t irq = spin_lock_irqsave(&p->lock);
        if (f->flags & 3)
            p->writers--;
        else
            p->readers--;
        bool unused = !p->readers && !p->writers;
        spin_unlock_irqrestore(&p->lock, irq);
        f->pipe = NULL;
        if (unused) {
            kfree(p->bytes);
            kfree(p);
        }
        return;
    }
    if ((f->flags & 3) && !f->tty && f->node.mount && f->node.mount->ops->sync)
        f->node.mount->ops->sync(f->node.mount);
    if (f->node.mount && f->node.mount->ops->release)
        f->node.mount->ops->release(&f->node);
}

long vfs_read(struct file *f, void *data, size_t size) {
    if(f && f->landlock_ruleset)return -E_INVAL;
    if (!f || !f->references || (f->flags & O_PATH) || (f->flags & 3) == O_WRONLY)
        return -E_BADF;
    int permission = sandbox_file(f, SANDBOX_READ);
    if (permission)
        return permission;
    if (f->memory)
        return memfd_transfer(f, data, size, false);
    if (f->socket || f->unix_socket || f->event || f->epoll || f->inotify)
        return -E_INVAL; /* Blocking endpoints never hold vfs_mutex. */
    if (f->device) {
        if (f->device == 1)
            return 0;
        if (f->device >= 3)
            return random_bytes(data, size);
        memset(data, 0, size);
        return size;
    }
    if ((f->node.mode & 0170000) == S_IFDIR)
        return -E_ISDIR;
    if (!f->node.mount->ops->read)
        return -E_NOTSUP;
    if (f->node.mount->ops->stat) {
        int e = f->node.mount->ops->stat(&f->node);
        if (e)
            return e;
    }
    long n = f->node.mount->ops->read(&f->node, f->offset, data, size);
    if (n > 0) {
        f->offset += n;
        inotify_file(f,IN_ACCESS);
    }
    return n;
}

long vfs_write(struct file *f, const void *data, size_t size) {
    if(f && f->landlock_ruleset)return -E_INVAL;
    if (!f || !f->references || (f->flags & O_PATH) || !(f->flags & 3))
        return -E_BADF;
    int permission = sandbox_file(f, SANDBOX_WRITE);
    if (permission)
        return permission;
    if (f->memory)
        return memfd_transfer(f, (void *)data, size, true);
    if (f->socket || f->unix_socket || f->event || f->epoll || f->inotify)
        return -E_INVAL;
    if (f->device)
        return f->device >= 3 ? -E_NOTSUP : (long)size;
    if (f->node.mount->readonly)
        return -E_ROFS;
    if (!f->node.mount->ops->write)
        return -E_NOTSUP;
    if (f->node.mount->ops->stat) {
        int e = f->node.mount->ops->stat(&f->node);
        if (e)
            return e;
    }
    int modified=credentials_modified(&f->node);if(modified)return modified;
    if (f->flags & O_APPEND)
        f->offset = f->node.size;
    long n = f->node.mount->ops->write(&f->node, f->offset, data, size);
    if (n > 0) {
        f->offset += n;
        inotify_file(f,IN_MODIFY);
    }
    return n;
}

int vfs_socket_node_mode(const char *path, unsigned mode, struct vnode *out) {
    struct vnode dir;char name[OS_NAME_MAX+1];
    int error=parent(path,&dir,name);
    if(error)return error;
    if(dir.mount->readonly)return -E_ROFS;
    error=landlock_parent_access(path,LL_MAKE_SOCK);if(error)return error;
    if(strcmp(dir.mount->type,"ramfs"))return -E_NOTSUP;
    error=dir.mount->ops->create?dir.mount->ops->create(&dir,name,S_IFSOCK|(mode&0777),out):-E_NOTSUP;
    if(!error)inotify_emit(&dir,IN_CREATE,0,name);
    return error;
}
int vfs_socket_node(const char *path, struct vnode *out) {
    return vfs_socket_node_mode(path,0600,out);
}

int vfs_mkdir(const char *path, unsigned mode) {
    if (!strcmp(path, "/"))
        return -E_EXIST;
    /* Linux reports an existing entry before checking whether its parent is
       writable. This also lets mkdir -p traverse root-owned ancestors. */
    struct vnode existing;
    int found=vfs_lstat(path,&existing);
    if(!found)return -E_EXIST;
    if(found!=-E_NOENT)return found;
    struct vnode dir;
    char name[OS_NAME_MAX + 1];
    int e = parent(path, &dir, name);
    if (e)
        return e;
    if (dir.mount->readonly)
        return -E_ROFS;
    e=landlock_parent_access(path,LL_MAKE_DIR);if(e)return e;
    e=dir.mount->ops->create?dir.mount->ops->create(&dir,name,S_IFDIR|(mode&01777),&(struct vnode){0}):-E_NOTSUP;
    if(!e)inotify_emit(&dir,IN_CREATE|IN_ISDIR,0,name);
    return e;
}

int vfs_symlink(const char *target, const char *path) {
    struct vnode dir;
    char name[OS_NAME_MAX + 1];
    int e = parent(path, &dir, name);
    if (e)
        return e;
    if (dir.mount->readonly)
        return -E_ROFS;
    if (!target[0])
        return -E_NOENT;
    e=landlock_parent_access(path,LL_MAKE_SYM);if(e)return e;
    e=dir.mount->ops->symlink?dir.mount->ops->symlink(&dir,name,target):-E_NOTSUP;
    if(!e)inotify_emit(&dir,IN_CREATE,0,name);
    return e;
}

/* Reconstruct the canonical dentry path while preserving the existing
   parent/sandbox resolution checks. Caller owns vfs_mutex. */
static int canonical_child(const char *path, const char *name, char out[OS_PATH_MAX]) {
    char parent_path[OS_PATH_MAX];
    size_t n = strlen(path);
    while (n && path[n-1] != '/') n--;
    if (!n) return -E_INVAL;
    if (n == 1) strcpy(parent_path,"/");
    else {memcpy(parent_path,path,n-1);parent_path[n-1]=0;}
    int error = vfs_realpath(parent_path,out);
    if (error) return error;
    n = strlen(out);
    size_t extra = strlen(name);
    if (n + (n>1) + extra >= OS_PATH_MAX) return -E_NAMETOOLONG;
    if (n>1) out[n++]='/';
    memcpy(out+n,name,extra+1);
    return 0;
}
static bool named_description(const struct file *f,const struct vnode *node,const char *path) {
    return f->references && !f->unlinked && f->node.mount==node->mount &&
           f->node.id==node->id && !strcmp(f->path,path);
}
static void descriptions_unlinked(const struct vnode *node,const char *path) {
    for (unsigned i=0;i<FILE_MAX;i++)
        if (named_description(&files[i],node,path)) files[i].unlinked=true;
}

/* Shared temporary directories allow creation but retain each entry's owner. */
static int sticky_entry(const struct vnode *dir,const struct vnode *node) {
    struct task *t=sched_ready()?this_cpu()->current:NULL;
    unsigned uid=credentials_fsuid(t);
    return (dir->mode&01000) && uid && uid!=dir->uid && uid!=node->uid ? -E_PERM : 0;
}

int vfs_unlink(const char *path, bool directory) {
    if (mountpoint(path))
        return -E_BUSY;
    if (directory && sched_path_busy(path))
        return -E_BUSY;
    struct vnode dir, node;
    char name[OS_NAME_MAX + 1];
    int e = parent(path, &dir, name);
    if (e)
        return e;
    if (dir.mount->readonly)
        return -E_ROFS;
    e = dir.mount->ops->lookup(&dir, name, &node);
    if (e)
        return e;
    e=sticky_entry(&dir,&node);if(e)return e;
    e=landlock_parent_access(path,directory?LL_REMOVE_DIR:LL_REMOVE_FILE);if(e)return e;
    if (busy(&node) && (directory || !dir.mount->ops->release))
        return -E_BUSY;
    char real[OS_PATH_MAX];
    bool retained=busy(&node);
    if (retained && (e=canonical_child(path,name,real))) return e;
    e=dir.mount->ops->unlink ? dir.mount->ops->unlink(&dir,name,directory) : -E_NOTSUP;
    if (!e && retained) descriptions_unlinked(&node,real);
    if(!e) {
        inotify_emit(&dir,IN_DELETE|(directory?IN_ISDIR:0),0,name);
        inotify_emit(&node,node.nlink<=1 || directory?IN_DELETE_SELF:IN_ATTRIB,0,NULL);
    }
    return e;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    return vfs_rename_flags(oldpath, newpath, false);
}

int vfs_rename_flags(const char *oldpath, const char *newpath, bool no_replace) {
    if (!strcmp(oldpath, newpath))
        return no_replace ? -E_EXIST : 0;
    if (mountpoint(oldpath) || mountpoint(newpath))
        return -E_BUSY;
    for (unsigned i = 1; i < mount_count; i++)
        if (mounts[i].ops && prefix(mounts[i].path, oldpath))
            return -E_BUSY;
    struct vnode old, new, node;
    char a[OS_NAME_MAX + 1], b[OS_NAME_MAX + 1];
    int e = parent(oldpath, &old, a);
    if (e)
        return e;
    e = parent(newpath, &new, b);
    if (e)
        return e;
    if (old.mount != new.mount)
        return -E_XDEV;
    if (old.mount->readonly)
        return -E_ROFS;
    e = old.mount->ops->lookup(&old, a, &node);
    if (e)
        return e;
    e=sticky_entry(&old,&node);if(e)return e;
    if (busy(&node) && ((node.mode & 0170000) == S_IFDIR || !old.mount->ops->release))
        return -E_BUSY;
    if ((node.mode & 0170000) == S_IFDIR && prefix(newpath, oldpath))
        return -E_INVAL;
    if ((node.mode & 0170000) == S_IFDIR) {
        if (sched_path_busy(oldpath))
            return -E_BUSY;
        for (unsigned i = 0; i < FILE_MAX; i++)
            if (files[i].references && prefix(files[i].path, oldpath))
                return -E_BUSY;
    }
    struct vnode target;
    e = old.mount->ops->lookup(&new, b, &target);
    if (!e) {
        if (no_replace)
            return -E_EXIST;
        e=sticky_entry(&new,&target);if(e)return e;
        if ((busy(&target) && ((target.mode & 0170000) == S_IFDIR || !old.mount->ops->release)) ||
            ((target.mode & 0170000) == S_IFDIR && sched_path_busy(newpath)))
            return -E_BUSY;
    } else if (e != -E_NOENT)
        return e;
    bool target_exists=!e;
    if (target_exists && target.id==node.id) return 0; /* same inode hard-link no-op */
    e=landlock_parent_access(oldpath,(node.mode&0170000)==S_IFDIR?LL_REMOVE_DIR:LL_REMOVE_FILE);
    if(!e)e=landlock_parent_access(newpath,landlock_make_access(&node));
    if(!e && target_exists)e=landlock_parent_access(newpath,(target.mode&0170000)==S_IFDIR?LL_REMOVE_DIR:LL_REMOVE_FILE);
    if(!e)e=landlock_reparent(oldpath,newpath,&node);
    if(e)return e;
    bool retained=busy(&node) || (target_exists && busy(&target));
    char old_real[OS_PATH_MAX],new_real[OS_PATH_MAX];
    if (retained) {
        e=canonical_child(oldpath,a,old_real);
        if (!e) e=canonical_child(newpath,b,new_real);
        if (e) return e;
    }
    e=old.mount->ops->rename ? old.mount->ops->rename(&old,a,&new,b) : -E_NOTSUP;
    if (!e && retained) {
        if (target_exists) descriptions_unlinked(&target,new_real);
        for (unsigned i=0;i<FILE_MAX;i++)
            if (named_description(&files[i],&node,old_real)) strcpy(files[i].path,new_real);
    }
    if(!e) {
        unsigned type=(node.mode&0170000)==S_IFDIR?IN_ISDIR:0,cookie=inotify_cookie();
        if(target_exists)inotify_emit(&target,target.nlink<=1?IN_DELETE_SELF:IN_ATTRIB,0,NULL);
        inotify_emit(&old,IN_MOVED_FROM|type,cookie,a);
        inotify_emit(&new,IN_MOVED_TO|type,cookie,b);
        inotify_emit(&node,IN_MOVE_SELF|type,0,NULL);
    }
    return e;
}

int vfs_link_node(const struct vnode *node, const char *path) {
    struct vnode fresh=*node;int owner=credentials_owner(&fresh);if(owner)return owner;
    if (!node->mount)
        return -E_XDEV;
    if ((node->mode & 0170000) == S_IFDIR)
        return -E_PERM;
    struct vnode dir;
    char name[OS_NAME_MAX + 1];
    int e = parent(path, &dir, name);
    if (e)
        return e;
    if (dir.mount != node->mount)
        return -E_XDEV;
    if (dir.mount->readonly)
        return -E_ROFS;
    e=landlock_parent_access(path,landlock_make_access(node));if(e)return e;
    e=dir.mount->ops->link?dir.mount->ops->link(node,&dir,name):-E_NOTSUP;
    if(!e){inotify_emit(&dir,IN_CREATE,0,name);inotify_emit(node,IN_ATTRIB,0,NULL);}
    return e;
}

/* Caller owns vfs_mutex. Locks belong to an open file description and survive
   dup/fork; its final close removes it from this live-description inventory. */
int vfs_flock(struct file *file, unsigned operation) {
    if (!file->node.mount || (file->flags & O_PATH))
        return -E_BADF;
    if (operation == 8) {
        file->flock_mode = 0;
        return 0;
    }
    if (file->flock_mode == operation)
        return 0;
    file->flock_mode = 0; /* Linux conversion is not atomic. */
    for (unsigned i = 0; i < FILE_MAX; i++) {
        struct file *other = &files[i];
        if (other != file && other->references && other->flock_mode &&
            other->node.mount == file->node.mount && other->node.id == file->node.id &&
            (operation == 2 || other->flock_mode == 2))
            return -E_AGAIN;
    }
    file->flock_mode = operation;
    return 0;
}

#include "record_lock.inc"

int vfs_chmod(const char *path, unsigned mode) {
    struct vnode node;
    int e = vfs_lookup_write(path, &node);
    if (e)
        return e;
    if (node.mount->readonly)
        return -E_ROFS;
    e=credentials_owner(&node);if(e)return e;
    e=node.mount->ops->chmod?node.mount->ops->chmod(&node,credentials_mode(&node,mode)):-E_NOTSUP;
    if(!e) {
        struct file changed={.node=node};strcpy(changed.path,path);inotify_file(&changed,IN_ATTRIB);
    }
    return e;
}

int vfs_readdir(struct file *f, struct directory_entry *entry) {
    if (!f || !f->references || (f->flags & O_PATH))
        return -E_BADF;
    int permission = sandbox_file(f, SANDBOX_READ);
    if (permission)
        return permission;
    if ((f->node.mode & 0170000) != S_IFDIR)
        return -E_NOTDIR;
    return f->node.mount->ops->readdir ? f->node.mount->ops->readdir(&f->node, &f->offset, entry)
                                       : -E_NOTDIR;
}

int vfs_sync(void) {
    int result = 0;
    for (unsigned i = 0; i < mount_count; i++)
        if (mounts[i].ops && mounts[i].ops->sync) {
            int e = mounts[i].ops->sync(&mounts[i]);
            if (e)
                result = e;
        }
    return result;
}

size_t vfs_mounts_text(char *out, size_t cap) {
    size_t used = 0;
    for (unsigned i = 0; i < mount_count && used < cap; i++) {
        struct mount *m = &mounts[i];
        if (!m->ops)
            continue;
        int n = snprintf(out + used, cap - used, "%s %s %s %s\n", m->path, m->type,
                         m->readonly ? "ro" : "rw", m->device ? m->device->name : "memory");
        used += min((size_t)max(n, 0), cap - used);
    }
    return min(used, cap ? cap - 1 : 0);
}

size_t vfs_lsblk_text(char *out, size_t cap) {
    size_t used = 0;
    int n = snprintf(out, cap, "NAME TYPE BYTES SECTOR RO FSTYPE MOUNTPOINT\n");
    used = min((size_t)max(n, 0), cap);
    for (unsigned i = 0; i < block_count && used < cap; i++) {
        struct block_device *d = &block_devices[i];
        const struct mount *mounted = NULL;
        for (unsigned j = 1; j < mount_count; j++)
            if (mounts[j].ops && mounts[j].device == d)
                mounted = &mounts[j];
        n = snprintf(out + used, cap - used, "%s%s %s %llu %u %u %s %s\n", d->parent ? "  " : "",
                     d->name, d->parent ? "part" : "disk",
                     (unsigned long long)(d->sectors * d->sector_size), d->sector_size,
                     mounted ? mounted->readonly : d->readonly, mounted ? mounted->type : "-",
                     mounted ? mounted->path : "-");
        used += min((size_t)max(n, 0), cap - used);
    }
    return min(used, cap ? cap - 1 : 0);
}

/* Mount slots never move: vnodes and open files retain pointers into this array. */
int vfs_mount_device(const char *source, const char *target, const char *type, unsigned flags) {
    if (flags & ~MS_RDONLY)
        return -E_INVAL;
    if (strcmp(type, "auto") && strcmp(type, "fat32") && strcmp(type, "ext") &&
        strcmp(type, "ext2") && strcmp(type, "ext4"))
        return -E_NOTSUP;
    if (!strncmp(source, "/dev/", 5))
        source += 5;
    struct block_device *device = NULL;
    for (unsigned i = 0; i < block_count; i++)
        if (!strcmp(source, block_devices[i].name))
            device = &block_devices[i];
    if (!device)
        return -E_NOENT;
    for (unsigned i = 0; i < block_count; i++)
        if (block_devices[i].parent == device)
            return -E_BUSY;
    for (unsigned i = 1; i < mount_count; i++)
        if (mounts[i].ops && mounts[i].device == device)
            return -E_BUSY;
    if (mountpoint(target))
        return -E_BUSY;
    struct vnode node;
    int e = vfs_lookup(target, &node);
    if (e)
        return e;
    if ((node.mode & 0170000) != S_IFDIR)
        return -E_NOTDIR;
    if (node.mount != &mounts[0] || sched_path_busy(target))
        return -E_BUSY;
    if (busy(&node))
        return -E_BUSY;
    uint64_t offset = 0;
    struct directory_entry entry;
    while ((e = node.mount->ops->readdir(&node, &offset, &entry)) > 0)
        if (strcmp(entry.name, ".") && strcmp(entry.name, ".."))
            return -E_NOTEMPTY;
    if (e < 0)
        return e;
    unsigned slot = 1;
    while (slot < mount_count && mounts[slot].ops)
        slot++;
    if (slot == MOUNT_MAX)
        return -E_NOSPC;
    struct mount *m = &mounts[slot];
    memset(m, 0, sizeof(*m));
    m->device = device;
    if (!strcmp(type, "auto") || !strcmp(type, "fat32"))
        e = fat32_mount(m);
    else
        e = -E_NOENT;
    if (e == -E_NOENT && strcmp(type, "fat32"))
        e = ext_mount(m);
    if (e) {
        memset(m, 0, sizeof(*m));
        return e;
    }
    m->readonly = m->readonly || device->readonly || (flags & MS_RDONLY);
    strcpy(m->path, target);
    if (slot == mount_count)
        mount_count++;
    return 0;
}

int vfs_unmount(const char *target, unsigned flags) {
    if (flags)
        return -E_INVAL;
    if (!strcmp(target, "/"))
        return -E_BUSY;
    struct mount *m = NULL;
    for (unsigned i = 1; i < mount_count; i++)
        if (mounts[i].ops && !strcmp(target, mounts[i].path))
            m = &mounts[i];
    if (!m)
        return -E_INVAL;
    if(m->security_references)return -E_BUSY;
    for (unsigned i = 0; i < FILE_MAX; i++)
        if (files[i].references && files[i].node.mount == m)
            return -E_BUSY;
    if (sched_path_busy(target) || ramfs_shared_mount_busy(m) || inotify_mount_busy(m))
        return -E_BUSY;
    int e = m->ops->sync ? m->ops->sync(m) : 0;
    if (e)
        return e;
    /* Disk filesystem contexts own a single allocation; root ramfs is permanent. */
    kfree(m->data);
    memset(m, 0, sizeof(*m));
    return 0;
}

static unsigned octal(const uint8_t *s, size_t n) {
    unsigned v = 0;
    for (size_t i = 0; i < n && s[i]; i++)
        if (s[i] >= '0' && s[i] <= '7')
            v = v * 8 + s[i] - '0';
    return v;
}

static void unpack(const struct boot_module *module) {
    const uint8_t *p = module->address;
    uint64_t offset = 0;
    while (offset + 512 <= module->size && p[offset]) {
        const uint8_t *h = p + offset;
        uint64_t size = octal(h + 124, 12), checksum = 0;
        for (unsigned i = 0; i < 512; i++)
            checksum += (i >= 148 && i < 156) ? ' ' : h[i];
        if (checksum != octal(h + 148, 8) || size > module->size - offset - 512)
            panic("invalid initramfs tar archive");
        char path[OS_PATH_MAX], raw[OS_PATH_MAX];
        /* USTAR permits a completely filled name/prefix field without NUL.
         * Copy fixed-width fields before passing them to string routines. */
        size_t name_len = 0, prefix_len = 0, used = 0;
        while (name_len < 100 && h[name_len])
            name_len++;
        while (prefix_len < 155 && h[345 + prefix_len])
            prefix_len++;
        if (!name_len || 1 + prefix_len + (prefix_len ? 1 : 0) + name_len >= sizeof(raw))
            panic("invalid initramfs pathname");
        raw[used++] = '/';
        if (prefix_len) {
            memcpy(raw + used, h + 345, prefix_len);
            used += prefix_len;
            raw[used++] = '/';
        }
        memcpy(raw + used, h, name_len);
        raw[used + name_len] = 0;
        int e = vfs_normalize("/", raw, path);
        if (e)
            panic("invalid initramfs pathname");
        unsigned mode = octal(h + 100, 8);
        /* Later trusted boot modules are filesystem overlays. Replace the
           leaf itself rather than following a symlink left by an earlier
           module (GNU multicall links can replace old standalone tools). */
        if(h[156]=='2' || h[156]=='0' || !h[156]) {
            struct vnode previous;
            e=vfs_lstat(path,&previous);
            if(!e) {
                if((previous.mode&0170000)==S_IFDIR)panic("initramfs replaces directory %s",path);
                e=vfs_unlink(path,false);
            }
            if(e && e!=-E_NOENT)panic("initramfs replace %s: %d",path,e);
        }
        if (h[156] == '5') {
            if (strcmp(path, "/")) {
                e = vfs_mkdir(path, mode);
                if (e && e != -E_EXIST)
                    panic("initramfs mkdir %s: %d", path, e);
            }
        } else if (h[156] == '2') {
            char target[101];
            size_t length = 0;
            while (length < 100 && h[157 + length])
                length++;
            memcpy(target, h + 157, length);
            target[length] = 0;
            e = vfs_symlink(target, path);
            if (e)
                panic("initramfs symlink %s: %d", path, e);
        } else if (h[156] == '0' || !h[156]) {
            struct file *file;
            e = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, mode, &file);
            if (e)
                panic("initramfs open %s: %d", path, e);
            e = ramfs_import(&file->node, h + 512, size);
            if(!e)e=mounts[0].ops->chmod(&file->node,mode&07777);
            vfs_close(file);
            if (e)
                panic("initramfs import failed: %d", e);
        }
        if(h[156]=='5' || h[156]=='2' || h[156]=='0' || !h[156]) {
            struct vnode node;
            e=vfs_lstat(path,&node);
            if(!e && node.mount->ops->chown)e=node.mount->ops->chown(&node,octal(h+108,8),octal(h+116,8));
            if(!e && h[156]!='2' && node.mount->ops->chmod)e=node.mount->ops->chmod(&node,mode&07777);
            if(!e && node.mount->ops->set_times)e=node.mount->ops->set_times(&node,octal(h+136,12),octal(h+136,12),3);
            if(e)panic("initramfs metadata %s: %d",path,e);
        }
        offset += 512 + ((size + 511) & ~511ULL);
    }
}

void vfs_init(const struct boot_module *modules, uint64_t module_count) {
    strcpy(mounts[0].path, "/");
    if (ramfs_mount(&mounts[0]))
        panic("ramfs allocation failed");
    mount_count = 1;

    vfs_mkdir("/bin", 0755);
    vfs_mkdir("/tmp", 01777);
    vfs_mkdir("/mnt", 0755);
    vfs_mkdir("/dev", 0755);

    struct vnode dev;
    vfs_lookup("/dev", &dev);
    mounts[0].ops->create(&dev, "tty", S_IFCHR | 0666, &(struct vnode){0});
    mounts[0].ops->create(&dev, "console", S_IFCHR | 0666, &(struct vnode){0});

    struct vnode node;
    if (mounts[0].ops->create(&dev, "null", S_IFCHR | 0666, &node))
        panic("create /dev/null");
    null_device = node.id;
    if (mounts[0].ops->create(&dev, "zero", S_IFCHR | 0666, &node))
        panic("create /dev/zero");
    zero_device = node.id;
    if (mounts[0].ops->create(&dev, "random", S_IFCHR | 0444, &node))
        panic("create /dev/random");
    random_device = node.id;
    if (mounts[0].ops->create(&dev, "urandom", S_IFCHR | 0444, &node))
        panic("create /dev/urandom");
    urandom_device = node.id;

    if (modules)
        for (uint64_t i = 0; i < module_count; i++)
            if (modules[i].string && !strcmp(modules[i].string, "initramfs"))
                unpack(&modules[i]);
    if(CONFIG_USERNS_TEST) {
        vfs_mkdir("/proc",0555);
        if(userns_proc_mount(&mounts[mount_count]))mount_count++;
    }
    /* Read-only hardware metadata, populated from the kernel's calibration.
       Timing libraries can reuse it instead of calibrating in every process. */
    if (mount_count < MOUNT_MAX) {
        int error=vfs_mkdir("/sys",0555);
        struct vnode sys;
        if ((!error || error==-E_EXIST) && !vfs_lstat("/sys",&sys) &&
            (sys.mode&0170000)==S_IFDIR && !sysfs_mount(&mounts[mount_count]))
            mount_count++;
    }
}

void vfs_discover(void) {
    block_partitions();

    for (unsigned i = 0; i < block_count && mount_count < MOUNT_MAX; i++) {
        struct block_device *d = &block_devices[i];
        bool children = false;
        for (unsigned j = 0; j < block_count; j++)
            if (block_devices[j].parent == d)
                children = true;
        if (children)
            continue;

        struct mount *m = &mounts[mount_count];
        memset(m, 0, sizeof(*m));
        m->device = d;
        int e = fat32_mount(m);
        if (e == -E_NOENT)
            e = ext_mount(m);
        if (e) {
            if (e != -E_NOENT)
                pr_warn("%s: filesystem mount failed (%d)\n", d->name, e);
            continue;
        }

        snprintf(m->path, sizeof(m->path), "/mnt/%s", d->name);
        e = vfs_mkdir(m->path, 0755);
        if (e && e != -E_EXIST)
            panic("mountpoint creation failed");
        mount_count++;
        pr_info("VFS: %s mounted at %s (%s)\n", m->type, m->path,
                m->readonly ? "read only" : "read/write");
    }
}

bool vfs_power_ready(void) {
    for (unsigned i = 0; i < mount_count; i++) {
        struct mount *m = &mounts[i];
        if (!m->ops || m->readonly || !m->device)
            continue;
        struct block_device *d = m->device;
        while (d->parent)
            d = d->parent;
        if (!m->ops->sync || !d->flush)
            return false;
    }
    return true;
}
