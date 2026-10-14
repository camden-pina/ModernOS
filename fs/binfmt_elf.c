#include <sched.h>
#include <mm/pmm.h>
#include <descriptor_tables/gdt.h>
#include <tty.h>
#include <string.h>
#include <video/display.h>
#include <pty.h>
#include <sandbox.h>
#include <fs/unix_socket.h>
#include <fs/memfd.h>

struct elf_header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct program_header {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

static long file_at(struct file *file, uint64_t off, void *data, size_t size) {
    file->offset = off;
    return vfs_read(file, data, size);
}

static int load_elf(const char *path, char *const argv[], char *const env[],
                    struct address_space *space, register_t *frame) {
    struct file *file = NULL;
    uint64_t *words = NULL;
    int result;

    mutex_lock(&vfs_mutex);
    result = vfs_open(path, O_RDONLY, 0, &file);
    if (result)
        goto finish;
    if ((file->node.mode & 0170000) != S_IFREG || !(file->node.mode & 0111)) {
        result = -E_ACCES;
        goto finish;
    }

    struct elf_header h;
    if (file_at(file, 0, &h, sizeof(h)) != sizeof(h) || memcmp(h.ident, "\177ELF", 4) ||
        h.ident[4] != 2 || h.ident[5] != 1 || h.ident[6] != 1 || h.type != 2 || h.machine != 62 ||
        h.version != 1 || h.ehsize != sizeof(h) || h.phentsize != sizeof(struct program_header) ||
        !h.phnum || h.phnum > 64 || h.phoff > file->node.size ||
        (uint64_t)h.phnum * sizeof(struct program_header) > file->node.size - h.phoff) {
        result = -E_NOEXEC;
        goto finish;
    }

    struct program_header ph[64];
    if (file_at(file, h.phoff, ph, h.phnum * sizeof(*ph)) != (long)(h.phnum * sizeof(*ph))) {
        result = -E_IO;
        goto finish;
    }

    bool executable_entry = false;
    uint64_t image_end = USER_MIN, mapped = 0, phdr = 0;
    for (unsigned i = 0; i < h.phnum; i++) {
        struct program_header *p = &ph[i];
        if (p->type == 3 || p->type == 2) {
            result = -E_NOEXEC;
            goto finish;
        } /* interpreter/dynamic */

        if (p->type == 7) {
            /* Static libc initializes TLS from AT_PHDR. Its template must live
               entirely in a validated, mapped load segment. */
            bool covered = false;
            for (unsigned j = 0; j < h.phnum; j++)
                if (ph[j].type == 1 && p->vaddr >= ph[j].vaddr && p->memsz <= ph[j].memsz &&
                    p->vaddr - ph[j].vaddr <= ph[j].memsz - p->memsz)
                    covered = true;
            if (!covered || p->filesz > p->memsz || p->memsz > USER_MAX_IMAGE ||
                (p->align && (p->align & (p->align - 1)))) {
                result = -E_NOEXEC;
                goto finish;
            }
        }

        if (p->type == 0x6474e551 && (p->flags & 1)) {
            result = -E_NOEXEC;
            goto finish;
        }
        if (p->type != 1 || !p->memsz)
            continue;
        if (p->filesz > p->memsz || p->offset > file->node.size ||
            p->filesz > file->node.size - p->offset || p->vaddr < USER_MIN ||
            p->vaddr >= USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE ||
            p->memsz > USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE - p->vaddr ||
            (p->flags & 3) == 3 ||
            (p->align > 1 &&
             ((p->align & (p->align - 1)) || ((p->vaddr - p->offset) & (p->align - 1))))) {
            result = -E_NOEXEC;
            goto finish;
        }

        uint64_t first = p->vaddr & ~4095ULL, last = align(p->vaddr + p->memsz, 4096ULL);
        mapped += last - first;
        if (mapped > USER_MAX_IMAGE) {
            result = -E_FBIG;
            goto finish;
        }
        for (unsigned j = 0; j < i; j++)
            if (ph[j].type == 1 && ph[j].memsz &&
                first < align(ph[j].vaddr + ph[j].memsz, 4096ULL) &&
                (ph[j].vaddr & ~4095ULL) < last) {
                result = -E_NOEXEC;
                goto finish;
            }

        if (h.entry >= p->vaddr && h.entry < p->vaddr + p->memsz && (p->flags & 1))
            executable_entry = true;
        if (h.phoff >= p->offset && h.phoff + h.phnum * sizeof(*ph) <= p->offset + p->filesz)
            phdr = p->vaddr + h.phoff - p->offset;
        image_end = max(image_end, last);
    }

    if (!executable_entry) {
        result = -E_NOEXEC;
        goto finish;
    }
    if (!user_space_create(space)) {
        result = -E_NOMEM;
        goto finish;
    }

    uint8_t buffer[4096];
    for (unsigned i = 0; i < h.phnum; i++) {
        struct program_header *p = &ph[i];
        if (p->type != 1 || !p->memsz)
            continue;

        unsigned flags = VM_READ | ((p->flags & 2) ? VM_WRITE : 0) | ((p->flags & 1) ? VM_EXEC : 0);
        for (uint64_t va = p->vaddr & ~4095ULL; va < align(p->vaddr + p->memsz, 4096ULL);
             va += PAGE_SIZE)
            if (!user_map(space, va, flags)) {
                result = -E_NOMEM;
                goto finish;
            }

        for (uint64_t off = 0; off < p->filesz;) {
            size_t n = min((uint64_t)sizeof(buffer), p->filesz - off);
            if (file_at(file, p->offset + off, buffer, n) != (long)n ||
                !user_load_bytes(space, p->vaddr + off, buffer, n)) {
                result = -E_IO;
                goto finish;
            }
            off += n;
        }
    }

    for (uint64_t va = USER_STACK_TOP - USER_STACK_SIZE; va < USER_STACK_TOP; va += PAGE_SIZE)
        if (!user_map(space, va, VM_RDWR)) {
            result = -E_NOMEM;
            goto finish;
        }

    /* SysV initial stack: argc, argv, actual envp strings, auxiliary vector. */
    words = kmalloc((OS_ARG_MAX + OS_ENV_MAX + 32) * sizeof(*words));
    if (!words) {
        result = -E_NOMEM;
        goto finish;
    }

    uint64_t sp = USER_STACK_TOP;
    unsigned argc = 0, envc = 0, n = 1;
    while (argv && argv[argc]) {
        if (argc == OS_ARG_MAX) {
            result = -E_2BIG;
            goto finish;
        }
        size_t len = strlen(argv[argc]) + 1;
        if (len > OS_ARG_STRING_MAX || sp - len < USER_STACK_TOP - USER_STACK_SIZE + 4096) {
            result = -E_2BIG;
            goto finish;
        }
        sp -= len;
        words[n++] = sp;
        user_load_bytes(space, sp, argv[argc], len);
        argc++;
    }

    words[0] = argc;
    words[n++] = 0;

    while (env && env[envc]) {
        if (envc == OS_ENV_MAX) {
            result = -E_2BIG;
            goto finish;
        }
        size_t len = strlen(env[envc]) + 1;
        if (len > OS_ARG_STRING_MAX || sp - len < USER_STACK_TOP - USER_STACK_SIZE + 4096) {
            result = -E_2BIG;
            goto finish;
        }
        sp -= len;
        words[n++] = sp;
        user_load_bytes(space, sp, env[envc], len);
        envc++;
    }
    words[n++] = 0;

    words[n++] = 6;
    words[n++] = PAGE_SIZE; /* AT_PAGESZ */
    words[n++] = 9;
    words[n++] = h.entry; /* AT_ENTRY */

    if (phdr) {
        words[n++] = 3;
        words[n++] = phdr;
        words[n++] = 4;
        words[n++] = sizeof(*ph);
        words[n++] = 5;
        words[n++] = h.phnum;
    }
    words[n++] = 0;
    words[n++] = 0;

    sp = (sp - n * sizeof(uint64_t)) & ~15ULL;
    user_load_bytes(space, sp, words, n * sizeof(uint64_t));

    space->vm->brk_base = space->vm->brk_end = image_end;
    space->vm->mmap_next = max(0x100000000ULL, align(image_end + USER_MAX_IMAGE, 4096ULL));
    *frame =
        (register_t){.rip = h.entry, .cs = USER_CS, .ss = USER_DS, .rflags = 0x202, .user_rsp = sp};
    result = 0;

finish:
    kfree(words);
    if (file)
        vfs_close(file);
    mutex_unlock(&vfs_mutex);
    if (result && space->root)
        user_space_destroy(space);

    return result;
}

/* Resolve bounded, executable #! chains before entering the ELF loader. Keep
   each rewritten argv in heap storage: recursive ELF-loader stack frames
   would exhaust a kernel stack while constructing a compiler's argument list. */
static int load(const char *path, char *const argv[], char *const env[],
                struct address_space *space, register_t *frame) {
    struct script_frame {
        char line[257], path[OS_PATH_MAX], interpreter[OS_PATH_MAX];
        char *arguments[OS_ARG_MAX + 1];
    } *scripts = NULL;
    const char *executable = path;
    char *const *args = argv;
    int result = 0;

    for (unsigned depth = 0;; depth++) {
        char line[257];
        long bytes = 0;
        struct file *file = NULL;

        mutex_lock(&vfs_mutex);
        result = vfs_open(executable, O_RDONLY, 0, &file);
        if (!result && ((file->node.mode & 0170000) != S_IFREG || !(file->node.mode & 0111)))
            result = -E_ACCES;
        if (!result) {
            bytes = vfs_read(file, line, 256);
            if (bytes < 0)
                result = (int)bytes;
        }
        if (file)
            vfs_close(file);
        mutex_unlock(&vfs_mutex);
        if (result)
            break;

        if (bytes < 2 || line[0] != '#' || line[1] != '!') {
            result = load_elf(executable, (char *const *)args, env, space, frame);
            break;
        }
        if (depth == 4) {
            result = -E_LOOP;
            break;
        }

        char *end = memchr(line, '\n', bytes);
        if (!end && bytes == 256) {
            result = -E_NOEXEC;
            break;
        }
        size_t length = end ? (size_t)(end - line) : (size_t)bytes;
        if (memchr(line, 0, length)) {
            result = -E_NOEXEC;
            break;
        }

        line[length] = 0;
        while (length &&
               (line[length - 1] == ' ' || line[length - 1] == '\t' || line[length - 1] == '\r'))
            line[--length] = 0;

        if (!scripts) {
            scripts = kmallocz(4 * sizeof(*scripts));
            if (!scripts) {
                result = -E_NOMEM;
                break;
            }
        }
        struct script_frame *s = &scripts[depth];
        strcpy(s->path, executable);
        memcpy(s->line, line, length + 1);

        char *interpreter = s->line + 2;
        while (*interpreter == ' ' || *interpreter == '\t')
            interpreter++;
        char *option = interpreter;
        while (*option && *option != ' ' && *option != '\t')
            option++;
        if (*option) {
            *option++ = 0;
            while (*option == ' ' || *option == '\t')
                option++;
        }

        if (*interpreter != '/') {
            result = -E_NOEXEC;
            break;
        }
        result = vfs_normalize("/", interpreter, s->interpreter);
        if (result)
            break;

        unsigned count = 0;
        s->arguments[count++] = s->interpreter;
        if (*option)
            s->arguments[count++] = option;
        s->arguments[count++] = s->path;

        for (unsigned i = 1; args && args[0] && args[i]; i++) {
            if (count == OS_ARG_MAX) {
                result = -E_2BIG;
                break;
            }
            s->arguments[count++] = args[i];
        }
        if (result)
            break;

        s->arguments[count] = NULL;
        args = s->arguments;
        executable = s->interpreter;
    }

    kfree(scripts);
    return result;
}

void process_cleanup(struct task *t) {
    kfree(t->environment_data);
    t->environment_data = NULL;
    memset(t->environment, 0, sizeof(t->environment));
    t->environment_count = 0;
    display_drop(t->pid);
    task_files_unpin(t);
    if (t->context && !t->context->live) {
        mutex_lock(&vfs_mutex);
        for (unsigned i = 0; i < OS_FD_MAX; i++) {
            vfs_close(t->fds[i]);
            t->fds[i] = NULL;
        }
        mutex_unlock(&vfs_mutex);
    }
    if (t->context && __atomic_fetch_sub(&t->context->references, 1, __ATOMIC_ACQ_REL) == 1) {
        memfd_account_release(t->context->memory_account);
        kfree(t->context);
    }
    t->context = NULL;
    if (t->space.root)
        user_space_destroy(&t->space);
    if (t->borrowed_space.root)
        user_space_destroy(&t->borrowed_space);
}

int process_spawn(const char *path, char *const argv[], unsigned ppid, const char *cwd, int cpu,
                  bool foreground) {
    struct task *parent = this_cpu()->current;
    struct file **streams =
        ppid && parent && parent->pid == ppid && parent->fds[0] && !parent->fds[0]->tty
            ? parent->fds
            : NULL;
    return process_spawn_streams(path, argv, ppid, cwd, cpu, foreground, streams);
}

int process_spawn_streams(const char *path, char *const argv[], unsigned ppid, const char *cwd,
                          int cpu, bool foreground, struct file *const streams[3]) {
    return process_spawn_env(path, argv, NULL, ppid, cwd, cpu, foreground, streams);
}

int process_environment_set(struct task *t, char *const env[]) {
    unsigned count = 0;
    size_t bytes = 0;
    for (; env && env[count]; count++) {
        if (count == OS_ENV_MAX)
            return -E_2BIG;
        size_t n = strlen(env[count]) + 1;
        if (n > OS_ARG_STRING_MAX || n > OS_EXEC_BYTES_MAX - bytes)
            return -E_2BIG;
        bytes += n;
    }

    char *data = bytes ? kmalloc(bytes) : NULL;
    if (bytes && !data)
        return -E_NOMEM;

    char *next[OS_ENV_MAX + 1] = {0};
    size_t offset = 0;
    for (unsigned i = 0; i < count; i++) {
        size_t n = strlen(env[i]) + 1;
        next[i] = data + offset;
        memcpy(next[i], env[i], n);
        offset += n;
    }

    kfree(t->environment_data);
    t->environment_data = data;
    t->environment_count = count;
    memcpy(t->environment, next, sizeof(next));
    return 0;
}

static int spawn(const char *path, char *const argv[], char *const env[], unsigned ppid,
                 const char *cwd, int cpu, bool foreground, struct file *const streams[3],
                 bool browser, const char *local, unsigned web_role, unsigned web_domain,
                 const char *origin) {
    struct task *t = task_allocate(path, true, ppid, cpu);
    if (!t)
        return -E_NOMEM;

    char *inherited[OS_ENV_MAX + 1] = {0};
    static char *defaults[] = {"PATH=/bin",     "HOME=/home/user", "TERM=vt100", "LANG=C.UTF-8",
                               "SHELL=/bin/sh", "USER=user",       NULL};
    struct task *parent = this_cpu()->current;
    if (!env && parent && parent->user) {
        for (unsigned i = 0; i < parent->environment_count && i < OS_ENV_MAX; i++)
            inherited[i] = parent->environment[i];
        env = inherited;
    }
    if (!env)
        env = defaults;

    int e = process_environment_set(t, env);
    if (e) {
        task_discard(t);
        return e;
    }
    t->context->creation_mask = parent && parent->user ? parent->context->creation_mask : 0022;

    struct address_space space = {0};
    register_t frame;
    e = load(path, argv, env, &space, &frame);
    if (e) {
        task_discard(t);
        return e;
    }

    t->space = space;
    *t->frame = frame;

    mutex_lock(&vfs_mutex);
    if (browser) {
        e = sandbox_init(t, cwd, local);
        if (e) {
            mutex_unlock(&vfs_mutex);
            task_discard(t);
            return e;
        }
    }

    if (web_role) {
        unsigned domain = web_role == OS_WEB_BROKER ? t->tgid : web_domain;
        e = sandbox_web_init(t, web_role, domain, origin, cwd);
        if (!e)
            e = unix_bind_domain(streams[0], domain);
        if (!e && web_role == OS_WEB_RENDERER && !unix_authorize(streams[0], domain, t->tgid))
            e = -E_PERM;
        if (e) {
            mutex_unlock(&vfs_mutex);
            task_discard(t);
            return e;
        }
    }

    snprintf(t->cwd, OS_PATH_MAX, "%s", cwd);
    for (unsigned i = 0; i < 3; i++) {
        if (streams) {
            t->fds[i] = streams[i];
            t->fds[i]->references++;
        } else
            e = vfs_open("/dev/tty", i ? O_WRONLY : O_RDONLY, 0, &t->fds[i]);
        if (e)
            break;
    }

    mutex_unlock(&vfs_mutex);
    if (e) {
        task_discard(t);
        return e;
    }

    /* Transfer ownership before another CPU can execute its first read.
       A background parent cannot take the terminal from its current owner. */
    if (foreground && !streams) {
        display_drop(ppid);
        tty_transfer_foreground(ppid, t->pid);
    }
    if (streams && streams[0]->pty)
        pty_foreground(streams[0]->pty, foreground ? ppid : 0, t->pid);

    task_publish(t);
    return t->pid;
}

int process_spawn_env(const char *path, char *const argv[], char *const env[], unsigned ppid,
                      const char *cwd, int cpu, bool foreground, struct file *const streams[3]) {
    return spawn(path, argv, env, ppid, cwd, cpu, foreground, streams, false, NULL, 0, 0, NULL);
}

int process_spawn_browser(const char *path, char *const argv[], unsigned ppid, const char *home,
                          struct file *const streams[3], const char *local) {
    char real_home[OS_PATH_MAX];
    mutex_lock(&vfs_mutex);
    int e = vfs_realpath(home, real_home);
    mutex_unlock(&vfs_mutex);
    if (e)
        return e;
    char setting[OS_PATH_MAX + 5];
    int n = snprintf(setting, sizeof(setting), "HOME=%s", real_home);
    if (n < 0 || n >= (int)sizeof(setting))
        return -E_NAMETOOLONG;
    char *env[] = {"PATH=/bin", setting, "LANG=C.UTF-8", "NETSURFRES=/usr/share/netsurf", NULL};
    return spawn(path, argv, env, ppid, real_home, -1, false, streams, true, local, 0, 0, NULL);
}

int process_spawn_web(const char *path, char *const argv[], unsigned ppid, const char *home,
                      struct file *const streams[3], unsigned role, unsigned domain,
                      const char *origin) {
    char setting[OS_PATH_MAX + 5];
    snprintf(setting, sizeof(setting), "HOME=%s", home);
    char *env[] = {"PATH=/bin", setting, "LANG=C.UTF-8", NULL};
    /* New ELF/VM/context: no forked locks, borrowed broker stack, extra FDs or
       inherited environment. Safe while other broker threads are running. */
    return spawn(path, argv, env, ppid, home, -1, false, streams, false, NULL, role, domain,
                 origin);
}

int process_exec(struct task *t, const char *path, char *const argv[], register_t *frame) {
    return process_exec_env(t, path, argv, NULL, frame);
}

int process_exec_env(struct task *t, const char *path, char *const argv[], char *const env[],
                     register_t *frame) {
    /* Replacing a live thread group's VM needs exec teardown and sibling
       cancellation; do not silently invalidate other threads' mappings. */
    if (t->pid != t->tgid || t->context->references != 1)
        return -E_NOTSUP;

    struct address_space replacement = {0};
    register_t initial;
    int e = load(path, argv, env, &replacement, &initial);
    if (e)
        return e;

    e = process_environment_set(t, env);
    if (e) {
        user_space_destroy(&replacement);
        return e;
    }

    t->clear_tid = 0;
    t->signal_depth = 0;
    t->signal_pending = 0;
    for (unsigned i = 0; i < 64; i++)
        if (t->signal_actions[i].handler != 1)
            memset(&t->signal_actions[i], 0, sizeof(t->signal_actions[i]));

    display_drop(t->pid);
    uint64_t saved = irq_save();
    struct address_space previous = t->space;
    t->space = replacement;
    __asm__ volatile("mov %0,%%cr3" ::"r"(replacement.root) : "memory");
    *frame = initial;
    t->fsbase = 0;
    __asm__ volatile("wrmsr" ::"c"(0xc0000100), "a"(0), "d"(0));
    irq_restore(saved);

    user_space_destroy(&previous);
    __atomic_store_n(&t->vfork_parent, 0, __ATOMIC_RELEASE);

    snprintf(t->name, sizeof(t->name), "%s", path);

    mutex_lock(&vfs_mutex);
    for (unsigned i = 0; i < OS_FD_MAX; i++)
        if (t->fds[i] && (t->fd_flags[i] & FD_CLOEXEC)) {
            vfs_close(t->fds[i]);
            t->fds[i] = NULL;
            t->fd_flags[i] = 0;
        }

    mutex_unlock(&vfs_mutex);

    return 0;
}
