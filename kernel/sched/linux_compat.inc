/* Bounded Linux compatibility: actual upstream GNU/util-linux regressions.
   Included after syscall helpers; no application-specific return shortcuts. */
#include <uapi/linux_compat.h>

static uint64_t node_links(const struct vnode *node) {
    return node->mount && node->mount->ops->retain ? node->nlink : max(node->nlink, 1U);
}

static struct os_stat node_stat(const struct vnode *node) {
    struct task *caller=sched_ready()?this_cpu()->current:NULL;
    return (struct os_stat){.ino = userns_proc_node(node) || (node->mount && node->mount->device) ? node->id : node->aux[3],
                            .mode = node->mode,
                            .uid = userns_uid_view(caller,node->uid),
                            .gid = userns_gid_view(caller,node->gid),
                            .nlink = node_links(node),
                            .size = node->size,
                            .blksize = 4096,
                            .blocks = (node->size + 511) / 512,
                            .dev = node->mount && node->mount->device
                                       ? (uint64_t)(node->mount->device - block_devices) + 1
                                       : 0,
                            .atime_sec = node->atime,
                            .mtime_sec = node->mtime,
                            .ctime_sec = node->ctime};
}

/* Caller owns the process lock and VFS mutex. Empty paths resolve the actual
   descriptor's retained inode, including unlinked files and O_PATH symlinks. */
static int node_at(struct task *t, int fd, uint64_t ptr, unsigned flags, bool write,
                   struct vnode *node) {
    char raw[OS_PATH_MAX], path[OS_PATH_MAX];
    int e = user_string(&t->space, raw, ptr, sizeof(raw));
    if (e)
        return e;
    if (!raw[0]) {
        if (!(flags & AT_EMPTY_PATH))
            return -E_NOENT;
        if (fd == AT_FDCWD && s1_cwd_check(t))return -E_NOENT;
        if (fd == AT_FDCWD)
            return write ? vfs_lookup_write(t->cwd, node) : vfs_lookup(t->cwd, node);
        if (fd < 0 || fd >= OS_FD_MAX || !t->fds[fd])
            return -E_BADF;
        struct file *f = t->fds[fd];
        e = sandbox_file(f, write ? SANDBOX_WRITE : SANDBOX_META);
        if (e)
            return e;
        *node = f->node;
        if (f->memory) {
            node->size = memfd_size(f->memory);
            return 0;
        }
        return node->mount && node->mount->ops->stat ? node->mount->ops->stat(node) : 0;
    }
    if(!write) {
        e=s1_at_lookup(t,fd,raw,flags&AT_SYMLINK_NOFOLLOW,true,node);
        if(e<=0)return e;
    }
    e = path_at(t, fd, ptr, path);
    if (e)
        return e;
    if (write) {
        e = sandbox_path(path, SANDBOX_WRITE);
        if (e)
            return e;
    }
    if (flags & AT_SYMLINK_NOFOLLOW)
        return vfs_lstat(path, node);
    return write ? vfs_lookup_write(path, node) : vfs_lookup_stat(path, node);
}

static long linux_metadata(struct task *t, register_t *r) {
    unsigned flags = r->rdx;
    uint32_t mask = 0x3ff;
    struct vnode node;
    long e;
    mutex_lock(&vfs_mutex);
    e = node_at(t, (int)r->rdi, r->rsi, flags, false, &node);
    struct os_stat st = {0};
    if (!e) {
        st = node_stat(&node);
        if (node.mount && !node.mount->ops->retain && !node.nlink)
            mask &= ~4u; /* FAT link counts are not supplied by this driver. */
    }
    mutex_unlock(&vfs_mutex);
    if (e)
        return e;
    struct os_statx x = {.mask = mask,
                         .blksize = 4096,
                         .nlink = st.nlink,
                         .uid = st.uid,
                         .gid = st.gid,
                         .mode = st.mode,
                         .ino = st.ino,
                         .size = st.size,
                         .atime = {.sec = st.atime_sec},
                         .ctime = {.sec = st.ctime_sec},
                         .mtime = {.sec = st.mtime_sec},
                         .dev_minor = st.dev};
    /* No invented birth times, allocation counts, mount IDs or attributes. */
    return user_copy_out(&t->space, r->r8, &x, sizeof(x)) ? 0 : -E_FAULT;
}

static long linux_permissions(struct task *t, register_t *r) {
    bool access = r->rax == SYS_faccessat2;
    unsigned flags = r->rax == SYS_fchmodat ? 0 : r->r10;
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | (access ? 0x200 : 0)))
        return -E_INVAL;
    if (!access && t->userns && (r->rdx & 07000))
        return -E_NOTSUP; /* The confined namespace does not grant set-ID policy. */
    if (access && ((unsigned)r->rdx & ~7u))
        return -E_INVAL;
    struct vnode node;
    unsigned saved_fsuid=t->context->fsuid,saved_fsgid=t->context->fsgid,saved_scope=t->context->desktop_fsuid;
    if(access && !(flags&0x200) && !t->userns) {t->context->fsuid=credentials_ruid(t);t->context->fsgid=credentials_rgid(t);t->context->desktop_fsuid=0;}
    mutex_lock(&vfs_mutex);
    long e = node_at(t, (int)r->rdi, r->rsi, flags, !access || (r->rdx & 2), &node);
    if(!e && !access)e=credentials_owner(&node);
    if (!e && !access) {
        if ((node.mode & 0170000) == S_IFLNK)
            e = -E_NOTSUP;
        else if (!node.mount)
            e = -E_NOTSUP;
        else if (node.mount->readonly)
            e = -E_ROFS;
        else
            e = node.mount->ops->chmod ? node.mount->ops->chmod(&node, credentials_mode(&node,r->rdx)) : -E_NOTSUP;
        if(!e)inotify_emit(&node,IN_ATTRIB,0,NULL);
    } else if (!e) {
        unsigned mode = r->rdx;
        e=credentials_node(&node,mode);
        if(!e && (mode&2) && node.mount && node.mount->readonly)e=-E_ROFS;
        if(!e && (mode&1) && (node.mode&0170000)==S_IFREG && !(node.mode&0111))e=-E_ACCES;
    }
    mutex_unlock(&vfs_mutex);
    t->context->fsuid=saved_fsuid;t->context->fsgid=saved_fsgid;t->context->desktop_fsuid=saved_scope;
    return e;
}

static long linux_link(struct task *t, register_t *r) {
    bool at = r->rax == SYS_linkat;
    unsigned flags = at ? r->r8 : 0;
    if (flags & ~(AT_EMPTY_PATH | 0x400)) /* AT_SYMLINK_FOLLOW */
        return -E_INVAL;
    char dest[OS_PATH_MAX];
    int e = path_at(t, at ? (int)r->rdx : AT_FDCWD, at ? r->r10 : r->rsi, dest);
    if (e)
        return e;
    struct vnode node;
    mutex_lock(&vfs_mutex);
    e = node_at(t, at ? (int)r->rdi : AT_FDCWD, at ? r->rsi : r->rdi,
                (flags & AT_EMPTY_PATH) | ((flags & 0x400) ? 0 : AT_SYMLINK_NOFOLLOW), false,
                &node);
    if (!e && !node_links(&node))
        e = -E_NOENT; /* An already unlinked inode cannot be resurrected. */
    if(!e && landlock_active()) {
        char source[OS_PATH_MAX],first;
        uint64_t argument=at?r->rsi:r->rdi;
        if(!user_copy_in(&t->space,&first,argument,1))e=-E_FAULT;
        else if(!first && at && (flags&AT_EMPTY_PATH)) {
            int fd=(int)r->rdi;
            if(fd<0 || fd>=OS_FD_MAX || !t->fds[fd] || t->fds[fd]->unlinked)e=-E_NOENT;
            else strcpy(source,t->fds[fd]->path);
        } else e=path_at(t,at?(int)r->rdi:AT_FDCWD,argument,source);
        if(!e) {
            struct vnode checked;
            e=(flags&0x400)?vfs_lookup(source,&checked):vfs_lstat(source,&checked);
            if(!e && (checked.mount!=node.mount || checked.id!=node.id ||
               checked.aux[0]!=node.aux[0] || checked.aux[2]!=node.aux[2] || checked.aux[3]!=node.aux[3]))e=-E_ACCES;
            if(!e && (flags&0x400)) {
                char real[OS_PATH_MAX];e=vfs_realpath(source,real);if(!e)strcpy(source,real);
            }
        }
        if(!e)e=landlock_reparent(source,dest,&node);
    }
    if (!e)
        e = vfs_link_node(&node, dest);
    mutex_unlock(&vfs_mutex);
    return e;
}

static long linux_chown(struct task *t, register_t *r) {
    bool at = r->rax == SYS_fchownat, fd = r->rax == SYS_fchown;
    unsigned flags = at ? r->r8 : r->rax == SYS_lchown ? AT_SYMLINK_NOFOLLOW : 0;
    if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
        return -E_INVAL;
    struct vnode node;
    uint32_t uid = at ? r->rdx : r->rsi, gid = at ? r->r10 : r->rdx;
    long e = 0;
    mutex_lock(&vfs_mutex);
    if (fd) {
        unsigned descriptor = r->rdi;
        if (descriptor >= OS_FD_MAX || !t->fds[descriptor] || (t->fds[descriptor]->flags & O_PATH))
            e = -E_BADF;
        else {
            e = sandbox_file(t->fds[descriptor], SANDBOX_WRITE);
            node = t->fds[descriptor]->node;
        }
    } else
        e = node_at(t, at ? (int)r->rdi : AT_FDCWD, at ? r->rsi : r->rdi, flags, true, &node);
    if (!e && !node.mount)
        e = -E_NOTSUP;
    if (!e && node.mount->readonly)
        e = -E_ROFS;
    if(!e)e=credentials_chown(&node,uid,gid);
    if (!e)
        e = node.mount->ops->chown ? node.mount->ops->chown(&node, uid, gid) : -E_NOTSUP;
    if(!e)inotify_emit(&node,IN_ATTRIB,0,NULL);
    mutex_unlock(&vfs_mutex);
    return e;
}

static long linux_statfs(struct task *t, register_t *r) {
    struct vnode node;
    long e = 0;
    struct os_statfs st = {0};
    mutex_lock(&vfs_mutex);
    if (r->rax == SYS_fstatfs) {
        unsigned descriptor = r->rdi;
        if (descriptor >= OS_FD_MAX || !t->fds[descriptor])
            e = -E_BADF;
        else {
            e = sandbox_file(t->fds[descriptor], SANDBOX_META);
            node = t->fds[descriptor]->node;
        }
    } else {
        char path[OS_PATH_MAX];
        e = path_argument(t, r->rdi, path);
        if (!e)
            e = vfs_lookup(path, &node);
    }
    if (!e && (!node.mount || strcmp(node.mount->type, "ramfs")))
        e = -E_NOTSUP;
    if (!e) {
        /* Linux simple_statfs RAMFS reports unknown block/inode capacities as
           zero; it is not a fixed-size tmpfs. Never invent persistent FS data. */
        st.type = 0x858458f6;
        st.bsize = st.frsize = PAGE_SIZE;
        st.namelen = OS_NAME_MAX;
        st.flags = 0x20 | (node.mount->readonly ? 1 : 0); /* ST_VALID / RDONLY */
    }
    mutex_unlock(&vfs_mutex);
    return e ? e : user_copy_out(&t->space, r->rsi, &st, sizeof(st)) ? 0 : -E_FAULT;
}

static long linux_flock(struct task *t, register_t *r) {
    unsigned operation = (unsigned)r->rsi, mode = operation & ~4u;
    if (operation & ~15u || (mode != 1 && mode != 2 && mode != 8))
        return -E_INVAL;
    struct file *file = task_file_pin(t, r->rdi);
    if (!file)
        return -E_BADF;
    for (;;) {
        mutex_lock(&vfs_mutex);
        long e = sandbox_file(file, SANDBOX_META);
        if (!e)
            e = vfs_flock(file, mode);
        mutex_unlock(&vfs_mutex);
        if (e != -E_AGAIN || (operation & 4))
            return e;
        if (task_pending_signal(t))
            task_exit(task_pending_signal(t));
        if (signal_pending(t))
            return -E_INTR;
        sched_sleep(1000);
    }
}

static long linux_clock_sleep(struct task *t, register_t *r) {
    extern int64_t kernel_boot_epoch;
    unsigned clock = r->rdi, flags = r->rsi & 1u;
    if (clock > 1)
        return -E_INVAL;
    if (clock == 0 && !kernel_boot_epoch)
        return -E_NOTSUP;
    struct os_timespec ts;
    if (!user_copy_in(&t->space, &ts, r->rdx, sizeof(ts)))
        return -E_FAULT;
    if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000 ||
        (uint64_t)ts.sec > (UINT64_MAX - 999999) / 1000000)
        return -E_INVAL;
    if (!flags && r->r10 && !user_access(&t->space, r->r10, sizeof(ts), true))
        return -E_FAULT;
    uint64_t duration = (uint64_t)ts.sec * 1000000 + (ts.nsec + 999) / 1000;
    uint64_t start = tsc_uptime_us(), deadline;
    if (flags) {
        uint64_t epoch = clock == 0 ? (uint64_t)kernel_boot_epoch * 1000000 : 0;
        deadline = duration > epoch ? duration - epoch : 0;
    } else {
        if (duration > UINT64_MAX - start)
            return -E_INVAL;
        deadline = start + duration;
    }
    for (;;) {
        uint64_t now = tsc_uptime_us();
        if (now >= deadline)
            return 0;
        if (task_pending_signal(t))
            task_exit(task_pending_signal(t));
        if (signal_pending(t)) {
            if(task_stop_checkpoint(t))continue;
            uint64_t left = deadline - now;
            ts = (struct os_timespec){left / 1000000, (left % 1000000) * 1000};
            if (!flags && r->r10 && !user_copy_out(&t->space, r->r10, &ts, sizeof(ts)))
                return -E_FAULT;
            return -E_INTR;
        }
        sched_sleep(min(deadline - now, 1000000ULL));
    }
}

static long linux_close_range(struct task *t, register_t *r) {
    unsigned first = r->rdi, last = r->rsi, flags = r->rdx;
    if (first > last || flags & ~6u)
        return -E_INVAL;
    /* Copying a shared task_context would also duplicate VM/signal ownership.
       Reject UNSHARE on a shared table instead of pretending to detach it. */
    if ((flags & 2) && __atomic_load_n(&t->context->references, __ATOMIC_ACQUIRE) > 1)
        return -E_NOTSUP;
    mutex_lock(&vfs_mutex);
    for (unsigned fd = first; fd < OS_FD_MAX && fd <= last; fd++) {
        if (!t->fds[fd])
            continue;
        if (flags & 4) {
            t->fd_flags[fd] |= FD_CLOEXEC;
            continue;
        }
        if (!fd)
            task_release_input(t, t->fds[fd]);
        vfs_record_release(t->fds[fd], t->tgid);
        vfs_close(t->fds[fd]);
        t->fds[fd] = NULL;
        t->fd_flags[fd] = 0;
    }
    mutex_unlock(&vfs_mutex);
    return 0;
}
