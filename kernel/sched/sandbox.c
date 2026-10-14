#include <sandbox.h>
#include <sched.h>
#include <string.h>
#include <fs/memfd.h>
#include <fs/unix_socket.h>

/* The broker supplies normalized ASCII HTTP(S) origins: lower-case DNS host,
   explicit nonzero decimal port, no URL path/userinfo/query/fragment. IDNA and
   registrable-site decisions belong to the engine/broker, never this parser. */
bool sandbox_web_origin(const char *origin) {
    const char *p = origin;
    if (!strncmp(p, "https://", 8))
        p += 8;
    else if (!strncmp(p, "http://", 7))
        p += 7;
    else
        return false;
    unsigned label = 0, host = 0;
    char previous = 0;
    for (; *p && *p != ':'; p++) {
        char c = *p;
        if (c == '.') {
            if (!label || previous == '-')
                return false;
            label = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
            if ((!label && c == '-') || ++label > 63)
                return false;
        } else
            return false;
        if (++host > 253)
            return false;
        previous = c;
    }
    if (!host || !label || previous == '-' || *p++ != ':' || *p < '1' || *p > '9')
        return false;
    unsigned port = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
        port = port * 10 + *p - '0';
        if (port > 65535)
            return false;
    }
    return port != 0;
}

int sandbox_web_init(struct task *t, unsigned role, unsigned domain, const char *origin,
                     const char *home) {
    if (role != OS_WEB_BROKER && role != OS_WEB_RENDERER)
        return -E_INVAL;
    if ((role == OS_WEB_RENDERER && !sandbox_web_origin(origin)) ||
        (role == OS_WEB_BROKER && origin[0]))
        return -E_INVAL;
    t->browser_sandbox = true;
    t->context->web_role = role;
    t->context->web_domain = domain;
    strcpy(t->context->web_origin, origin);
    snprintf(t->browser_home, sizeof(t->browser_home), "%s", home);
    /* Large modern script/CSS bundles need room beyond the static engine.
       The trusted network/storage broker retains the smaller budget. */
    t->space.vm->page_limit = role == OS_WEB_RENDERER ? 196608 : 131072;
    return t->space.vm->pages > t->space.vm->page_limit ? -E_NOMEM : 0;
}

static bool below(const char *path, const char *root) {
    size_t n = strlen(root);
    return n && !strncmp(path, root, n) && (!path[n] || path[n] == '/');
}

static bool ancestor(const char *path, const char *leaf) {
    return !strcmp(path, "/") || below(leaf, path);
}

int sandbox_set_local(struct task *t, const char *local) {
    t->browser_local[0] = 0;
    t->browser_local_file = false;
    if (!local || !local[0])
        return 0;
    /* A desktop-selected document grants its directory for relative images
       and stylesheets. A file at / grants only that file, never all of VFS. */
    char real[OS_PATH_MAX];
    int e = vfs_realpath(local, real);
    if (e)
        return e;
    struct vnode node;
    e = vfs_lookup(real, &node);
    if (e)
        return e;
    if ((node.mode & 0170000) != S_IFREG)
        return -E_ACCES;
    strcpy(t->browser_local, real);
    char *slash = t->browser_local;
    for (char *p = t->browser_local; *p; p++)
        if (*p == '/')
            slash = p;
    if (slash != t->browser_local)
        *slash = 0;
    else
        t->browser_local_file = true;
    return 0;
}

int sandbox_init(struct task *t, const char *home, const char *local) {
    int n = snprintf(t->browser_home, sizeof(t->browser_home), "%s", home);
    if (n < 1 || n >= OS_PATH_MAX || home[0] != '/')
        return -E_INVAL;
    /* Reserve enough room for the longest private-path suffix. */
    if (strlen(home) + sizeof("/.config/netsurf") >= OS_PATH_MAX)
        return -E_NAMETOOLONG;
    int e = sandbox_set_local(t, local);
    if (e)
        return e;
    t->browser_sandbox = true;
    t->space.vm->page_limit = 131072; /* 512 MiB including image, stack and mappings */
    if (t->space.vm->pages > t->space.vm->page_limit)
        return -E_NOMEM;
    return 0;
}

int sandbox_path(const char *path, unsigned access) {
    struct task *t = sched_ready() ? this_cpu()->current : NULL;
    if (!t || !t->browser_sandbox)
        return 0;
    if (t->context->web_role) {
        static const char *const resources[] = {"/usr/share/fonts/TTF",
                                                "/usr/share/modern-browser"};
        if (!strcmp(path, "/dev/null") || !strcmp(path, "/dev/zero"))
            return access & SANDBOX_CREATE ? -E_ACCES : 0;
        if (!(access & SANDBOX_WRITE)) {
            if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random"))
                return 0;
            for (unsigned i = 0; i < sizeof(resources) / sizeof(*resources); i++)
                if (below(path, resources[i]) ||
                    (access == SANDBOX_META && ancestor(path, resources[i])))
                    return 0;
        }
        if (t->context->web_role == OS_WEB_RENDERER)
            return -E_ACCES;
        char config[OS_PATH_MAX], downloads[OS_PATH_MAX];
        snprintf(config, sizeof(config), "%s/.config/webkit", t->browser_home);
        snprintf(downloads, sizeof(downloads), "%s/Downloads", t->browser_home);
        if (below(path, config) || below(path, downloads) ||
            (access == SANDBOX_META && (ancestor(path, config) || ancestor(path, downloads))))
            return 0;
        if (access & SANDBOX_WRITE)
            return -E_ACCES;
        static const char *const files[] = {"/etc/ssl/cert.pem", "/etc/resolv.conf",
                                            "/etc/hosts",        "/etc/services",
                                            "/bin/web-renderer", "/bin/jsc",
                                            "/bin/glib-check",   "/bin/web-capability-check"};
        for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++)
            if (!strcmp(path, files[i]) || (access == SANDBOX_META && ancestor(path, files[i])))
                return 0;
        return -E_ACCES;
    }
    static const char *const files[] = {"/etc/ssl/cert.pem", "/etc/resolv.conf", "/etc/hosts",
                                        "/etc/services",     "/dev/urandom",     "/dev/random",
                                        "/dev/null",         "/dev/zero"};
    static const char *const roots[] = {"/usr/share/netsurf", "/usr/share/fonts/TTF"};
    char config[OS_PATH_MAX], downloads[OS_PATH_MAX];
    snprintf(config, sizeof(config), "%s/.config/netsurf", t->browser_home);
    snprintf(downloads, sizeof(downloads), "%s/Downloads", t->browser_home);
    bool private = below(path, config) || below(path, downloads);
    if (access & SANDBOX_WRITE) {
        if (!private &&
            ((access & SANDBOX_CREATE) || (strcmp(path, "/dev/null") && strcmp(path, "/dev/zero"))))
            return -E_ACCES;
    }
    if (private || !strcmp(path, "/dev/null") || !strcmp(path, "/dev/zero"))
        return 0;
    if (access & SANDBOX_WRITE)
        return -E_ACCES;
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (!strcmp(path, files[i]) || (access == SANDBOX_META && ancestor(path, files[i])))
            return 0;
    for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (below(path, roots[i]) || (access == SANDBOX_META && ancestor(path, roots[i])))
            return 0;
    if (t->browser_local[0] && ((!t->browser_local_file && below(path, t->browser_local)) ||
                                !strcmp(path, t->browser_local) ||
                                (access == SANDBOX_META && ancestor(path, t->browser_local))))
        return 0;
    if (access == SANDBOX_META && (ancestor(path, config) || ancestor(path, downloads)))
        return 0;
    return -E_ACCES;
}

int sandbox_file(const struct file *f, unsigned access) {
    struct task *t = sched_ready() ? this_cpu()->current : NULL;
    if (t && t->context->web_role) {
        unsigned role = t->context->web_role, domain = t->context->web_domain;
        if (f->event) {
            if (f->capability_domain != domain ||
                (role == OS_WEB_RENDERER && f->capability_renderer &&
                 f->capability_renderer != t->tgid))
                return -E_ACCES;
            if (role == OS_WEB_RENDERER)
                ((struct file *)f)->capability_renderer = t->tgid;
            return 0;
        }
        if (f->memory)
            return role == OS_WEB_RENDERER
                       ? (memfd_authorize(f->memory, domain, t->tgid) ? 0 : -E_ACCES)
                       : (memfd_domain(f->memory) == domain ? 0 : -E_ACCES);
        if (f->unix_socket)
            return role == OS_WEB_RENDERER
                       ? (unix_authorize((struct file *)f, domain, t->tgid) ? 0 : -E_ACCES)
                       : (unix_domain((struct file *)f) == domain ? 0 : -E_ACCES);
        if (f->epoll)
            return -E_ACCES;
        if (role == OS_WEB_RENDERER && (f->socket || f->tty || f->pty || f->pipe))
            return -E_ACCES;
    }
    if (f->memory || f->unix_socket)
        return t && t->browser_sandbox ? -E_ACCES : 0;
    if (f->pipe || f->socket || f->event)
        return 0;
    return sandbox_path(f->path, access);
}

int sandbox_syscall(struct task *t, register_t *r) {
    if (!t->browser_sandbox)
        return 0;

    unsigned role = t->context->web_role;
    if (role) {
        switch (r->rax) {
        case SYS_futex:
        case SYS_mremap:
        case SYS_memfd_create:
        case SYS_socketpair:
        case SYS_web_identity:
            return 0;

        case SYS_epoll_create:
        case SYS_epoll_create1:
        case SYS_epoll_ctl:
        case SYS_epoll_wait:
        case SYS_epoll_pwait:
            return 0;

        case SYS_clone:
            return (r->rdi & 0x10f00) == 0x10f00 ? 0 : -E_PERM;

        case SYS_spawn_web:
        case SYS_wait4:
            return role == OS_WEB_BROKER ? 0 : -E_PERM;

        case SYS_audio:
            return role == OS_WEB_BROKER ? 0 : -E_PERM;

        case SYS_socket:
        case SYS_connect:
        case SYS_bind:
            if (role == OS_WEB_RENDERER)
                return -E_PERM;
            break;

        case SYS_sendto:
        case SYS_recvfrom:
        case SYS_sendmsg:
        case SYS_recvmsg:
        case SYS_shutdown:
        case SYS_getsockname:
        case SYS_getpeername:
        case SYS_setsockopt:
        case SYS_getsockopt:
            if (role == OS_WEB_RENDERER) {
                return r->rdi < OS_FD_MAX && t->fds[r->rdi] && t->fds[r->rdi]->unix_socket
                           ? 0
                           : -E_PERM;
            }
            break;

        case SYS_tgkill:
            return r->rdi == t->tgid ? 0 : -E_PERM;

        case SYS_kill:
            /* The scheduler checks the live child and broker domain under its
             * lock. Only SIGKILL may clean up this broker's renderers. */
            return r->rdi == t->tgid || (role == OS_WEB_BROKER && r->rdi && r->rsi == 9) ? 0
                                                                                         : -E_PERM;

        default:
            break;
        }
    }

    /* Default deny: adding a kernel syscall cannot expand browser authority. */
    switch (r->rax) {
    case SYS_fcntl:
        /* Page code cannot grow inherited kernel buffers outside its quota.
         * Only the trusted broker may size its bounded output pipes. */
        return r->rsi == F_SETPIPE_SZ && role != OS_WEB_BROKER ? -E_PERM : 0;

    case SYS_prlimit64:
        return (!r->rdi || r->rdi == t->tgid) && !r->rdx ? 0 : -E_PERM;

    case SYS_read:
    case SYS_write:
    case SYS_pread64:
    case SYS_pwrite64:
    case SYS_readv:
    case SYS_writev:
    case SYS_open:
    case SYS_openat:
    case SYS_close:
    case SYS_stat:
    case SYS_lstat:
    case SYS_fstat:
    case SYS_newfstatat:
    case SYS_lseek:
    case SYS_access:
    case SYS_faccessat:
    case SYS_getdents64:
    case SYS_readlink:
    case SYS_readlinkat:
    case SYS_getcwd:
    case SYS_chdir:
    case SYS_dup:
    case SYS_dup2:
    case SYS_dup3:
    case SYS_mmap:
    case SYS_munmap:
    case SYS_mprotect:
    case SYS_brk:
    case SYS_rt_sigaction:
    case SYS_rt_sigprocmask:
    case SYS_rt_sigpending:
    case SYS_rt_sigreturn:
    case SYS_rt_sigsuspend:
    case SYS_pause:
    case SYS_getitimer:
    case SYS_setitimer:
    case SYS_alarm:
    case SYS_sched_yield:
    case SYS_nanosleep:
    case SYS_clock_gettime:
    case SYS_clock_getres:
    case SYS_gettimeofday:
    case SYS_time:
    case SYS_getrusage:
    case SYS_times:
    case SYS_getrlimit:
    case SYS_memory_usage:
    case SYS_getpid:
    case SYS_getppid:
    case SYS_gettid:
    case SYS_getuid:
    case SYS_getgid:
    case SYS_geteuid:
    case SYS_getegid:
    case SYS_uname:
    case SYS_arch_prctl:
    case SYS_set_tid_address:
    case SYS_getrandom:
    case SYS_poll:
    case SYS_ppoll:
    case SYS_select:
    case SYS_pselect6:
    case SYS_socket:
    case SYS_connect:
    case SYS_sendto:
    case SYS_recvfrom:
    case SYS_sendmsg:
    case SYS_recvmsg:
    case SYS_shutdown:
    case SYS_getsockname:
    case SYS_getpeername:
    case SYS_setsockopt:
    case SYS_getsockopt:
    case SYS_eventfd:
    case SYS_eventfd2:
    case SYS_pipe:
    case SYS_pipe2:
    case SYS_mkdir:
    case SYS_mkdirat:
    case SYS_unlink:
    case SYS_unlinkat:
    case SYS_rmdir:
    case SYS_rename:
    case SYS_renameat:
    case SYS_renameat2:
    case SYS_ftruncate:
    case SYS_truncate:
    case SYS_fsync:
    case SYS_fdatasync:
    case SYS_umask:
    case SYS_exit:
    case SYS_exit_group:
        return 0;

    case SYS_ioctl:
        return r->rdi < OS_FD_MAX && t->fds[r->rdi] &&
                       (t->fds[r->rdi]->socket || (role && t->fds[r->rdi]->unix_socket))
                   ? 0
                   : -E_PERM;

    case SYS_bind: {
        struct {
            uint16_t family, port;
            uint32_t address;
            uint8_t rest[8];
        } addr;
        if (r->rdx != sizeof(addr) || !user_copy_in(&t->space, &addr, r->rsi, sizeof(addr)))
            return -E_PERM;
        return addr.family == 2 && !addr.port && !addr.address ? 0 : -E_PERM;
    }

    case SYS_kill:
    case SYS_tkill:
        return r->rdi == t->pid ? 0 : -E_PERM;

    case SYS_tgkill:
        return r->rdi == t->pid && r->rsi == t->pid ? 0 : -E_PERM;

    default:
        return -E_PERM;
    }
}
