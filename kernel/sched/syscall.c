#include <sched.h>
#include <random.h>
#include <tty.h>
#include <drivers/block.h>
#include <mm/pmm.h>
#include <console.h>
#include <string.h>
#include <tsc.h>
#include <video/display.h>
#include <video/fbcon.h>
#include <pty.h>
#include <fs/file_time.h>
#include <net/network.h>
#include <fs/eventfd.h>
#include <fs/memfd.h>
#include <fs/unix_socket.h>
#include <fs/epoll.h>
#include <sandbox.h>
#include <drivers/audio.h>

#define IO_LIMIT (16 * 1024 * 1024)

extern void syscall_entry(void);

static void msr(unsigned n, uint64_t value) {
    __asm__ volatile("wrmsr" ::"c"(n), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                     : "memory");
}

void syscall_cpu_init(void) {
    msr(0xc0000081, (0x18ULL << 48) | (8ULL << 32));
    msr(0xc0000082, (uintptr_t)syscall_entry);
    msr(0xc0000084,
        (1ULL << 8) | (1ULL << 9) | (1ULL << 10) | (3ULL << 12) | (1ULL << 14) | (1ULL << 18));
}

static int path_argument(struct task *t, uint64_t ptr, char *out) {
    char raw[OS_PATH_MAX];
    int e = user_string(&t->space, raw, ptr, sizeof(raw));
    return e ? e : vfs_normalize(t->cwd, raw, out);
}

static int path_at(struct task *t, int fd, uint64_t ptr, char *out) {
    char raw[OS_PATH_MAX];
    int e = user_string(&t->space, raw, ptr, sizeof(raw));
    if (e)
        return e;
    const char *base = t->cwd;
    if (raw[0] != '/' && fd != AT_FDCWD) {
        if (fd < 0 || fd >= OS_FD_MAX || !t->fds[fd])
            return -E_BADF;
        if ((t->fds[fd]->node.mode & 0170000) != S_IFDIR)
            return -E_NOTDIR;
        base = t->fds[fd]->path;
    }
    return vfs_normalize(base, raw, out);
}

struct argument_sizes {
    size_t bytes;
    unsigned count;
};

static int measure_arguments(struct task *t, uint64_t ptr, unsigned limit,
                             struct argument_sizes *sizes) {
    *sizes = (struct argument_sizes){0};
    if (!ptr)
        return 0;
    for (unsigned i = 0; i <= limit; i++) {
        uint64_t arg;
        size_t bytes;
        if (ptr > UINT64_MAX - i * 8ULL || !user_copy_in(&t->space, &arg, ptr + i * 8, 8))
            return -E_FAULT;
        if (!arg) {
            sizes->count = i;
            return 0;
        }
        if (i == limit || sizes->bytes == OS_EXEC_BYTES_MAX)
            return -E_2BIG;
        int e = user_string_size(&t->space, arg,
                                 min((size_t)OS_ARG_STRING_MAX, OS_EXEC_BYTES_MAX - sizes->bytes),
                                 &bytes);
        if (e)
            return e == -E_NAMETOOLONG ? -E_2BIG : e;
        sizes->bytes += bytes;
    }
    return -E_2BIG;
}

static int arguments(struct task *t, uint64_t ptr, char **argv, char *storage, unsigned limit,
                     size_t capacity) {
    if (!ptr) {
        argv[0] = NULL;
        return 0;
    }
    size_t used = 0;
    for (unsigned i = 0; i <= limit; i++) {
        uint64_t arg;
        if (ptr > UINT64_MAX - i * 8ULL)
            return -E_FAULT;
        if (!user_copy_in(&t->space, &arg, ptr + i * 8, 8))
            return -E_FAULT;
        if (!arg) {
            argv[i] = NULL;
            return 0;
        }
        if (i == limit || used == capacity)
            return -E_2BIG;
        argv[i] = storage + used;
        int e =
            user_string(&t->space, argv[i], arg, min((size_t)OS_ARG_STRING_MAX, capacity - used));
        if (e)
            return e == -E_NAMETOOLONG ? -E_2BIG : e;
        used += strlen(argv[i]) + 1;
    }
    return -E_2BIG;
}

static long process_call(struct task *t, register_t *r, bool exec) {
    char path[OS_PATH_MAX];
    int e = path_argument(t, r->rdi, path);
    if (e)
        return e;

    struct argument_sizes args, environment = {0};
    e = measure_arguments(t, r->rsi, OS_ARG_MAX, &args);
    if (e)
        return e;
    if (exec || r->rax == SYS_spawnve) {
        e = measure_arguments(t, r->rdx, OS_ENV_MAX, &environment);
        if (e)
            return e;
    }

    /* Copy only the actual strings and pointers. A fixed 256 KiB scratch
       allocation for every short exec fragmented the source-tree heap until
       no new process could start, despite tens of MiB of free memory.
       Another user thread may change the source between passes; the second
       pass checks every pointer, terminator and measured bound again. */
    size_t slots = max(args.count, 1U) + 1;
    char *storage = kmalloc(max(args.bytes + environment.bytes, 1UL));
    char **argv = kmalloc((slots + environment.count + 1) * sizeof(*argv));
    if (!storage || !argv) {
        kfree(storage);
        kfree(argv);
        return -E_NOMEM;
    }

    char **env = argv + slots;
    e = arguments(t, r->rsi, argv, storage, args.count, args.bytes);
    if (!e && !argv[0]) {
        argv[0] = path;
        argv[1] = NULL;
    }

    if (!e) {
        if (exec) {
            e = arguments(t, r->rdx, env, storage + args.bytes, environment.count,
                          environment.bytes);
            if (!e)
                e = process_exec_env(t, path, argv, env, r);
        } else if (r->rax == SYS_spawnve) {
            e = arguments(t, r->rdx, env, storage + args.bytes, environment.count,
                          environment.bytes);
            if (!e && (r->r10 & ~1ULL))
                e = -E_INVAL;
            if (!e) {
                struct file **streams = t->fds[0] && !t->fds[0]->tty ? t->fds : NULL;
                e = process_spawn_env(path, argv, env, t->pid, t->cwd, -1, !(r->r10 & 1), streams);
            }
        } else if (r->rax == SYS_spawn_web) {
            struct os_spawn_web spec;
            struct file *streams[3];
            char origin[256] = {0}, home[OS_PATH_MAX] = "/";

            if (!user_copy_in(&t->space, &spec, r->rdx, sizeof(spec)))
                e = -E_FAULT;
            else if (spec.reserved || spec.reserved2 ||
                     (spec.role != OS_WEB_BROKER && spec.role != OS_WEB_RENDERER))
                e = -E_INVAL;
            else if ((!t->context->web_role && spec.role != OS_WEB_BROKER) ||
                     (t->context->web_role &&
                      (t->context->web_role != OS_WEB_BROKER || spec.role != OS_WEB_RENDERER)))
                e = -E_PERM;
            else {
                int fd[3] = {spec.ipc_fd, spec.stdout_fd, spec.stderr_fd};
                for (unsigned i = 0; i < 3; i++) {
                    if (fd[i] < 0 || fd[i] >= OS_FD_MAX || !t->fds[fd[i]]) {
                        e = -E_BADF;
                        break;
                    }
                    streams[i] = t->fds[fd[i]];
                    if ((!i && (!streams[i]->unix_socket || (streams[i]->flags & 3) != O_RDWR)) ||
                        (i && (!streams[i]->pipe || !(streams[i]->flags & 3)))) {
                        e = -E_PERM;
                        break;
                    }
                }

                if (!e && spec.origin)
                    e = user_string(&t->space, origin, spec.origin, sizeof(origin));
                if (!e && spec.role == OS_WEB_RENDERER) {
                    if (spec.home || !sandbox_web_origin(origin))
                        e = -E_INVAL;
                    else if (strcmp(path, "/bin/web-renderer") && strcmp(path, "/bin/jsc") &&
                             strcmp(path, "/bin/glib-check") &&
                             strcmp(path, "/bin/web-capability-check"))
                        e = -E_PERM;
                } else if (!e) {
                    if (origin[0] || !spec.home)
                        e = -E_INVAL;
                    else
                        e = path_argument(t, spec.home, home);
                    if (!e) {
                        char real[OS_PATH_MAX];
                        struct vnode node;
                        mutex_lock(&vfs_mutex);
                        e = vfs_realpath(home, real);
                        if (!e)
                            e = vfs_lookup(real, &node);
                        mutex_unlock(&vfs_mutex);
                        if (!e && (node.mode & 0170000) != S_IFDIR)
                            e = -E_NOTDIR;
                        if (!e && strlen(real) + sizeof("/.config/webkit") >= OS_PATH_MAX)
                            e = -E_NAMETOOLONG;
                        if (!e)
                            strcpy(home, real);
                    }
                }

                if (!e)
                    e = process_spawn_web(path, argv, t->tgid, home, streams, spec.role,
                                          t->context->web_domain, origin);
            }
        } else if (r->rax == SYS_spawn_io || r->rax == SYS_spawn_browser) {
            struct os_spawn_io spec;
            struct file *streams[3];
            char cwd[OS_PATH_MAX];
            char local[OS_PATH_MAX] = {0};

            if (!user_copy_in(&t->space, &spec, r->rdx, sizeof(spec)))
                e = -E_FAULT;
            else if (spec.reserved)
                e = -E_INVAL;
            else {
                for (unsigned i = 0; i < 3; i++) {
                    if (spec.fd[i] < 0 || spec.fd[i] >= OS_FD_MAX || !t->fds[spec.fd[i]]) {
                        e = -E_BADF;
                        break;
                    }
                    streams[i] = t->fds[spec.fd[i]];
                    if ((i == 0 && (streams[i]->flags & 3) == O_WRONLY) ||
                        (i && !(streams[i]->flags & 3))) {
                        e = -E_BADF;
                        break;
                    }
                    if (r->rax == SYS_spawn_browser && !streams[i]->pipe) {
                        e = -E_PERM;
                        break;
                    }
                }

                if (!e)
                    e = spec.cwd ? path_argument(t, spec.cwd, cwd)
                                 : vfs_normalize(t->cwd, ".", cwd);
                if (!e) {
                    struct vnode dir;
                    mutex_lock(&vfs_mutex);
                    e = vfs_lookup(cwd, &dir);
                    mutex_unlock(&vfs_mutex);
                    if (!e && (dir.mode & 0170000) != S_IFDIR)
                        e = -E_NOTDIR;
                }

                if (!e && r->rax == SYS_spawn_browser) {
                    struct os_spawn_browser browser;
                    if (!user_copy_in(&t->space, &browser, r->rdx, sizeof(browser)))
                        e = -E_FAULT;
                    else if (browser.local_file)
                        e = path_argument(t, browser.local_file, local);

                    /* Set up the private directory's parent before launch. */
                    if (!e) {
                        char config[OS_PATH_MAX];
                        int n = snprintf(config, sizeof(config), "%s/.config", cwd);
                        if (n < 0 || n >= OS_PATH_MAX)
                            e = -E_NAMETOOLONG;
                        else {
                            mutex_lock(&vfs_mutex);
                            e = vfs_mkdir(config, 0700);
                            if (e == -E_EXIST)
                                e = 0;
                            mutex_unlock(&vfs_mutex);
                        }
                    }
                    if (!e)
                        e = process_spawn_browser(path, argv, t->pid, cwd, streams, local);
                } else if (!e)
                    e = process_spawn_streams(path, argv, t->pid, cwd, -1, false, streams);
            }
        } else {
            if (r->rdx & ~1ULL)
                e = -E_INVAL;
            else {
                e = process_spawn(path, argv, t->pid, t->cwd, -1, !(r->rdx & 1));
            }
        }
    }

    kfree(argv);
    kfree(storage);
    return e;
}

static long io_file_call(struct task *t, register_t *r, bool write, bool positional,
                         struct file *file) {
    size_t size = r->rdx;
    uint64_t address = r->rsi;
    if (positional && (file->tty || file->pipe || file->pty || file->device || file->socket ||
                       file->unix_socket || file->event || file->epoll))
        return -E_SPIPE;
    if (positional && ((int64_t)r->r10 < 0 || size > INT64_MAX - r->r10))
        return -E_INVAL;
    if (write ? !(file->flags & 3) : (file->flags & 3) == O_WRONLY)
        return -E_BADF;
    if (size > IO_LIMIT)
        return -E_2BIG;
    if (!user_access(&t->space, address, size, !write))
        return -E_FAULT;
    if (file->event) {
        uint64_t value;
        if (size < 8 || (write && size != 8))
            return -E_INVAL;
        if (write && !user_copy_in(&t->space, &value, address, 8))
            return -E_FAULT;
        long result = eventfd_transfer(file, &value, size, write);
        return result > 0 && !write && !user_copy_out(&t->space, address, &value, 8) ? -E_FAULT
                                                                                     : result;
    }
    if (file->socket || file->unix_socket) {
        register_t socket_call = *r;
        socket_call.rax = write ? SYS_sendto : SYS_recvfrom;
        socket_call.r10 = socket_call.r8 = socket_call.r9 = 0;
        return network_file_syscall(t, &socket_call, file);
    }
    size_t done = 0;
    uint8_t buffer[4096];
    while (done < size) {
        size_t chunk = min(size - done, sizeof(buffer));
        long n;
        if (write && !user_copy_in(&t->space, buffer, address + done, chunk))
            return done ? (long)done : -E_FAULT;
        if (file->pty)
            n = write ? pty_write(file, buffer, chunk) : pty_read(file, buffer, chunk);
        else if (file->tty) {
            if (write) {
                console_write((const char *)buffer, chunk);
                n = chunk;
            } else
                n = tty_read(buffer, chunk, file->flags & O_NONBLOCK);
        } else if (file->pipe)
            n = write ? pipe_write(file, buffer, chunk) : pipe_read(file, buffer, chunk);
        else {
            mutex_lock(&vfs_mutex);
            struct file at;
            struct file *active = file;
            if (positional) {
                at = *file;
                at.offset = r->r10 + done;
                active = &at;
            }
            n = write ? vfs_write(active, buffer, chunk) : vfs_read(active, buffer, chunk);
            mutex_unlock(&vfs_mutex);
        }
        if (n < 0)
            return done ? (long)done : n;
        if (!write && n && !user_copy_out(&t->space, address + done, buffer, n))
            return done ? (long)done : -E_FAULT;
        done += n;
        if ((size_t)n < chunk || ((file->tty || file->pipe || file->pty) && !write))
            break;
    }
    return done;
}

static long io_call(struct task *t, register_t *r, bool write, bool positional) {
    struct file *file = task_file_pin(t, r->rdi);
    return file ? io_file_call(t, r, write, positional, file) : -E_BADF;
}

static long open_call(struct task *t, int dirfd, uint64_t pathptr, unsigned flags, unsigned mode) {
    unsigned fd = 0;
    while (fd < OS_FD_MAX && t->fds[fd])
        fd++;
    if (fd == OS_FD_MAX)
        return -E_MFILE;
    char path[OS_PATH_MAX];
    int e = path_at(t, dirfd, pathptr, path);
    if (e)
        return e;
    mutex_lock(&vfs_mutex);
    e = vfs_open(path, flags, mode & 0777 & ~t->context->creation_mask, &t->fds[fd]);
    if (!e)
        t->fd_flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    mutex_unlock(&vfs_mutex);
    return e ? e : (long)fd;
}

static long stat_call(struct task *t, register_t *r, bool fd) {
    uint64_t destination = r->rsi;
    if (!user_access(&t->space, destination, sizeof(struct os_stat), true))
        return -E_FAULT;
    struct vnode node;
    int e = 0;
    mutex_lock(&vfs_mutex);
    if (fd) {
        if (r->rdi >= OS_FD_MAX || !t->fds[r->rdi])
            e = -E_BADF;
        else {
            node = t->fds[r->rdi]->node;
            if (t->fds[r->rdi]->memory)
                node.size = memfd_size(t->fds[r->rdi]->memory);
            else if (node.mount && node.mount->ops->stat)
                e = node.mount->ops->stat(&node);
        }
    } else {
        char path[OS_PATH_MAX];
        e = path_argument(t, r->rdi, path);
        if (!e)
            e = r->rax == SYS_lstat ? vfs_lstat(path, &node) : vfs_lookup(path, &node);
    }
    struct os_stat st = {0};
    if (!e) {
        st.ino = node.mount && node.mount->device ? node.id : node.aux[3];
        st.mode = node.mode;
        st.nlink = 1;
        st.size = node.size;
        st.blksize = 4096;
        st.blocks = (node.size + 511) / 512;
        st.atime_sec = node.atime;
        st.mtime_sec = node.mtime;
        st.ctime_sec = node.ctime;
        st.dev = node.mount && node.mount->device
                     ? (uint64_t)(node.mount->device - block_devices) + 1
                     : 0;
    }
    mutex_unlock(&vfs_mutex);
    if (e)
        return e;
    return user_copy_out(&t->space, destination, &st, sizeof(st)) ? 0 : -E_FAULT;
}

static long vector_call(struct task *t, register_t *r, bool write) {
    if (r->rdx > 16)
        return -E_INVAL;
    struct file *file = task_file_pin(t, r->rdi);
    if (!file)
        return -E_BADF;
    if (file->socket || file->unix_socket)
        return network_file_syscall(t, r, file);
    struct os_iovec v[16];
    size_t total = 0;
    if (!user_copy_in(&t->space, v, r->rsi, r->rdx * sizeof(v[0])))
        return -E_FAULT;
    for (unsigned i = 0; i < r->rdx; i++) {
        if (v[i].len > IO_LIMIT - total)
            return -E_2BIG;
        if (!user_access(&t->space, v[i].base, v[i].len, !write))
            return -E_FAULT;
        total += v[i].len;
    }
    if (file->event) {
        uint64_t value = 0;
        size_t at = 0;
        if (total < 8 || (write && total != 8))
            return -E_INVAL;
        if (write)
            for (unsigned i = 0; i < r->rdx; i++) {
                size_t count = min((size_t)v[i].len, 8 - at);
                if (count && !user_copy_in(&t->space, (uint8_t *)&value + at, v[i].base, count))
                    return -E_FAULT;
                at += count;
            }
        long result = eventfd_transfer(file, &value, total, write);
        if (result > 0 && !write)
            for (unsigned i = 0; i < r->rdx && at < 8; i++) {
                size_t count = min((size_t)v[i].len, 8 - at);
                if (count && !user_copy_out(&t->space, v[i].base, (uint8_t *)&value + at, count))
                    return -E_FAULT;
                at += count;
            }
        return result;
    }
    long done = 0;
    for (unsigned i = 0; i < r->rdx; i++) {
        register_t part = *r;
        part.rsi = v[i].base;
        part.rdx = v[i].len;
        long n = io_file_call(t, &part, write, false, file);
        if (n < 0)
            return done ? done : n;
        done += n;
        if ((uint64_t)n < v[i].len)
            break;
    }
    return done;
}

static long duplicate(struct task *t, unsigned old, unsigned first, bool exact, unsigned flags) {
    if (old >= OS_FD_MAX || !t->fds[old] || first >= OS_FD_MAX)
        return -E_BADF;
    unsigned dest = first;
    if (!exact) {
        while (dest < OS_FD_MAX && t->fds[dest])
            dest++;
        if (dest == OS_FD_MAX)
            return -E_MFILE;
    }
    if (old == dest)
        return dest;
    mutex_lock(&vfs_mutex);
    if (!dest && t->fds[dest] &&
        (t->fds[dest]->tty != t->fds[old]->tty || t->fds[dest]->pty != t->fds[old]->pty))
        task_release_input(t, t->fds[dest]);
    if (t->fds[dest])
        vfs_close(t->fds[dest]);
    t->fds[dest] = t->fds[old];
    t->fds[dest]->references++;
    t->fd_flags[dest] = flags;
    mutex_unlock(&vfs_mutex);
    return dest;
}

static long ioctl_call(struct task *t, register_t *r) {
    if (r->rdi >= OS_FD_MAX || !t->fds[r->rdi])
        return -E_BADF;
    if (t->fds[r->rdi]->socket || t->fds[r->rdi]->unix_socket) {
        int value = 0;
        long error;
        if (r->rsi == 0x5421) {
            if (!user_copy_in(&t->space, &value, r->rdx, sizeof(value)))
                return -E_FAULT;
            return t->fds[r->rdi]->unix_socket ? unix_ioctl(t->fds[r->rdi], r->rsi, &value)
                                               : network_ioctl(t->fds[r->rdi], r->rsi, &value);
        }
        if (!user_access(&t->space, r->rdx, sizeof(value), true))
            return -E_FAULT;
        error = t->fds[r->rdi]->unix_socket ? unix_ioctl(t->fds[r->rdi], r->rsi, &value)
                                            : network_ioctl(t->fds[r->rdi], r->rsi, &value);
        return error                                                     ? error
               : user_copy_out(&t->space, r->rdx, &value, sizeof(value)) ? 0
                                                                         : -E_FAULT;
    }
    if (t->fds[r->rdi]->pty)
        return pty_ioctl(t->fds[r->rdi], r->rsi, r->rdx);
    if (!t->fds[r->rdi]->tty)
        return -E_NOTTY;
    if (r->rsi == TCGETS) {
        struct os_termios config;
        tty_get_config(&config);
        return user_copy_out(&t->space, r->rdx, &config, sizeof(config)) ? 0 : -E_FAULT;
    }
    if (r->rsi == TCSETS || r->rsi == TCSETSW || r->rsi == TCSETSF) {
        struct os_termios config;
        if (!user_copy_in(&t->space, &config, r->rdx, sizeof(config)))
            return -E_FAULT;
        return tty_set_config(&config, r->rsi == TCSETSF);
    }
    if (r->rsi == TIOCGWINSZ) {
        struct os_winsize size;
        fbcon_winsize(&size);
        return user_copy_out(&t->space, r->rdx, &size, sizeof(size)) ? 0 : -E_FAULT;
    }
    if (r->rsi == FIONREAD) {
        if (!tty_is_foreground(t->pid))
            return -E_PERM;
        int n = tty_available();
        return user_copy_out(&t->space, r->rdx, &n, sizeof(n)) ? 0 : -E_FAULT;
    }
    return -E_NOTTY;
}

static long directory_call(struct task *t, register_t *r) {
    if (r->rdi >= OS_FD_MAX || !t->fds[r->rdi])
        return -E_BADF;
    if (r->rdx < 24 || r->rdx > IO_LIMIT)
        return -E_INVAL;
    if (!user_access(&t->space, r->rsi, r->rdx, true))
        return -E_FAULT;
    struct file *f = t->fds[r->rdi];
    size_t done = 0;
    mutex_lock(&vfs_mutex);
    while (done < r->rdx) {
        struct directory_entry entry;
        uint64_t previous = f->offset;
        int e = vfs_readdir(f, &entry);
        if (e <= 0) {
            mutex_unlock(&vfs_mutex);
            return done ? (long)done : e;
        }
        size_t n = align(19 + strlen(entry.name) + 1, 8UL);
        if (n > r->rdx - done) {
            f->offset = previous;
            break;
        }
        uint8_t record[288] = {0};
        struct os_dirent *d = (void *)record;
        d->ino = entry.id;
        d->off = f->offset;
        d->reclen = n;
        d->type = (entry.mode & 0170000) == S_IFDIR   ? 4
                  : (entry.mode & 0170000) == S_IFLNK ? 10
                  : (entry.mode & 0170000) == S_IFCHR ? 2
                                                      : 8;
        strcpy(d->name, entry.name);
        user_copy_out(&t->space, r->rsi + done, record, n);
        done += n;
    }
    mutex_unlock(&vfs_mutex);
    return done ? (long)done : -E_INVAL;
}

static long mmap_call(struct task *t, register_t *r) {
    uint64_t size = r->rsi, addr = r->rdi, prot = r->rdx, flags = r->r10;
    if (!size || size > USER_MAX_ALLOC || prot & ~7ULL || (prot && !(prot & 1)) ||
        (prot & 6) == 6 || flags & ~0x4033ULL || (flags & 3) > 2 || !(flags & 3) || r->r9 & 4095 ||
        r->r9 > INT64_MAX || size > INT64_MAX - r->r9)
        return -E_INVAL;
    bool anonymous = flags & 0x20;
    struct file *file = NULL;
    if (anonymous) {
        if ((int)r->r8 != -1 || r->r9)
            return -E_INVAL;
    } else {
        int fd = (int)r->r8;
        if (fd < 0 || fd >= OS_FD_MAX || !t->fds[fd] || (t->fds[fd]->flags & 3) == O_WRONLY)
            return -E_BADF;
        file = t->fds[fd];
        if ((file->node.mode & 0170000) != S_IFREG)
            return -E_NODEV;
        int permission = sandbox_file(file, SANDBOX_READ);
        if (permission)
            return permission;
    }
    bool shared = (flags & 3) == 1;
    if (shared && (!file || !file->memory))
        return -E_NOTSUP;
    if (file && file->memory && (prot & 4))
        return -E_ACCES;
    if (shared && (prot & 2) && (!(file->flags & 3) || sandbox_file(file, SANDBOX_WRITE)))
        return -E_ACCES;
    uint64_t requested = size;
    size = align(size, 4096ULL);
    if (!(flags & 0x10) && t->space.vm->page_limit &&
        size / PAGE_SIZE > t->space.vm->page_limit - t->space.vm->pages)
        return -E_NOMEM;
    if (!addr)
        addr = t->space.vm->mmap_next;
    if ((addr & 4095) || addr < USER_MIN || addr >= USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE ||
        size > USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE - addr)
        return -E_NOMEM;
    unsigned vmflags =
        ((prot & 1) ? VM_READ : 0) | ((prot & 2) ? VM_WRITE : 0) | ((prot & 4) ? VM_EXEC : 0);
    uint64_t mapped = 0;
    int error = -E_NOMEM;
    if (file) {
        /* MAP_PRIVATE is an eager, independent snapshot. Writes never reach
           the file and the shared descriptor offset remains unchanged.
           Whole pages beyond EOF need retained backing-file metadata and
           page-fault handling that delivers SIGBUS; snapshots reject them. */
        mutex_lock(&vfs_mutex);
        if (file->memory) {
            file->node.size = memfd_size(file->memory);
            error = 0;
        } else if (file->node.mount->ops->stat)
            error = file->node.mount->ops->stat(&file->node);
        else
            error = 0;
        if (error || r->r9 >= file->node.size || size > align(file->node.size - r->r9, 4096ULL)) {
            mutex_unlock(&vfs_mutex);
            return error ? error : -E_INVAL;
        }
        error = -E_NOMEM;
    }
    for (; mapped < size; mapped += PAGE_SIZE) {
        if (flags & 0x10)
            user_unmap(&t->space, addr + mapped);
        if (shared) {
            if (!user_map_memory(&t->space, addr + mapped, vmflags, file->memory, r->r9 + mapped,
                                 !(file->flags & 3)))
                break;
            continue;
        }
        if (!user_map(&t->space, addr + mapped, vmflags | (file ? VM_READ : 0)))
            break;
    }
    if (mapped == size && file && !shared) {
        uint8_t buffer[PAGE_SIZE];
        uint64_t previous = file->offset, bytes = min(requested, file->node.size - r->r9);
        file->offset = r->r9;
        for (uint64_t done = 0; done < bytes;) {
            size_t part = min(bytes - done, (uint64_t)sizeof(buffer));
            long count = vfs_read(file, buffer, part);
            if (count <= 0 || !user_load_bytes(&t->space, addr + done, buffer, count)) {
                error = count < 0 ? (int)count : -E_IO;
                break;
            }
            done += count;
            if (done == bytes)
                error = 0;
        }
        file->offset = previous;
        if (!error && !(vmflags & VM_READ))
            for (uint64_t off = 0; off < size; off += PAGE_SIZE)
                if (!user_protect(&t->space, addr + off, vmflags)) {
                    error = -E_IO;
                    break;
                }
    } else if (mapped == size)
        error = 0;
    if (file)
        mutex_unlock(&vfs_mutex);
    if (error) {
        while (mapped) {
            mapped -= PAGE_SIZE;
            user_unmap(&t->space, addr + mapped);
        }
        return error;
    }
    t->space.vm->mmap_next = max(t->space.vm->mmap_next, addr + size);
    return addr;
}

static long brk_call(struct task *t, uint64_t addr) {
    struct address_space *s = &t->space;
    if (!addr)
        return s->vm->brk_end;
    if (addr < s->vm->brk_base || addr - s->vm->brk_base > USER_MAX_ALLOC)
        return s->vm->brk_end;
    uint64_t first = align(s->vm->brk_end, 4096ULL), last = align(addr, 4096ULL), mapped = first;
    for (; mapped < last; mapped += PAGE_SIZE)
        if (!user_map(s, mapped, VM_RDWR))
            break;
    if (mapped < last) {
        while (mapped > first) {
            mapped -= PAGE_SIZE;
            user_unmap(s, mapped);
        }
        return s->vm->brk_end;
    }
    for (uint64_t va = last; va < first; va += PAGE_SIZE)
        user_unmap(s, va);
    s->vm->brk_end = addr;
    return addr;
}

static long mremap_call(struct task *t, register_t *r) {
    uint64_t address = r->rdi, old_size = r->rsi, new_size = r->rdx;
    if (r->r10)
        return -E_NOTSUP; /* Moving/aliasing needs full VMA tracking. */
    if ((address & 4095) || address < USER_MIN || !old_size || !new_size ||
        old_size > USER_MAX_ALLOC || new_size > USER_MAX_ALLOC)
        return -E_INVAL;
    old_size = align(old_size, PAGE_SIZE);
    new_size = align(new_size, PAGE_SIZE);
    if (address >= USER_STACK_TOP || max(old_size, new_size) > USER_STACK_TOP - address)
        return -E_INVAL;
    unsigned protection;
    if (!user_mapping_flags(&t->space, address, &protection))
        return -E_FAULT;
    for (uint64_t offset = 0; offset < old_size; offset += PAGE_SIZE) {
        unsigned flags;
        if (!user_mapping_flags(&t->space, address + offset, &flags))
            return -E_FAULT;
        if (user_shared(&t->space, address + offset))
            return -E_NOTSUP;
        if (flags != protection)
            return -E_NOTSUP;
    }
    /* Never replace an adjacent mapping or partially destroy the old range
       on failed growth. musl uses this real behavior to find the main stack. */
    for (uint64_t offset = old_size; offset < new_size; offset += PAGE_SIZE)
        if (user_mapped(&t->space, address + offset))
            return -E_NOMEM;
    uint64_t mapped = old_size;
    for (; mapped < new_size; mapped += PAGE_SIZE)
        if (!user_map(&t->space, address + mapped, protection))
            break;
    if (mapped < new_size) {
        while (mapped > old_size) {
            mapped -= PAGE_SIZE;
            user_unmap(&t->space, address + mapped);
        }
        return -E_NOMEM;
    }
    for (uint64_t offset = new_size; offset < old_size; offset += PAGE_SIZE)
        user_unmap(&t->space, address + offset);
    t->space.vm->mmap_next = max(t->space.vm->mmap_next, address + new_size);
    return address;
}

static long poll_wait(struct task *t, struct os_pollfd *fds, unsigned count, int64_t timeout) {
    uint64_t start = tsc_uptime_us();
    for (;;) {
        long ready = 0;
        mutex_lock(&t->context->syscall_lock);
        for (unsigned i = 0; i < count; i++) {
            int fd = fds[i].fd;
            fds[i].revents = 0;
            if (fd < 0)
                continue;
            if (fd >= OS_FD_MAX || !t->fds[fd])
                fds[i].revents = POLLNVAL;
            else
                fds[i].revents = vfs_poll(t->fds[fd], fds[i].events);
            ready += fds[i].revents != 0;
        }
        mutex_unlock(&t->context->syscall_lock);
        if (ready || (timeout >= 0 && tsc_uptime_us() - start >= (uint64_t)timeout))
            return ready;
        unsigned killed = task_pending_signal(t);
        if (killed)
            task_exit(killed);
        if (signal_pending(t))
            return -E_INTR;
        sched_sleep(1000);
    }
}

static long poll_call(struct task *t, register_t *r, bool precise) {
    if (r->rsi > OS_FD_MAX)
        return -E_INVAL;
    unsigned count = r->rsi;
    struct os_pollfd fds[OS_FD_MAX];
    if (!user_access(&t->space, r->rdi, count * sizeof(*fds), true) ||
        !user_copy_in(&t->space, fds, r->rdi, count * sizeof(*fds)))
        return -E_FAULT;
    int64_t timeout = -1;
    uint64_t previous = t->signal_mask;
    if (precise) {
        if (r->rdx) {
            struct os_timespec ts;
            if (!user_copy_in(&t->space, &ts, r->rdx, sizeof(ts)))
                return -E_FAULT;
            if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000 ||
                ts.sec > (INT64_MAX - (ts.nsec + 999) / 1000) / 1000000)
                return -E_INVAL;
            timeout = ts.sec * 1000000 + (ts.nsec + 999) / 1000;
        }
        if (r->r10) {
            uint64_t mask;
            if (r->r8 != 8)
                return -E_INVAL;
            if (!user_copy_in(&t->space, &mask, r->r10, 8))
                return -E_FAULT;
            t->signal_mask = mask & ~((1ULL << 8) | (1ULL << 18));
        }
    } else if ((int32_t)r->rdx >= 0)
        timeout = (int64_t)(int32_t)r->rdx * 1000;
    long result = poll_wait(t, fds, count, timeout);
    /* An interrupting handler must run under the temporary mask. Its signal
       frame restores the original mask after the handler, as for sigsuspend. */
    if (result != -E_INTR)
        t->signal_mask = previous;
    if (result >= 0 && !user_copy_out(&t->space, r->rdi, fds, count * sizeof(*fds)))
        return -E_FAULT;
    return result;
}

static long select_call(struct task *t, register_t *r, bool precise) {
    if (r->rdi > OS_FD_MAX)
        return -E_INVAL;
    uint64_t set[3][(OS_FD_MAX + 63) / 64] = {{0}}, out[3][(OS_FD_MAX + 63) / 64] = {{0}};
    uint64_t ptr[3] = {r->rsi, r->rdx, r->r10};
    size_t bytes = ((r->rdi + 63) / 64) * 8;
    for (unsigned i = 0; i < 3; i++)
        if (ptr[i] && r->rdi &&
            (!user_copy_in(&t->space, set[i], ptr[i], bytes) ||
             !user_access(&t->space, ptr[i], bytes, true)))
            return -E_FAULT;
    struct os_pollfd fds[OS_FD_MAX];
    unsigned n = 0;
    for (unsigned fd = 0; fd < r->rdi; fd++)
        if ((set[0][fd / 64] | set[1][fd / 64] | set[2][fd / 64]) & (1ULL << (fd % 64))) {
            if (!t->fds[fd])
                return -E_BADF;
            fds[n++] = (struct os_pollfd){fd,
                                          ((set[0][fd / 64] >> (fd % 64)) & 1 ? POLLIN : 0) |
                                              ((set[1][fd / 64] >> (fd % 64)) & 1 ? POLLOUT : 0),
                                          0};
        }
    int64_t timeout = -1;
    uint64_t previous = t->signal_mask;
    if (r->r8) {
        struct os_timespec ts;
        if (!user_copy_in(&t->space, &ts, r->r8, sizeof(ts)) ||
            (!precise && !user_access(&t->space, r->r8, sizeof(ts), true)))
            return -E_FAULT;
        int64_t fraction = precise ? 1000000000 : 1000000;
        if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= fraction ||
            ts.sec > (INT64_MAX - (precise ? (ts.nsec + 999) / 1000 : ts.nsec)) / 1000000)
            return -E_INVAL;
        timeout = ts.sec * 1000000 + (precise ? (ts.nsec + 999) / 1000 : ts.nsec);
    }
    if (precise && r->r9) {
        uint64_t arg[2], mask;
        if (!user_copy_in(&t->space, arg, r->r9, sizeof(arg)))
            return -E_FAULT;
        if (arg[0]) {
            if (arg[1] != 8)
                return -E_INVAL;
            if (!user_copy_in(&t->space, &mask, arg[0], 8))
                return -E_FAULT;
            t->signal_mask = mask & ~((1ULL << 8) | (1ULL << 18));
        }
    }
    uint64_t start = tsc_uptime_us();
    long ready = poll_wait(t, fds, n, timeout), result = 0;
    if (ready != -E_INTR)
        t->signal_mask = previous;
    if (!precise && r->r8 && timeout >= 0) {
        uint64_t elapsed = tsc_uptime_us() - start;
        uint64_t remaining = elapsed < (uint64_t)timeout ? (uint64_t)timeout - elapsed : 0;
        struct os_timeval tv = {remaining / 1000000, remaining % 1000000};
        if (!user_copy_out(&t->space, r->r8, &tv, sizeof(tv)))
            return -E_FAULT;
    }
    if (ready < 0)
        return ready;
    for (unsigned i = 0; i < n; i++) {
        unsigned word = (unsigned)fds[i].fd / 64;
        uint64_t bit = 1ULL << (fds[i].fd % 64);
        if ((set[0][word] & bit) && (fds[i].revents & (POLLIN | POLLHUP))) {
            out[0][word] |= bit;
            result++;
        }
        if ((set[1][word] & bit) && (fds[i].revents & POLLOUT)) {
            out[1][word] |= bit;
            result++;
        }
    }
    for (unsigned i = 0; i < 3; i++)
        if (ptr[i] && r->rdi && !user_copy_out(&t->space, ptr[i], out[i], bytes))
            return -E_FAULT;
    return result;
}

static long timestamp_call(struct task *t, register_t *r) {
    bool at = r->rax == SYS_utimensat || r->rax == SYS_futimesat;
    bool nano = r->rax == SYS_utimensat;
    uint64_t pathptr = at ? r->rsi : r->rdi, ptr = at ? r->rdx : r->rsi;
    unsigned flags = nano ? r->r10 : 0;
    int fd = at ? (int)r->rdi : AT_FDCWD;
    if (flags & ~AT_SYMLINK_NOFOLLOW || (!pathptr && (!at || flags)))
        return -E_INVAL;
    struct os_timespec times[2];
    if (!ptr)
        times[0] = times[1] = (struct os_timespec){.nsec = 1073741823}; /* UTIME_NOW */
    else if (r->rax == SYS_utime) {
        int64_t seconds[2];
        if (!user_copy_in(&t->space, seconds, ptr, sizeof(seconds)))
            return -E_FAULT;
        for (unsigned i = 0; i < 2; i++)
            times[i] = (struct os_timespec){seconds[i], 0};
    } else if (nano) {
        if (!user_copy_in(&t->space, times, ptr, sizeof(times)))
            return -E_FAULT;
    } else {
        struct os_timeval values[2];
        if (!user_copy_in(&t->space, values, ptr, sizeof(values)))
            return -E_FAULT;
        for (unsigned i = 0; i < 2; i++) {
            if (values[i].usec < 0 || values[i].usec > 999999)
                return -E_INVAL;
            times[i] = (struct os_timespec){values[i].sec, values[i].usec * 1000};
        }
    }
    unsigned changed = 0;
    int64_t now = fs_time_now();
    for (unsigned i = 0; i < 2; i++) {
        if (nano || !ptr) {
            if (times[i].nsec == 1073741822)
                continue; /* UTIME_OMIT */
            if (times[i].nsec == 1073741823) {
                if (!now)
                    return -E_IO;
                times[i].sec = now;
                times[i].nsec = 0;
            }
        }
        if (times[i].nsec < 0 || times[i].nsec > 999999999)
            return -E_INVAL;
        changed |= 1u << i;
    }
    char path[OS_PATH_MAX];
    int e = pathptr ? path_at(t, fd, pathptr, path) : 0;
    if (e)
        return e;
    mutex_lock(&vfs_mutex);
    struct vnode node;
    if (!pathptr) {
        if (fd < 0 || fd >= OS_FD_MAX || !t->fds[fd])
            e = -E_BADF;
        else {
            node = t->fds[fd]->node;
            if (!node.mount)
                e = -E_INVAL;
            else if (node.mount->ops->stat)
                e = node.mount->ops->stat(&node);
        }
    } else
        e = flags & AT_SYMLINK_NOFOLLOW ? vfs_lstat(path, &node) : vfs_lookup(path, &node);
    if (!e && changed) {
        if (node.mount->readonly)
            e = -E_ROFS;
        else if (!node.mount->ops->set_times)
            e = -E_NOTSUP;
        else
            e = node.mount->ops->set_times(&node, times[0].sec, times[1].sec, changed);
    }
    mutex_unlock(&vfs_mutex);
    return e;
}

static int resize_node(struct vnode *node, uint64_t size) {
    if ((int64_t)size < 0 || (node->mode & 0170000) != S_IFREG)
        return -E_INVAL;
    if (node->mount->readonly)
        return -E_ROFS;
    if (!(node->mode & 0222))
        return -E_ACCES;
    if (node->mount->ops->resize)
        return node->mount->ops->resize(node, size);
    return !size && node->mount->ops->truncate ? node->mount->ops->truncate(node) : -E_NOTSUP;
}

static long dispatch(struct task *t, register_t *r) {
    if (t->vfork_parent && (r->rax == SYS_brk || r->rax == SYS_mmap || r->rax == SYS_mprotect ||
                            r->rax == SYS_munmap || r->rax == SYS_mremap))
        return -E_NOTSUP;
    switch (r->rax) {
    case SYS_futex:
        return task_futex(t, r);

    case SYS_epoll_ctl:
        return epoll_control(t, r);

    case SYS_epoll_wait:
        return epoll_wait_call(t, r);

    case SYS_epoll_pwait: {
        uint64_t previous = t->signal_mask, mask;
        if (r->r8) {
            if (r->r9 != 8)
                return -E_INVAL;
            if (!user_copy_in(&t->space, &mask, r->r8, 8))
                return -E_FAULT;
            t->signal_mask = mask & ~((1ULL << 8) | (1ULL << 18));
        }
        long result = epoll_wait_call(t, r);
        if (result != -E_INTR)
            t->signal_mask = previous;
        return result;
    }

    case SYS_epoll_create:
    case SYS_epoll_create1: {
        if (r->rax == SYS_epoll_create ? (int)r->rdi <= 0 : r->rdi & ~(uint64_t)O_CLOEXEC)
            return -E_INVAL;
        unsigned fd = 0;
        while (fd < OS_FD_MAX && t->fds[fd])
            fd++;
        if (fd == OS_FD_MAX)
            return -E_MFILE;
        mutex_lock(&vfs_mutex);
        int error = vfs_epoll(&t->fds[fd]);
        if (!error)
            t->fd_flags[fd] = r->rax == SYS_epoll_create1 && (r->rdi & O_CLOEXEC) ? FD_CLOEXEC : 0;
        mutex_unlock(&vfs_mutex);
        return error ? error : (long)fd;
    }

    case SYS_socket:
    case SYS_socketpair:
    case SYS_connect:
    case SYS_bind:
    case SYS_listen:
    case SYS_accept:
    case SYS_accept4:
    case SYS_shutdown:
    case SYS_sendto:
    case SYS_recvfrom:
    case SYS_sendmsg:
    case SYS_recvmsg:
    case SYS_getsockname:
    case SYS_getpeername:
    case SYS_setsockopt:
    case SYS_getsockopt:
        return network_syscall(t, r);

    case SYS_fork:
        return process_clone(t, r, 17, 0);

    case SYS_vfork:
        return process_clone(t, r, 0x4111, 0);

    case SYS_clone:
        return process_clone(t, r, r->rdi, r->rsi);

    case SYS_read:
        return io_call(t, r, false, false);

    case SYS_write:
        return io_call(t, r, true, false);

    case SYS_pread64:
        return io_call(t, r, false, true);

    case SYS_pwrite64:
        return io_call(t, r, true, true);

    case SYS_readv:
        return vector_call(t, r, false);

    case SYS_writev:
        return vector_call(t, r, true);

    case SYS_open:
        return open_call(t, AT_FDCWD, r->rdi, r->rsi, r->rdx);

    case SYS_openat:
        return open_call(t, (int)r->rdi, r->rsi, r->rdx, r->r10);

    case SYS_dup:
        return duplicate(t, r->rdi, 0, false, 0);

    case SYS_memfd_create: {
        if (r->rsi & ~3ULL)
            return -E_INVAL;
        char name[250];
        int error = user_string(&t->space, name, r->rdi, sizeof(name));
        if (error)
            return error == -E_NAMETOOLONG ? -E_INVAL : error;
        unsigned fd = 0;
        while (fd < OS_FD_MAX && t->fds[fd])
            fd++;
        if (fd == OS_FD_MAX)
            return -E_MFILE;
        mutex_lock(&vfs_mutex);
        error = vfs_memfd(r->rsi, t->context->web_domain, &t->fds[fd]);
        if (!error) {
            t->fd_flags[fd] = (r->rsi & 1) ? FD_CLOEXEC : 0;
            if (t->context->web_role == OS_WEB_RENDERER)
                memfd_authorize(t->fds[fd]->memory, t->context->web_domain, t->tgid);
        }
        mutex_unlock(&vfs_mutex);
        return error ? error : (long)fd;
    }

    case SYS_eventfd:
    case SYS_eventfd2: {
        unsigned flags = r->rax == SYS_eventfd2 ? r->rsi : 0;
        if (r->rax == SYS_eventfd2 && r->rsi & ~(1ULL | O_CLOEXEC | O_NONBLOCK))
            return -E_INVAL;
        unsigned fd = 0;
        while (fd < OS_FD_MAX && t->fds[fd])
            fd++;
        if (fd == OS_FD_MAX)
            return -E_MFILE;
        mutex_lock(&vfs_mutex);
        int error = vfs_eventfd(r->rdi, flags, &t->fds[fd]);
        if (!error)
            t->fd_flags[fd] = flags & O_CLOEXEC ? FD_CLOEXEC : 0;
        mutex_unlock(&vfs_mutex);
        return error ? error : (long)fd;
    }

    case SYS_pipe:
        r->rsi = 0;
        __attribute__((fallthrough));

    case SYS_pty:
    case SYS_pipe2: {
        if (r->rsi & ~(O_CLOEXEC | O_NONBLOCK))
            return -E_INVAL;
        if (!user_access(&t->space, r->rdi, 2 * sizeof(int), true))
            return -E_FAULT;
        int fd[2] = {-1, -1};
        for (unsigned i = 0; i < OS_FD_MAX; i++)
            if (!t->fds[i]) {
                if (fd[0] < 0)
                    fd[0] = i;
                else {
                    fd[1] = i;
                    break;
                }
            }
        if (fd[1] < 0)
            return -E_MFILE;
        struct file *ends[2];
        mutex_lock(&vfs_mutex);
        int e = r->rax == SYS_pty ? vfs_pty(ends, r->rsi) : vfs_pipe(ends, r->rsi);
        if (!e)
            for (unsigned i = 0; i < 2; i++) {
                t->fds[fd[i]] = ends[i];
                t->fd_flags[fd[i]] = (r->rsi & O_CLOEXEC) ? FD_CLOEXEC : 0;
            }
        mutex_unlock(&vfs_mutex);
        return e ? e : user_copy_out(&t->space, r->rdi, fd, sizeof(fd)) ? 0 : -E_FAULT;
    }

    case SYS_dup2:
        return duplicate(t, r->rdi, r->rsi, true, 0);

    case SYS_dup3:
        if (r->rdi == r->rsi || r->rdx & ~O_CLOEXEC)
            return -E_INVAL;
        return duplicate(t, r->rdi, r->rsi, true, (r->rdx & O_CLOEXEC) ? FD_CLOEXEC : 0);

    case SYS_fcntl: {
        unsigned fd = r->rdi;
        if (fd >= OS_FD_MAX || !t->fds[fd])
            return -E_BADF;
        if (r->rsi == F_GETFD)
            return t->fd_flags[fd];
        if (r->rsi == F_SETFD) {
            t->fd_flags[fd] = r->rdx & FD_CLOEXEC;
            return 0;
        }
        if (r->rsi == F_GETFL)
            return t->fds[fd]->flags;
        if (r->rsi == F_GETPIPE_SZ || r->rsi == F_SETPIPE_SZ)
            return pipe_capacity(t->fds[fd], r->rsi == F_SETPIPE_SZ, r->rdx);
        if (t->fds[fd]->memory && (r->rsi == 1033 || r->rsi == 1034)) {
            mutex_lock(&vfs_mutex);
            long result = memfd_seals(t->fds[fd]->memory, r->rsi == 1033, r->rdx);
            mutex_unlock(&vfs_mutex);
            return result;
        }
        if (r->rsi == F_SETFL) {
            /* musl preserves the harmless large-file bit in F_SETFL. */
            if (r->rdx & ~(3u | O_APPEND | O_NONBLOCK | O_LARGEFILE))
                return -E_NOTSUP;
            t->fds[fd]->flags =
                (t->fds[fd]->flags & ~(O_APPEND | O_NONBLOCK)) | (r->rdx & (O_APPEND | O_NONBLOCK));
            return 0;
        }
        if (r->rsi == F_DUPFD || r->rsi == F_DUPFD_CLOEXEC) {
            if (r->rdx >= OS_FD_MAX)
                return -E_INVAL;
            return duplicate(t, fd, r->rdx, false, r->rsi == F_DUPFD_CLOEXEC ? FD_CLOEXEC : 0);
        }
        return -E_NOTSUP;
    }

    case SYS_ioctl:
        return ioctl_call(t, r);

    case SYS_poll:
        return poll_call(t, r, false);

    case SYS_ppoll:
        return poll_call(t, r, true);

    case SYS_select:
        return select_call(t, r, false);

    case SYS_pselect6:
        return select_call(t, r, true);

    case SYS_rt_sigaction: {
        unsigned sig = r->rdi;
        if (!sig || sig > 64 || sig == 9 || sig == 19 || r->r10 != 8)
            return -E_INVAL;
        struct os_sigaction action;
        if (r->rsi && !user_copy_in(&t->space, &action, r->rsi, sizeof(action)))
            return -E_FAULT;
        if (r->rsi &&
            action.flags & ~(SA_SIGINFO | SA_RESTORER | SA_RESTART | SA_NODEFER | SA_RESETHAND))
            return -E_NOTSUP;
        if (r->rsi && action.handler > 1 &&
            (!action.restorer || !(action.flags & SA_RESTORER) ||
             !user_access(&t->space, action.handler, 1, false) ||
             !user_access(&t->space, action.restorer, 1, false)))
            return -E_FAULT;
        if (r->rdx) {
            uint64_t flags = irq_save();
            struct os_sigaction previous = t->signal_actions[sig - 1];
            irq_restore(flags);
            if (!user_copy_out(&t->space, r->rdx, &previous, sizeof(previous)))
                return -E_FAULT;
        }
        if (r->rsi) {
            action.mask &= ~((1ULL << 8) | (1ULL << 18));
            /* Dispositions are shared. Same-CPU delivery must never observe
               a partially copied handler/restorer/mask during preemption. */
            uint64_t flags = irq_save();
            t->signal_actions[sig - 1] = action;
            if (action.handler == 1)
                __atomic_fetch_and(&t->signal_pending, ~(1ULL << (sig - 1)), __ATOMIC_ACQ_REL);
            irq_restore(flags);
        }
        return 0;
    }

    case SYS_rt_sigprocmask: {
        if (r->r10 != 8 || (r->rsi && r->rdi > 2))
            return -E_INVAL;
        uint64_t mask;
        if (r->rsi && !user_copy_in(&t->space, &mask, r->rsi, 8))
            return -E_FAULT;
        if (r->rdx && !user_copy_out(&t->space, r->rdx, &t->signal_mask, 8))
            return -E_FAULT;
        if (r->rsi) {
            if (!r->rdi)
                t->signal_mask |= mask;
            else if (r->rdi == 1)
                t->signal_mask &= ~mask;
            else
                t->signal_mask = mask;
            t->signal_mask &= ~((1ULL << 8) | (1ULL << 18));
        }
        return 0;
    }

    case SYS_rt_sigpending: {
        if (r->rsi != 8)
            return -E_INVAL;
        uint64_t bits = __atomic_load_n(&t->signal_pending, __ATOMIC_ACQUIRE);
        return user_copy_out(&t->space, r->rdi, &bits, 8) ? 0 : -E_FAULT;
    }

    case SYS_rt_sigsuspend: {
        uint64_t mask;
        if (r->rsi != 8)
            return -E_INVAL;
        if (!user_copy_in(&t->space, &mask, r->rdi, 8))
            return -E_FAULT;
        return signal_suspend(t, mask);
    }

    case SYS_pause:
        return signal_suspend(t, t->signal_mask);

    case SYS_rt_sigreturn:
        return signal_return(t, r);

    case SYS_umask: {
        unsigned old = t->context->creation_mask;
        t->context->creation_mask = r->rdi & 0777;
        return old;
    }

    case SYS_fchdir: {
        if (r->rdi >= OS_FD_MAX || !t->fds[r->rdi])
            return -E_BADF;
        if ((t->fds[r->rdi]->node.mode & 0170000) != S_IFDIR)
            return -E_NOTDIR;
        strcpy(t->cwd, t->fds[r->rdi]->path);
        return 0;
    }

    case SYS_readlink:
    case SYS_readlinkat: {
        char path[OS_PATH_MAX], buffer[OS_PATH_MAX];
        uint64_t ptr = r->rax == SYS_readlink ? r->rsi : r->rdx,
                 size = r->rax == SYS_readlink ? r->rdx : r->r10;
        if (!size)
            return -E_INVAL;
        if (!user_access(&t->space, ptr, size, true))
            return -E_FAULT;
        int e = path_at(t, r->rax == SYS_readlink ? AT_FDCWD : (int)r->rdi,
                        r->rax == SYS_readlink ? r->rdi : r->rsi, path);
        if (e)
            return e;
        mutex_lock(&vfs_mutex);
        long n = vfs_readlink(path, buffer, min(size, sizeof(buffer)));
        mutex_unlock(&vfs_mutex);
        return n > 0 ? (user_copy_out(&t->space, ptr, buffer, n) ? n : -E_FAULT) : n;
    }

    case SYS_symlink:
    case SYS_symlinkat: {
        char target[OS_PATH_MAX], path[OS_PATH_MAX];
        int e = user_string(&t->space, target, r->rdi, sizeof(target));
        if (e)
            return e;
        e = path_at(t, r->rax == SYS_symlink ? AT_FDCWD : (int)r->rsi,
                    r->rax == SYS_symlink ? r->rsi : r->rdx, path);
        if (e)
            return e;
        mutex_lock(&vfs_mutex);
        e = vfs_symlink(target, path);
        mutex_unlock(&vfs_mutex);
        return e;
    }

    case SYS_utime:
    case SYS_utimes:
    case SYS_futimesat:
    case SYS_utimensat:
        return timestamp_call(t, r);

    case SYS_getrlimit:
    case SYS_prlimit64: {
        bool extended = r->rax == SYS_prlimit64;
        /* Limits remain fixed. musl queries prlimit64 before its getrlimit
           fallback, including when WebKit discovers the main stack bounds. */
        if (extended && ((r->rdi && r->rdi != t->tgid) || r->rdx))
            return -E_NOTSUP;
        uint64_t resource = extended ? r->rsi : r->rdi, output = extended ? r->r10 : r->rsi;
        if (resource > 15)
            return -E_INVAL;
        uint64_t limit[2] = {UINT64_MAX, UINT64_MAX};
        if (resource == 3)
            limit[0] = limit[1] = USER_STACK_SIZE;
        if (resource == 2)
            limit[0] = limit[1] = USER_MAX_ALLOC;
        if (resource == 9 && t->space.vm->page_limit)
            limit[0] = limit[1] = t->space.vm->page_limit * PAGE_SIZE;
        if (resource == 4)
            limit[0] = limit[1] = 0;
        if (resource == 7)
            limit[0] = limit[1] = OS_FD_MAX;
        if (extended && !output)
            return 0;
        return user_copy_out(&t->space, output, limit, sizeof(limit)) ? 0 : -E_FAULT;
    }

    case SYS_ftruncate: {
        if (r->rdi >= OS_FD_MAX || !t->fds[r->rdi] || !(t->fds[r->rdi]->flags & 3))
            return -E_BADF;
        struct file *f = t->fds[r->rdi];
        if (f->tty || f->pipe || f->pty || (f->node.mode & 0170000) != S_IFREG)
            return -E_INVAL;
        mutex_lock(&vfs_mutex);
        int e = sandbox_file(f, SANDBOX_WRITE);
        if (!e)
            e = f->memory ? memfd_resize(f->memory, r->rsi) : resize_node(&f->node, r->rsi);
        mutex_unlock(&vfs_mutex);
        return e;
    }

    case SYS_truncate: {
        char path[OS_PATH_MAX];
        int e = path_argument(t, r->rdi, path);
        if (e)
            return e;
        mutex_lock(&vfs_mutex);
        struct vnode node;
        e = vfs_lookup_write(path, &node);
        if (!e)
            e = resize_node(&node, r->rsi);
        mutex_unlock(&vfs_mutex);
        return e;
    }

    case SYS_close:
        if (r->rdi >= OS_FD_MAX || !t->fds[r->rdi])
            return -E_BADF;
        mutex_lock(&vfs_mutex);
        if (!r->rdi)
            task_release_input(t, t->fds[r->rdi]);
        vfs_close(t->fds[r->rdi]);
        t->fds[r->rdi] = NULL;
        t->fd_flags[r->rdi] = 0;
        mutex_unlock(&vfs_mutex);
        return 0;

    case SYS_stat:
    case SYS_lstat:
        return stat_call(t, r, false);

    case SYS_fstat:
        return stat_call(t, r, true);

    case SYS_getdents64:
        return directory_call(t, r);

    case SYS_lseek: {
        if (r->rdi >= OS_FD_MAX || !t->fds[r->rdi])
            return -E_BADF;
        struct file *f = t->fds[r->rdi];
        if (f->tty || f->pipe || f->pty || f->device || f->socket || f->unix_socket || f->event ||
            f->epoll)
            return -E_SPIPE;
        mutex_lock(&vfs_mutex);
        if (f->memory)
            f->node.size = memfd_size(f->memory);
        else if (f->node.mount->ops->stat)
            f->node.mount->ops->stat(&f->node);
        int64_t off = r->rsi, base;
        if (r->rdx == 0)
            base = 0;
        else if (r->rdx == 1)
            base = f->offset;
        else if (r->rdx == 2)
            base = f->node.size;
        else {
            mutex_unlock(&vfs_mutex);
            return -E_INVAL;
        }
        if (base < 0 || (off < 0 && off < -base) || (off > 0 && off > INT64_MAX - base)) {
            mutex_unlock(&vfs_mutex);
            return -E_INVAL;
        }
        f->offset = base + off;
        mutex_unlock(&vfs_mutex);
        return f->offset;
    }

    case SYS_mmap:
        return mmap_call(t, r);

    case SYS_mremap:
        return mremap_call(t, r);

    case SYS_mprotect: {
        uint64_t addr = r->rdi, size = r->rsi, prot = r->rdx;
        if ((addr & 4095) || !size || size > USER_MAX_ALLOC || prot & ~7ULL ||
            (prot && !(prot & 1)) || (prot & 6) == 6 || addr < USER_MIN || addr >= USER_TOP ||
            size > USER_TOP - addr)
            return -E_INVAL;
        size = align(size, 4096ULL);
        unsigned flags =
            ((prot & 1) ? VM_READ : 0) | ((prot & 2) ? VM_WRITE : 0) | ((prot & 4) ? VM_EXEC : 0);
        for (uint64_t va = addr; va < addr + size; va += PAGE_SIZE)
            if (!user_protect(&t->space, va, flags))
                return -E_NOMEM;
        return 0;
    }

    case SYS_munmap:
        if ((r->rdi & 4095) || !r->rsi || r->rdi < USER_MIN || r->rdi >= USER_TOP ||
            r->rsi > USER_TOP - r->rdi || r->rsi > USER_MAX_ALLOC)
            return -E_INVAL;
        for (uint64_t va = r->rdi; va < r->rdi + align(r->rsi, 4096ULL); va += PAGE_SIZE)
            user_unmap(&t->space, va);
        return 0;

    case SYS_brk:
        return brk_call(t, r->rdi);

    case SYS_sched_yield:
        sched_yield();
        return 0;

    case SYS_nanosleep: {
        struct os_timespec ts;
        if (!user_copy_in(&t->space, &ts, r->rdi, sizeof(ts)))
            return -E_FAULT;
        if (ts.sec < 0 || ts.sec > 86400 || ts.nsec < 0 || ts.nsec >= 1000000000)
            return -E_INVAL;
        if (r->rsi && !user_access(&t->space, r->rsi, sizeof(ts), true))
            return -E_FAULT;
        uint64_t duration = ts.sec * 1000000ULL + (ts.nsec + 999) / 1000, start = tsc_uptime_us();
        for (;;) {
            uint64_t elapsed = tsc_uptime_us() - start;
            if (elapsed >= duration)
                break;
            unsigned killed = task_pending_signal(t);
            if (killed)
                task_exit(killed);
            if (signal_pending(t)) {
                uint64_t remaining = duration - elapsed;
                ts = (struct os_timespec){remaining / 1000000, (remaining % 1000000) * 1000};
                if (r->rsi && !user_copy_out(&t->space, r->rsi, &ts, sizeof(ts)))
                    return -E_FAULT;
                return -E_INTR;
            }
            sched_sleep(duration - elapsed);
        }
        if (r->rsi) {
            ts = (struct os_timespec){0};
            if (!user_copy_out(&t->space, r->rsi, &ts, sizeof(ts)))
                return -E_FAULT;
        }
        return 0;
    }

    case SYS_getitimer:
    case SYS_setitimer:
    case SYS_alarm: {
        struct os_itimerval in = {0}, old;
        uint64_t value = 0, interval = 0;
        bool set = r->rax != SYS_getitimer;
        if (r->rax == SYS_alarm)
            value = min(r->rdi, UINT32_MAX) * 1000000ULL;
        else {
            if (r->rdi > 2)
                return -E_INVAL;
            if (r->rdi)
                return -E_NOTSUP; /* CPU-time timers are unavailable. */
            if (r->rax == SYS_getitimer) {
                if (!user_access(&t->space, r->rsi, sizeof(old), true))
                    return -E_FAULT;
            } else {
                if (!user_copy_in(&t->space, &in, r->rsi, sizeof(in)))
                    return -E_FAULT;
                if (r->rdx && !user_access(&t->space, r->rdx, sizeof(old), true))
                    return -E_FAULT;
                if (in.value.sec < 0 || in.value.usec < 0 || in.value.usec >= 1000000 ||
                    in.interval.sec < 0 || in.interval.usec < 0 || in.interval.usec >= 1000000 ||
                    (uint64_t)in.value.sec > (UINT64_MAX - 1000000) / 1000000 ||
                    (uint64_t)in.interval.sec > (UINT64_MAX - 1000000) / 1000000)
                    return -E_INVAL;
                value = (uint64_t)in.value.sec * 1000000 + in.value.usec;
                interval = (uint64_t)in.interval.sec * 1000000 + in.interval.usec;
            }
        }
        task_real_timer(t, set, value, interval, &old);
        if (r->rax == SYS_alarm)
            return old.value.sec + (old.value.usec != 0);
        uint64_t out = r->rax == SYS_getitimer ? r->rsi : r->rdx;
        return !out || user_copy_out(&t->space, out, &old, sizeof(old)) ? 0 : -E_FAULT;
    }

    case SYS_getrandom: {
        if (r->rdx & ~3ULL || r->rsi > IO_LIMIT)
            return -E_INVAL;
        if (!r->rsi)
            return 0;
        if (!user_access(&t->space, r->rdi, r->rsi, true))
            return -E_FAULT;
        unsigned char bytes[128];
        size_t done = 0;
        while (done < r->rsi) {
            long n = random_bytes(bytes, min((size_t)(r->rsi - done), sizeof(bytes)));
            if (n <= 0)
                return done ? (long)done : n;
            if (!user_copy_out(&t->space, r->rdi + done, bytes, n))
                return done ? (long)done : -E_FAULT;
            done += n;
        }
        return done;
    }

    case SYS_clock_gettime: {
        extern int64_t kernel_boot_epoch;
        if (r->rdi > 3)
            return -E_INVAL;
        if (!r->rdi && !kernel_boot_epoch)
            return -E_NOTSUP;
        uint64_t now = tsc_uptime_us();
        struct os_timespec ts = {now / 1000000, (now % 1000000) * 1000};
        if (r->rdi >= 2) {
            uint64_t user, system;
            if (r->rdi == 3)
                task_thread_cpu_usage(t, &user, &system);
            else
                task_cpu_usage(t, false, &user, &system);
            now = user + system;
            ts = (struct os_timespec){now / 1000000, (now % 1000000) * 1000};
        }
        if (!r->rdi)
            ts.sec += kernel_boot_epoch;
        return user_copy_out(&t->space, r->rsi, &ts, sizeof(ts)) ? 0 : -E_FAULT;
    }

    case SYS_gettimeofday:
    case SYS_time: {
        extern int64_t kernel_boot_epoch;
        if (!kernel_boot_epoch)
            return -E_NOTSUP;
        uint64_t now = tsc_uptime_us();
        struct os_timeval tv = {kernel_boot_epoch + now / 1000000, now % 1000000};
        if (r->rax == SYS_time)
            return !r->rdi || user_copy_out(&t->space, r->rdi, &tv.sec, 8) ? tv.sec : -E_FAULT;
        if (r->rsi)
            return -E_NOTSUP;
        return !r->rdi || user_copy_out(&t->space, r->rdi, &tv, sizeof(tv)) ? 0 : -E_FAULT;
    }

    case SYS_clock_getres: {
        if (r->rdi > 3)
            return -E_INVAL;
        struct os_timespec ts = {0, r->rdi >= 2 ? 1000000 : 1000};
        return !r->rsi || user_copy_out(&t->space, r->rsi, &ts, sizeof(ts)) ? 0 : -E_FAULT;
    }

    case SYS_getrusage: {
        int who = (int)r->rdi;
        if (who != -1 && who != 0 && who != 1)
            return -E_INVAL;
        uint64_t user, system;
        if (who == 1)
            task_thread_cpu_usage(t, &user, &system);
        else
            task_cpu_usage(t, who == -1, &user, &system);
        struct os_rusage usage = {.user = {user / 1000000, user % 1000000},
                                  .system = {system / 1000000, system % 1000000}};
        usage.other[0] = (who == -1 ? t->context->child_peak_pages : t->space.vm->peak_pages) *
                         (PAGE_SIZE / 1024);
        return user_copy_out(&t->space, r->rsi, &usage, sizeof(usage)) ? 0 : -E_FAULT;
    }

    case SYS_times: {
        uint64_t user, system, child_user, child_system;
        task_cpu_usage(t, false, &user, &system);
        task_cpu_usage(t, true, &child_user, &child_system);
        int64_t times[4] = {user / 10000, system / 10000, child_user / 10000, child_system / 10000};
        if (r->rdi && !user_copy_out(&t->space, r->rdi, times, sizeof(times)))
            return -E_FAULT;
        return tsc_uptime_us() / 10000; /* musl's CLK_TCK=100 */
    }

    case SYS_getpid:
        return t->tgid;

    case SYS_gettid:
        return t->pid;

    case SYS_getuid:
    case SYS_geteuid:
    case SYS_getgid:
    case SYS_getegid:
        return 0;

    case SYS_set_tid_address:
        if (r->rdi && !user_access(&t->space, r->rdi, 4, true))
            return -E_FAULT;
        t->clear_tid = r->rdi;
        return t->pid;

    case SYS_arch_prctl: {
        if (r->rdi == 0x1002) {
            if (r->rsi >= USER_TOP)
                return -E_PERM;
            t->fsbase = r->rsi;
            msr(0xc0000100, r->rsi);
            return 0;
        }
        if (r->rdi == 0x1003)
            return user_copy_out(&t->space, r->rsi, &t->fsbase, 8) ? 0 : -E_FAULT;
        return -E_NOTSUP; /* GS remains reserved for safe kernel entry. */
    }

    case SYS_uname: {
        struct os_utsname name = {0};
        strcpy(name.sysname, "ModernOS");
        strcpy(name.nodename, "modernos");
        strcpy(name.release, "0.2");
        strcpy(name.version, "Limine x86-64");
        strcpy(name.machine, "x86_64");
        return user_copy_out(&t->space, r->rdi, &name, sizeof(name)) ? 0 : -E_FAULT;
    }

    case SYS_getppid:
        return t->ppid;

    case SYS_mount: {
        if (r->r10 & ~MS_RDONLY || r->r8)
            return -E_NOTSUP;
        char source[OS_PATH_MAX], target[OS_PATH_MAX], type[OS_PATH_MAX];
        int e = user_string(&t->space, source, r->rdi, sizeof(source));
        if (e)
            return e;
        e = path_argument(t, r->rsi, target);
        if (e)
            return e;
        if (r->rdx) {
            e = user_string(&t->space, type, r->rdx, sizeof(type));
            if (e)
                return e;
        } else
            strcpy(type, "auto");
        mutex_lock(&vfs_mutex);
        e = vfs_mount_device(source, target, type, r->r10);
        mutex_unlock(&vfs_mutex);
        return e;
    }

    case SYS_umount2: {
        char path[OS_PATH_MAX];
        int e = path_argument(t, r->rdi, path);
        if (e)
            return e;
        mutex_lock(&vfs_mutex);
        e = vfs_unmount(path, r->rsi);
        mutex_unlock(&vfs_mutex);
        return e;
    }

    case SYS_access:
    case SYS_faccessat: {
        char path[OS_PATH_MAX];
        unsigned mode = r->rax == SYS_access ? r->rsi : r->rdx;
        if (mode & ~7u)
            return -E_INVAL;
        int e = path_at(t, r->rax == SYS_access ? AT_FDCWD : (int)r->rdi,
                        r->rax == SYS_access ? r->rdi : r->rsi, path);
        if (e)
            return e;
        mutex_lock(&vfs_mutex);
        struct vnode node;
        e = (mode & 2) ? vfs_lookup_write(path, &node) : vfs_lookup(path, &node);
        if (!e && (mode & 2) && node.mount->readonly)
            e = -E_ROFS;
        if (!e && (((mode & 4) && !(node.mode & 0444)) || ((mode & 2) && !(node.mode & 0222)) ||
                   ((mode & 1) && !(node.mode & 0111))))
            e = -E_ACCES;
        mutex_unlock(&vfs_mutex);
        return e;
    }

    case SYS_fsync:
    case SYS_fdatasync:
    case SYS_fchmod: {
        if (r->rdi >= OS_FD_MAX || !t->fds[r->rdi])
            return -E_BADF;
        struct file *file = t->fds[r->rdi];
        if (file->tty || file->pipe || file->pty || file->socket || file->unix_socket ||
            file->event || file->epoll)
            return -E_INVAL;
        if (file->memory)
            return r->rax == SYS_fchmod ? -E_NOTSUP : 0;
        mutex_lock(&vfs_mutex);
        struct mount *m = file->node.mount;
        long e = 0;
        if (r->rax == SYS_fchmod)
            e = m->readonly     ? -E_ROFS
                : m->ops->chmod ? m->ops->chmod(&file->node, r->rsi & 0777)
                                : -E_NOTSUP;
        else if (m->ops->sync)
            e = m->ops->sync(m);
        mutex_unlock(&vfs_mutex);
        return e;
    }

    case SYS_newfstatat:
    case SYS_mkdirat:
    case SYS_unlinkat:
    case SYS_renameat:
    case SYS_renameat2: {
        char path[OS_PATH_MAX], other[OS_PATH_MAX];
        if (r->rax == SYS_newfstatat && r->r10 & ~AT_SYMLINK_NOFOLLOW)
            return -E_NOTSUP;
        if (r->rax == SYS_unlinkat && r->rdx & ~AT_REMOVEDIR)
            return -E_INVAL;
        int e = path_at(t, (int)r->rdi, r->rsi, path);
        if (e)
            return e;
        if (r->rax == SYS_newfstatat) {
            /* Reuse the stat copy/validation path with a temporary user argument
               avoided: resolve directly and return the same ABI layout. */
            struct vnode node;
            mutex_lock(&vfs_mutex);
            e = r->r10 & AT_SYMLINK_NOFOLLOW ? vfs_lstat(path, &node) : vfs_lookup(path, &node);
            struct os_stat st = {0};
            if (!e) {
                st.ino = node.mount->device ? node.id : node.aux[3];
                st.mode = node.mode;
                st.nlink = 1;
                st.size = node.size;
                st.blksize = 4096;
                st.blocks = (node.size + 511) / 512;
                st.dev =
                    node.mount->device ? (uint64_t)(node.mount->device - block_devices) + 1 : 0;
                st.atime_sec = node.atime;
                st.mtime_sec = node.mtime;
                st.ctime_sec = node.ctime;
            }
            mutex_unlock(&vfs_mutex);
            return e ? e : user_copy_out(&t->space, r->rdx, &st, sizeof(st)) ? 0 : -E_FAULT;
        }
        if (r->rax == SYS_renameat || r->rax == SYS_renameat2) {
            if (r->rax == SYS_renameat2 && r->r8 & ~1ULL)
                return -E_NOTSUP;
            e = path_at(t, (int)r->rdx, r->r10, other);
            if (e)
                return e;
        }
        mutex_lock(&vfs_mutex);
        if (r->rax == SYS_mkdirat)
            e = vfs_mkdir(path, (r->rdx & 0777) & ~t->context->creation_mask);
        else if (r->rax == SYS_unlinkat)
            e = vfs_unlink(path, r->rdx & AT_REMOVEDIR);
        else
            e = vfs_rename_flags(path, other, r->rax == SYS_renameat2 && (r->r8 & 1));
        mutex_unlock(&vfs_mutex);
        return e;
    }

    case SYS_exit:
        task_exit((r->rdi & 255) << 8);
        return 0;

    case SYS_exit_group:
        task_exit_group((r->rdi & 255) << 8);
        return 0;

    case SYS_execve:
        return process_call(t, r, true);

    case SYS_spawn:
    case SYS_spawn_io:
    case SYS_spawnve:
    case SYS_spawn_browser:
    case SYS_spawn_web:
        return process_call(t, r, false);

    case SYS_memory_usage: {
        struct os_memory_usage usage = {t->space.vm->pages * PAGE_SIZE,
                                        t->space.vm->peak_pages * PAGE_SIZE,
                                        t->space.vm->page_limit * PAGE_SIZE};
        return user_copy_out(&t->space, r->rdi, &usage, sizeof(usage)) ? 0 : -E_FAULT;
    }

    case SYS_web_identity: {
        struct os_web_identity identity = {
            .role = t->context->web_role, .process = t->tgid, .domain = t->context->web_domain};
        memcpy(identity.origin, t->context->web_origin, sizeof(identity.origin));
        return user_copy_out(&t->space, r->rdi, &identity, sizeof(identity)) ? 0 : -E_FAULT;
    }

    case SYS_browser_grant: {
        char path[OS_PATH_MAX];
        int e = path_argument(t, r->rsi, path);
        if (e)
            return e;
        return task_browser_grant(t->pid, r->rdi, path);
    }

    case SYS_wait4: {
        if ((int)r->rdi < -1 || (int)r->rdi == 0)
            return -E_INVAL;
        if (r->rsi && !user_access(&t->space, r->rsi, 4, true))
            return -E_FAULT;
        if (r->r10 && !user_access(&t->space, r->r10, sizeof(struct os_rusage), true))
            return -E_FAULT;
        struct os_rusage usage = {0};
        int status = 0;
        int pid = task_wait_usage((int)r->rdi, &status, r->rdx, &usage);
        if (pid > 0) {
            bool copied = (!r->rsi || user_copy_out(&t->space, r->rsi, &status, 4)) &&
                          (!r->r10 || user_copy_out(&t->space, r->r10, &usage, sizeof(usage)));
            bool owned = tty_is_foreground(pid);
            tty_transfer_foreground(pid, t->tgid);
            if (owned && (status & 127))
                tty_reset_config(t->tgid);
            struct file *input = task_file_pin(t, 0);
            if (input && input->pty)
                pty_foreground(input->pty, pid, t->tgid);
            if (!copied)
                return -E_FAULT;
        }
        return pid;
    }

    case SYS_kill:
        return task_kill(r->rdi, r->rsi);

    case SYS_tkill:
        return task_tgkill(0, r->rdi, r->rsi);

    case SYS_tgkill:
        return r->rdi ? task_tgkill(r->rdi, r->rsi, r->rdx) : -E_INVAL;

    case SYS_getcwd: {
        size_t len = strlen(t->cwd) + 1;
        if (len > r->rsi)
            return -E_NAMETOOLONG;
        return user_copy_out(&t->space, r->rdi, t->cwd, len) ? (long)len : -E_FAULT;
    }

    case SYS_chdir:
    case SYS_mkdir:
    case SYS_rmdir:
    case SYS_unlink:
    case SYS_chmod:
    case SYS_rename: {
        char path[OS_PATH_MAX], other[OS_PATH_MAX];
        int e = path_argument(t, r->rdi, path);
        if (e)
            return e;
        if (r->rax == SYS_rename) {
            e = path_argument(t, r->rsi, other);
            if (e)
                return e;
        }
        mutex_lock(&vfs_mutex);
        if (r->rax == SYS_chdir) {
            struct vnode node;
            e = vfs_lookup(path, &node);
            if (!e && (node.mode & 0170000) != S_IFDIR)
                e = -E_NOTDIR;
            if (!e)
                strcpy(t->cwd, path);
        } else if (r->rax == SYS_mkdir)
            e = vfs_mkdir(path, (r->rsi & 0777) & ~t->context->creation_mask);
        else if (r->rax == SYS_chmod)
            e = vfs_chmod(path, r->rsi & 0777);
        else if (r->rax == SYS_rename)
            e = vfs_rename(path, other);
        else
            e = vfs_unlink(path, r->rax == SYS_rmdir);
        mutex_unlock(&vfs_mutex);
        return e;
    }

    case SYS_sync: {
        mutex_lock(&vfs_mutex);
        int e = vfs_sync();
        mutex_unlock(&vfs_mutex);
        return e;
    }

    case SYS_sysinfo: {
        if (r->rdi > INFO_NETWORK || r->rdx < 2 || r->rdx > IO_LIMIT)
            return -E_INVAL;
        size_t cap = min(r->rdx, 8192ULL);
        char *buffer = kmalloc(cap);
        if (!buffer)
            return -E_NOMEM;
        size_t n;
        if (r->rdi == INFO_CPUS || r->rdi == INFO_PROCESSES)
            n = sched_info(buffer, cap, r->rdi == INFO_PROCESSES);
        else if (r->rdi == INFO_DISKS)
            n = block_info(buffer, cap);
        else if (r->rdi == INFO_NETWORK)
            n = network_info(buffer, cap);
        else {
            mutex_lock(&vfs_mutex);
            n = r->rdi == INFO_LSBLK ? vfs_lsblk_text(buffer, cap) : vfs_mounts_text(buffer, cap);
            mutex_unlock(&vfs_mutex);
        }
        bool copied = user_copy_out(&t->space, r->rsi, buffer, n);
        kfree(buffer);
        return copied ? (long)n : -E_FAULT;
    }

    case SYS_display:
        return display_call(t, r->rdi, r->rsi, r->rdx);

    case SYS_input:
        return display_input(t, r->rdi, r->rsi);

    case SYS_audio:
        return audio_call(t, r->rdi, r->rsi, r->rdx);

    default:
        return -E_NOSYS;
    }
}

void syscall_dispatch(register_t *r) {
    struct task *t = this_cpu()->current;
    __asm__ volatile("sti" ::: "memory");
    task_reap_threads();
    uint64_t number = r->rax;
    uint64_t previous_mask = t->signal_mask;
    /* Serialize table/VM mutations and short syscalls within a thread group.
       Blocking I/O retains file references instead, allowing a sibling to
       produce data or close/reuse a descriptor while the operation sleeps. */
    bool independent = number == SYS_read || number == SYS_write || number == SYS_pread64 ||
                       number == SYS_pwrite64 || number == SYS_readv || number == SYS_writev ||
                       number == SYS_poll || number == SYS_ppoll || number == SYS_select ||
                       number == SYS_pselect6 || number == SYS_epoll_wait ||
                       number == SYS_epoll_pwait || number == SYS_futex ||
                       number == SYS_sched_yield || number == SYS_nanosleep ||
                       number == SYS_wait4 || number == SYS_exit || number == SYS_exit_group ||
                       number == SYS_rt_sigsuspend || number == SYS_pause ||
                       (number >= SYS_socket && number <= SYS_getsockopt) || number == SYS_accept4;
    if (!independent) {
        mutex_lock(&t->context->syscall_lock);
        t->syscall_locked = true;
    }
    long result;
    int permission = sandbox_syscall(t, r);
    if (permission)
        result = permission;
    else if (t->context->references > 1 &&
             (number == SYS_fork || number == SYS_vfork || number == SYS_execve ||
              number == SYS_spawn || number == SYS_spawn_io || number == SYS_spawnve ||
              number == SYS_spawn_browser))
        result = -E_NOTSUP;
    else
        result = dispatch(t, r);
    if (t->syscall_locked) {
        t->syscall_locked = false;
        mutex_unlock(&t->context->syscall_lock);
    }
    task_files_unpin(t);
    r->rax = result;
    bool restart =
        result == -E_INTR && (number == SYS_read || number == SYS_write || number == SYS_wait4);
    if (restart)
        r->rcx = number;
    /* A temporary wait mask remains active while its handler executes. Restore
       the caller's old mask only through that handler's saved return frame. */
    uint64_t return_mask =
        result == -E_INTR &&
                (number == SYS_rt_sigsuspend || number == SYS_pause || number == SYS_ppoll ||
                 number == SYS_pselect6 || number == SYS_epoll_pwait)
            ? previous_mask
            : t->signal_mask;
    if (!task_pending_signal(t))
        signal_deliver(t, r, restart, return_mask);
    unsigned signal = task_pending_signal(t);
    if (signal)
        task_exit_group(signal);
    /* SYSRET is safe only with canonical, lower-half return addresses.
       Flags which can affect kernel entry or privilege are never restored. */
    if (r->rip < USER_MIN || r->rip >= USER_TOP || r->user_rsp < USER_MIN ||
        r->user_rsp >= USER_TOP)
        task_exit_group(11);
    r->rflags = (r->rflags & 0x8d5) | 0x202;
    /* The signal frame can originate in a timer interrupt, where RCX/R11
       contain live application registers. The assembly return uses IRET for
       rt_sigreturn so it does not overwrite them with RIP/RFLAGS for SYSRET. */
    if (number == SYS_rt_sigreturn)
        r->interrupt = 257;
}
