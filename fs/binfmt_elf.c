#include <pidns.h>
#include <filesystem.h>
#include <credentials.h>
#include <accounts.h>
#include <sched.h>
#include <mm/pmm.h>
#include <descriptor_tables/gdt.h>
#include <tty.h>
#include <string.h>
#include <video/display.h>
#include <pty.h>
#include <sandbox.h>
#include <seccomp.h>
#include <landlock.h>
#include <random.h>
#include <fs/unix_socket.h>
#include <fs/memfd.h>
#ifndef CONFIG_ELECTRON_PROFILE
#define CONFIG_ELECTRON_PROFILE 0
#endif
#if CONFIG_ELECTRON_PROFILE
#include <crypto/sha256.h>
#include <printk.h>
#endif
#define ELECTRON_IMAGE_LIMIT (256ULL * 1024 * 1024)
/* Full debugger bootstrap exceeded both6144/6656 MiB during concurrent
   eager fork/exec replacement of Electron helpers. Retain every charge and
   a finite8192-MiB aggregate, with exact-boundary/refund/overflow selftests.
   The full-workbench qualification uses16-GiB guest RAM. */
#define ELECTRON_GROUP_PAGES 2097152ULL

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

/* ET_DYN load biases are deterministic. ASLR is not currently provided. */
#define ELF_PIE_BASE 0x40000000ULL
/* Verified profile only: preserve Chromium's low 4097-MiB guard. */
#define ELF_ELECTRON_BASE 0x400000000ULL
#define ELF_INTERP_BASE 0x200000000ULL
#define ELF_MAX_PHDR 64

struct elf_image {
    struct file *file;
    struct elf_header header;
    struct program_header ph[ELF_MAX_PHDR];
    uint64_t bias, entry, phdr, end, mapped;
    bool dynamic, phdr_record, tls_record, relro_record, electron_profile;
    char interpreter[OS_PATH_MAX];
};

static long file_at(struct file *file, uint64_t off, void *data, size_t size) {
    file->offset = off;
    return vfs_read(file, data, size);
}

/* The same open file is hashed and loaded while vfs_mutex is held. A
   canonical path alone, guest arguments or environment never grant a budget. */
#if CONFIG_ELECTRON_PROFILE_SELFTEST
static unsigned profile_hashes;
#endif
static bool electron_profile_match(struct file *file) {
#if CONFIG_ELECTRON_PROFILE
#ifdef COAST_ELECTRON_SHA256
    static const uint8_t approved[][32]={COAST_ELECTRON_SHA256};
    static const uint8_t companion_node[32]=COAST_NODE_SHA256;
#else
    static const uint8_t approved[][32] = {{
        0xc8,0xf5,0x81,0x9c,0xb4,0xd5,0x16,0x0c,0xee,0x5d,0x95,0x1c,0x4f,0x12,0x3c,0x07,
        0x5f,0x65,0xc6,0x3c,0x89,0x40,0x5a,0x4a,0x67,0x4f,0x56,0x09,0x19,0x17,0x48,0xd6
    }, {
        0x8a,0x01,0x07,0xea,0xd3,0x03,0x0d,0x84,0x3d,0x27,0x43,0xa4,0xe4,0x07,0x07,0xdc,
        0xbe,0x55,0x28,0x3a,0xde,0x46,0xf4,0xc2,0xcc,0x65,0x66,0x0a,0xd5,0x7a,0x6b,0xe5
    }, {
        0xef,0x57,0x99,0x81,0x30,0x04,0xb3,0x8d,0x01,0x5d,0xc0,0x67,0x6c,0xc3,0x57,0x0d,
        0x18,0xac,0x4f,0x80,0xd8,0x3c,0xff,0x0b,0x90,0x51,0xcf,0x52,0xac,0x2a,0xf0,0x5a
    }, {
        0x6b,0x24,0xc4,0x6a,0xaa,0x2d,0x2d,0x4f,0xac,0x8c,0x8f,0xdb,0x20,0x3c,0x98,0x2a,
        0x0c,0xb3,0x75,0xa0,0x3b,0xaa,0x6a,0xd9,0xc3,0x18,0x22,0x61,0x8e,0x47,0xbb,0xfa
    }};
    static const uint8_t companion_node[32]={
        0x66,0x99,0x82,0xa0,0xc8,0xb7,0x6b,0x27,0xab,0xdf,0xde,0x15,0x6f,0x8a,0xa2,0x92,
        0xd8,0x3d,0xbe,0x37,0x14,0x87,0xf6,0xd6,0x79,0x71,0xd4,0x2f,0x26,0xbd,0x87,0x3f
    };
#endif
    bool node_path=!strcmp(file->path,"/opt/electron-43.7.3/node");
    if ((!node_path && strcmp(file->path,"/opt/electron-43.7.3/electron")) ||
        !file->node.size || file->node.size > ELECTRON_IMAGE_LIMIT) return false;
    /* The immutable boot module outlives every process and cannot be written
       through a file or mapping. Hash each approved boot image once. A write,
       truncate or replacement loses this identity and must be hashed again;
       timestamps, pathname and inode-address reuse are never cache keys. */
    static struct { const void *bytes; uint64_t size; bool node; } verified[5];
    const void *boot = ramfs_boot_identity(&file->node);
    if (boot)
        for (unsigned i = 0; i < sizeof(verified)/sizeof(verified[0]); i++)
            if (verified[i].bytes == boot && verified[i].size == file->node.size &&
                verified[i].node == node_path)
                return true;
    uint64_t previous = file->offset;
#if CONFIG_ELECTRON_PROFILE_SELFTEST
    profile_hashes++;
#endif
    uint8_t buffer[4096], digest[32];
    struct sha256 hash;
    sha256_init(&hash);
    file->offset = 0;
    bool ok = true;
    for (uint64_t done = 0; done < file->node.size;) {
        size_t part = min(file->node.size - done, (uint64_t)sizeof(buffer));
        long n = vfs_read(file, buffer, part);
        if (n <= 0 || (uint64_t)n > part || !sha256_update(&hash, buffer, n)) {
            ok = false;
            break;
        }
        done += n;
    }
    file->offset = previous;
    if (!ok) return false;
    sha256_final(&hash, digest);
    /* A separately compiled standalone Node entry uses the same bounded V8
       profile only at its own immutable canonical path and exact digest. */
    bool approved_image = node_path && !memcmp(digest,companion_node,sizeof(digest));
    /* Exact, locally compiled revisions retain the same canonical path.
       The second fixes snapshot-free Node mode and enforces no-JIT helpers.
       The third adds readonly jitless/explicit-bounds Wasm. The fourth rebuilds those same Worker policies for Coast identity. */
    for(unsigned i=0;i<sizeof(approved)/sizeof(approved[0]);i++)
        if(!node_path && !memcmp(digest,approved[i],sizeof(digest)))approved_image=true;
    if (approved_image && boot)
        for (unsigned i = 0; i < sizeof(verified)/sizeof(verified[0]); i++)
            if (!verified[i].bytes) {
                verified[i].bytes=boot; verified[i].size=file->node.size;
                verified[i].node=node_path; break;
            }
    return approved_image;
#else
    (void)file;
    return false;
#endif
}

/* Metadata used by libc must be readable and wholly inside a load segment.
   File-backed metadata must also describe the same file bytes as PT_LOAD. */
static bool elf_contains(const struct elf_image *image, const struct program_header *part,
                         bool file_backed) {
    for (unsigned i = 0; i < image->header.phnum; i++) {
        const struct program_header *p = &image->ph[i];
        if (p->type != 1 || !(p->flags & 4) || part->vaddr < p->vaddr ||
            part->memsz > p->memsz || part->vaddr - p->vaddr > p->memsz - part->memsz)
            continue;
        if (!file_backed ||
            (part->offset >= p->offset && part->filesz <= p->filesz &&
             part->offset - p->offset <= p->filesz - part->filesz &&
             part->vaddr - p->vaddr == part->offset - p->offset))
            return true;
    }
    return false;
}

/* Called under vfs_mutex. Retain the executing inode rather than reopening
   its pathname, which could have been replaced or unlinked after exec. */
static int executable_open(const char *path, struct file **out) {
    struct task *t=sched_ready()?this_cpu()->current:NULL;
    if(!strcmp(path,"/proc/self/exe") && t && !s1_restricted(t) &&
       userns_admitted_account(t) && t->context->executable) {
        struct file *file=t->context->executable;
        int error=sandbox_file(file,SANDBOX_READ);if(error)return error;
        if(landlock_active()) {
            struct landlock_walk walk;struct vnode node;
            error=vfs_landlock_walk(file->path,false,&walk,&node);
            if(!error && (node.mount!=file->node.mount || node.id!=file->node.id))error=-E_ACCES;
            if(!error)error=landlock_walk_check(&walk,LL_EXECUTE|LL_READ_FILE);
            if(error)return error;
        }
        file->references++;file->offset=0;*out=file;return 0;
    }
    return vfs_open_exec(path,out);
}

static int elf_open_image(struct elf_image *image, const char *path, uint64_t bias,
                          bool interpreter) {
    int result = executable_open(path, &image->file);
    if (result)
        return result;
    struct file *file = image->file;
    if ((file->node.mode & 0170000) != S_IFREG || !(file->node.mode & 0111))
        return -E_ACCES;
    result=credentials_node(&file->node,1);
    if(result)return result; /* Applies to PT_INTERP as well as the main ELF. */

    image->electron_profile = !interpreter && electron_profile_match(file);
    uint64_t image_limit = image->electron_profile ? ELECTRON_IMAGE_LIMIT : USER_MAX_IMAGE;
    struct elf_header *h = &image->header;
    if (file_at(file, 0, h, sizeof(*h)) != sizeof(*h) || memcmp(h->ident, "\177ELF", 4) ||
        h->ident[4] != 2 || h->ident[5] != 1 || h->ident[6] != 1 ||
        (h->type != 2 && h->type != 3) || (interpreter && h->type != 3) || h->machine != 62 ||
        h->version != 1 || h->ehsize != sizeof(*h) ||
        h->phentsize != sizeof(struct program_header) || !h->phnum || h->phnum > ELF_MAX_PHDR ||
        h->phoff > file->node.size ||
        (uint64_t)h->phnum * sizeof(*image->ph) > file->node.size - h->phoff)
        return -E_NOEXEC;
    if (file_at(file, h->phoff, image->ph, h->phnum * sizeof(*image->ph)) !=
        (long)(h->phnum * sizeof(*image->ph)))
        return -E_IO;

    image->bias = h->type == 3 ? (image->electron_profile ? ELF_ELECTRON_BASE : bias) : 0;
    if (h->entry > UINT64_MAX - image->bias)
        return -E_NOEXEC;
    image->entry = h->entry + image->bias;
    image->end = USER_MIN;
    bool executable_entry = false;
    uint64_t mapped = 0, phsize = h->phnum * sizeof(*image->ph);

    /* Validate every load segment before examining metadata coverage. */
    for (unsigned i = 0; i < h->phnum; i++) {
        struct program_header *p = &image->ph[i];
        if (p->type == 0x6474e551 && (p->flags & 1))
            return -E_NOEXEC; /* executable stack */
        if (p->type != 1)
            continue;
        if (p->filesz > p->memsz || p->offset > file->node.size ||
            p->filesz > file->node.size - p->offset || p->vaddr > UINT64_MAX - image->bias ||
            (p->flags & 3) == 3 || !(p->flags & 4) ||
            (p->align > 1 &&
             ((p->align & (p->align - 1)) || ((p->vaddr - p->offset) & (p->align - 1)) ||
              (image->bias & (p->align - 1)))))
            return -E_NOEXEC;
        if (!p->memsz)
            continue;
        uint64_t address = p->vaddr + image->bias;
        if (address < USER_MIN || address >= USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE ||
            p->memsz > USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE - address)
            return -E_NOEXEC;
        uint64_t first = address & ~4095ULL, last = align(address + p->memsz, 4096ULL);
        mapped += last - first;
        if (mapped > image_limit)
            return -E_FBIG;
        for (unsigned j = 0; j < i; j++) {
            const struct program_header *q = &image->ph[j];
            if (q->type == 1 && q->memsz &&
                first < align(q->vaddr + image->bias + q->memsz, 4096ULL) &&
                ((q->vaddr + image->bias) & ~4095ULL) < last)
                return -E_NOEXEC;
        }
        if (image->entry >= address && image->entry < address + p->memsz && (p->flags & 1))
            executable_entry = true;
        if (h->phoff >= p->offset && phsize <= p->filesz &&
            h->phoff - p->offset <= p->filesz - phsize)
            image->phdr = address + h->phoff - p->offset;
        image->end = max(image->end, last);
    }
    if (!executable_entry)
        return -E_NOEXEC;

    image->mapped = mapped;
    for (unsigned i = 0; i < h->phnum; i++) {
        struct program_header *p = &image->ph[i];
        if (p->type == 3) { /* PT_INTERP */
            if (interpreter || image->interpreter[0] || p->filesz < 2 ||
                p->filesz > sizeof(image->interpreter) || p->filesz != p->memsz ||
                !elf_contains(image, p, true) ||
                file_at(file, p->offset, image->interpreter, p->filesz) != (long)p->filesz ||
                image->interpreter[0] != '/' || image->interpreter[p->filesz - 1] ||
                memchr(image->interpreter, 0, p->filesz - 1))
                return -E_NOEXEC;
        } else if (p->type == 2) { /* PT_DYNAMIC */
            if (image->dynamic || p->filesz < 16 || p->filesz > 65536 || (p->filesz & 15) ||
                p->filesz > p->memsz || !elf_contains(image, p, true))
                return -E_NOEXEC;
            bool terminated = false;
            for (uint64_t off = 0; off < p->filesz; off += 16) {
                uint64_t entry[2];
                if (file_at(file, p->offset + off, entry, sizeof(entry)) != sizeof(entry))
                    return -E_IO;
                if (!entry[0]) {
                    terminated = true;
                    break;
                }
                if (entry[0] == 22 || (entry[0] == 30 && (entry[1] & 4)))
                    return -E_NOEXEC; /* DT_TEXTREL / DF_TEXTREL */
            }
            if (!terminated)
                return -E_NOEXEC;
            image->dynamic = true;
        } else if (p->type == 7) { /* PT_TLS */
            /* Only .tdata is an image template. The .tbss suffix describes
               per-thread zeroes and need not occupy a PT_LOAD mapping. */
            struct program_header template=*p;
            template.memsz=template.filesz;
            if (image->tls_record || p->filesz > p->memsz || p->memsz > USER_MAX_IMAGE ||
                p->offset>file->node.size || p->filesz>file->node.size-p->offset ||
                p->vaddr>UINT64_MAX-image->bias || p->vaddr+image->bias>=USER_TOP ||
                p->memsz>USER_TOP-(p->vaddr+image->bias) ||
                (p->align && (p->align & (p->align - 1))) ||
                (p->filesz && !elf_contains(image, &template, true)))
                return -E_NOEXEC;
            image->tls_record = true;
        } else if (p->type == 0x6474e552) { /* PT_GNU_RELRO */
            if (image->relro_record || !elf_contains(image, p, false))
                return -E_NOEXEC;
            image->relro_record = true;
        } else if (p->type == 6) { /* PT_PHDR */
            if (image->phdr_record || p->vaddr > UINT64_MAX - image->bias ||
                p->vaddr + image->bias != image->phdr || p->offset != h->phoff ||
                p->filesz != phsize || p->memsz != phsize || !elf_contains(image, p, true))
                return -E_NOEXEC;
            image->phdr_record = true;
        }
    }
    if ((h->type == 3 || image->interpreter[0]) && (!image->dynamic || !image->phdr))
        return -E_NOEXEC;
    /* musl derives a PIE's load bias from AT_PHDR and PT_PHDR. */
    if (h->type == 3 && image->interpreter[0] && !image->phdr_record)
        return -E_NOEXEC;
    return 0;
}

static bool elf_images_overlap(const struct elf_image *a, const struct elf_image *b) {
    for (unsigned i = 0; i < a->header.phnum; i++) {
        const struct program_header *p = &a->ph[i];
        if (p->type != 1 || !p->memsz)
            continue;
        uint64_t first = (p->vaddr + a->bias) & ~4095ULL;
        uint64_t last = align(p->vaddr + a->bias + p->memsz, 4096ULL);
        for (unsigned j = 0; j < b->header.phnum; j++) {
            const struct program_header *q = &b->ph[j];
            if (q->type == 1 && q->memsz &&
                first < align(q->vaddr + b->bias + q->memsz, 4096ULL) &&
                ((q->vaddr + b->bias) & ~4095ULL) < last)
                return true;
        }
    }
    return false;
}

static int elf_map_image(const struct elf_image *image, struct address_space *space) {
    uint8_t buffer[PAGE_SIZE];
    for (unsigned i = 0; i < image->header.phnum; i++) {
        const struct program_header *p = &image->ph[i];
        if (p->type != 1 || !p->memsz)
            continue;
        uint64_t address = p->vaddr + image->bias;
        unsigned flags = VM_READ | ((p->flags & 2) ? VM_WRITE : 0) | ((p->flags & 1) ? VM_EXEC : 0);
        for (uint64_t va = address & ~4095ULL; va < align(address + p->memsz, 4096ULL);
             va += PAGE_SIZE)
            if (!user_map_file(space, va, flags))
                return -E_NOMEM;
        for (uint64_t off = 0; off < p->filesz;) {
            size_t n = min((uint64_t)sizeof(buffer), p->filesz - off);
            if (file_at(image->file, p->offset + off, buffer, n) != (long)n ||
                !user_load_bytes(space, address + off, buffer, n))
                return -E_IO;
            off += n;
        }
    }
    return 0;
}

static bool elf_combined_budget(const struct elf_image *app, const struct elf_image *interp) {
    return !app->electron_profile ||
           (app->mapped <= ELECTRON_IMAGE_LIMIT && interp->mapped <= ELECTRON_IMAGE_LIMIT - app->mapped);
}

static int load_elf(const char *path, char *const argv[], char *const env[],
                    struct address_space *space, register_t *frame,
                    const struct address_space *inherit, uint64_t role_limit, struct file **identity, struct userns_budget_claim *claim, struct exec_credentials *exec_ids) {
    if (identity) *identity = NULL;
    struct elf_image *images = kmallocz(2 * sizeof(*images));
    uint64_t *words = NULL;
    struct user_budget *budget = inherit && inherit->root ? inherit->vm->budget : NULL;
    bool own_budget = false;
    struct userns_budget_claim local_claim={0};
    struct userns_budget_claim *active_claim=claim?claim:&local_claim;
    *active_claim=(struct userns_budget_claim){0};
    int result = -E_NOMEM;
    if (!images)
        return result;
    struct elf_image *app = &images[0], *interp = &images[1];
    mutex_lock(&vfs_mutex);
    result = elf_open_image(app, path, ELF_PIE_BASE, false);
    if (result)
        goto finish;
    if (app->interpreter[0]) {
        result = elf_open_image(interp, app->interpreter, ELF_INTERP_BASE, true);
        if (result)
            goto finish;
        if (elf_images_overlap(app, interp)) {
            result = -E_NOEXEC;
            goto finish;
        }
    }
    if (!elf_combined_budget(app, interp)) {
        result = -E_FBIG;
        goto finish;
    }
    if (app->electron_profile && !budget) {
        budget = userns_budget_existing(sched_ready()?this_cpu()->current:NULL);
        if(!budget)budget = user_budget_create(ELECTRON_GROUP_PAGES);
        if (!budget) { result = -E_NOMEM; goto finish; }
        own_budget = true;
    }
    result=userns_budget_begin(sched_ready()?this_cpu()->current:NULL,budget,active_claim);
    if(result)goto finish;
    if (!user_space_create(space)) {
        result = -E_NOMEM;
        goto finish;
    }
    if (budget) {
        if (!user_space_budget_attach(space, budget, inherit && inherit->root ? inherit->vm->page_limit : 0)) {
            result = -E_NOMEM;
            goto finish;
        }
        if (inherit && inherit->root &&
            (inherit->vm->range_restricted || (!inherit->vm->budget && inherit->vm->page_limit)))
            space->vm->range_restricted = true;
        user_space_limit(space, role_limit);
    }
    result = elf_map_image(app, space);
    if (!result && interp->file)
        result = elf_map_image(interp, space);
    if (result)
        goto finish;
    for (uint64_t va = USER_STACK_TOP - USER_STACK_SIZE; va < USER_STACK_TOP; va += PAGE_SIZE)
        if (!user_map(space, va, VM_RDWR)) {
            result = -E_NOMEM;
            goto finish;
        }

    /* SysV initial stack: argc, argv, envp and auxiliary vector. */
    words = kmalloc((OS_ARG_MAX + OS_ENV_MAX + 64) * sizeof(*words));
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
        user_load_bytes(space, sp, argv[argc++], len);
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
        user_load_bytes(space, sp, env[envc++], len);
    }
    words[n++] = 0;
    sp -= strlen(path) + 1;
    user_load_bytes(space, sp, path, strlen(path) + 1);
    uint64_t execfn = sp;
    uint8_t entropy[16];
    uint64_t random = 0;
    if (random_bytes(entropy, sizeof(entropy)) == sizeof(entropy)) {
        sp = (sp - sizeof(entropy)) & ~15ULL;
        user_load_bytes(space, sp, entropy, sizeof(entropy));
        random = sp;
    }

    words[n++] = 6; words[n++] = PAGE_SIZE; /* AT_PAGESZ */
    words[n++] = 9; words[n++] = app->entry; /* AT_ENTRY */
    words[n++] = 7; words[n++] = interp->file ? interp->bias : 0; /* AT_BASE */
    if (app->phdr) {
        words[n++] = 3; words[n++] = app->phdr; /* AT_PHDR */
        words[n++] = 4; words[n++] = sizeof(*app->ph); /* AT_PHENT */
        words[n++] = 5; words[n++] = app->header.phnum; /* AT_PHNUM */
    }
    struct task *credential_caller=sched_ready()?this_cpu()->current:NULL;
    struct exec_credentials planned;
    credentials_exec_prepare(credential_caller,&app->file->node,&planned);
    unsigned ids[4]={planned.ruid,planned.uid,planned.rgid,planned.gid};
    for(unsigned i=0;i<4;i++) {
        words[n++]=11+i;
        words[n++]=i<2?userns_uid_view(credential_caller,ids[i]):userns_gid_view(credential_caller,ids[i]);
    }
    words[n++]=23;words[n++]=planned.secure; /* AT_SECURE before ld.so runs. */
    words[n++] = 31; words[n++] = execfn; /* AT_EXECFN */
    if (random) {
        words[n++] = 25; words[n++] = random; /* AT_RANDOM, hardware entropy only */
    }
    words[n++] = 0; words[n++] = 0;
    sp = (sp - n * sizeof(uint64_t)) & ~15ULL;
    user_load_bytes(space, sp, words, n * sizeof(uint64_t));

    space->vm->brk_base = space->vm->brk_end = app->end;
    uint64_t image_end = max(app->end, interp->end);
    space->vm->mmap_next = max(0x100000000ULL, align(image_end + USER_MAX_IMAGE, 4096ULL));
    *frame = (register_t){.rip = interp->file ? interp->entry : app->entry,
                           .cs = USER_CS, .ss = USER_DS, .rflags = 0x202, .user_rsp = sp};
    if (identity) {
        app->file->references++;
        *identity = app->file;
    }
    if(exec_ids)*exec_ids=planned;
    result = 0;
finish:
#if CONFIG_ELECTRON_PROFILE
    if(result==-E_NOMEM && budget) {
        static unsigned failures;
        if(__atomic_fetch_add(&failures,1,__ATOMIC_RELAXED)<16) {
            struct task *current=sched_ready()?this_cpu()->current:NULL;
            pr_warn("ELECTRON_EXEC_NOMEM path=%s pid=%u old_pages=%llu new_pages=%llu group_pages=%llu limit_pages=%llu\n",
                path,current?current->pid:0,
                (unsigned long long)(inherit&&inherit->root?inherit->vm->pages:0),
                (unsigned long long)(space->root?space->vm->pages:0),
                (unsigned long long)user_budget_pages(budget),(unsigned long long)user_budget_limit(budget));
        }
    }
#endif
    kfree(words);
    for (unsigned i = 0; i < 2; i++)
        if (images[i].file)
            vfs_close(images[i].file);
    kfree(images);
    mutex_unlock(&vfs_mutex);
    if (result && space->root)
        user_space_destroy(space);
    if(result || !claim)userns_budget_finish(active_claim,false);
    if (own_budget)
        user_budget_put(budget);
    return result;
}

/* Test-only boot exercise, absent from normal and release kernels. It does
   not add another image identity or grant a guest policy-setting API. */
#if CONFIG_ELECTRON_PROFILE_SELFTEST
#include <mm/user_ranges.h>
#include <panic.h>
#include <printk.h>
static void virtual_v1_selftest(void);
static void profile_cache_selftest(void) {
    struct file *original=NULL,*copy=NULL;
    mutex_lock(&vfs_mutex);
    kassert(!vfs_open("/opt/electron-43.7.3/electron",O_RDONLY,0,&original));
    unsigned before=profile_hashes;
    kassert(electron_profile_match(original) && profile_hashes==before+1);
    kassert(electron_profile_match(original) && profile_hashes==before+1);
    const void *boot=ramfs_boot_identity(&original->node);
    kassert(boot && !vfs_open("/tmp/profile-cache-selftest",O_CREAT|O_RDWR,0600,&copy));
    kassert(!ramfs_import(&copy->node,boot,original->node.size));
    /* Correct bytes at an unrelated pathname do not grant the profile. */
    kassert(!electron_profile_match(copy) && profile_hashes==before+1);
    strcpy(copy->path,original->path); /* unpublished test descriptor only */
    kassert(electron_profile_match(copy) && profile_hashes==before+1);
    uint8_t last=0;
    copy->offset=copy->node.size-1;
    kassert(vfs_read(copy,&last,1)==1);
    uint8_t changed=last^1;
    copy->offset=copy->node.size-1;
    kassert(vfs_write(copy,&changed,1)==1 && !ramfs_boot_identity(&copy->node));
    kassert(!electron_profile_match(copy) && profile_hashes==before+2);
    /* Restoring mutable bytes still requires a complete new verification. */
    copy->offset=copy->node.size-1;
    kassert(vfs_write(copy,&last,1)==1 && electron_profile_match(copy));
    kassert(profile_hashes==before+3);
    kassert(electron_profile_match(copy) && profile_hashes==before+4);
    vfs_close(copy); vfs_close(original);
    kassert(!vfs_unlink("/tmp/profile-cache-selftest",false));
    kassert(!vfs_open("/tmp/profile-cache-selftest",O_CREAT|O_RDWR,0600,&copy));
    kassert(!ramfs_boot_identity(&copy->node));
    strcpy(copy->path,"/opt/electron-43.7.3/electron");
    kassert(!electron_profile_match(copy));
    vfs_close(copy); kassert(!vfs_unlink("/tmp/profile-cache-selftest",false));
    mutex_unlock(&vfs_mutex);
    pr_notice("ELECTRON_PROFILE_CACHE_SELFTEST_PASS immutable/hit/canonical-path/same-size-write/reverify/replacement\n");
}
/* Unpublished verifier VMs only. No new syscall, guest authority or state. */
static void reserve_virtual_selftest(struct address_space *s,uint64_t first,uint64_t bytes) {
    for(uint64_t done=0;done<bytes;) {
        uint64_t chunk=min(bytes-done,USER_RESERVE_MAX);
        kassert(user_ranges_new_mapping(s) && user_ranges_reserve(s,first+done,chunk,false));
        done+=chunk;
    }
}
static noinline void virtual_v1_selftest(void) {
    const uint64_t first=0x1000000000ULL,second=0x3000000000ULL,room=64ULL<<30;
    struct user_budget *g=user_budget_create(ELECTRON_GROUP_PAGES);
    struct address_space a={0},b={0},shared={0},next={0},small={0};
    kassert(USER_VIRTUAL_MAX==(32768ULL<<30) && USER_RESERVE_MAX==(1380ULL<<30));
    kassert(g && user_space_create(&a) && user_space_budget_attach(&a,g,0));
    kassert(user_space_create(&b) && user_space_budget_attach(&b,g,0));
    kassert(!user_budget_virtual_take(g,UINT64_MAX) && !user_budget_virtual_pages(g));
    kassert(user_ranges_new_mapping(&a) && !user_ranges_reserve(&a,first,USER_RESERVE_MAX+PAGE_SIZE,false));
    reserve_virtual_selftest(&a,first,USER_RESERVE_MAX);
    uint64_t filler=USER_VIRTUAL_MAX-USER_RESERVE_MAX;
    reserve_virtual_selftest(&b,second,filler);
    kassert(!user_budget_pages(g) && user_budget_virtual_pages(g)==USER_VIRTUAL_MAX/PAGE_SIZE);
    kassert(!user_access(&a,first+PAGE_SIZE,1,false));
    kassert(!user_ranges_protect(&a,first+PAGE_SIZE,3*PAGE_SIZE,VM_RDWR));
    kassert(a.vm->pages==3 && user_budget_pages(g)==3);
    uint64_t records=user_budget_records(g),generation=b.vm->generation;
    kassert(user_ranges_new_mapping(&b) && !user_ranges_reserve(&b,second+filler+PAGE_SIZE,PAGE_SIZE,false));
    kassert(!user_budget_virtual_take(g,1) && !user_budget_virtual_take(g,UINT64_MAX));
    kassert(user_budget_records(g)==records && b.vm->generation==generation);
    kassert(!user_space_clone(&next,&a) && !next.root && user_budget_pages(g)==3);
    register_t frame;char *args[]={"/bin/check",NULL};
    kassert(load_elf("/bin/check",args,NULL,&next,&frame,&a,0,NULL,NULL,NULL)==-E_NOMEM);
    kassert(!next.root && user_budget_pages(g)==3 && user_budget_records(g)==records);
    kassert(user_space_create(&small) && user_space_budget_attach(&small,g,ELECTRON_GROUP_PAGES-1));
    kassert(!user_ranges_sparse(&small) && !user_ranges_reserve(&small,first,USER_RESERVE_MAX,false));
    user_space_limit(&small,UINT64_MAX);
    kassert(small.vm->page_limit==ELECTRON_GROUP_PAGES-1 && !user_ranges_sparse(&small));
    user_space_destroy(&small);
    kassert(user_ranges_unmap(&b,second+filler-room,room));
    kassert(!load_elf("/bin/check",args,NULL,&next,&frame,&a,0,NULL,NULL,NULL));
    user_space_destroy(&next);
    kassert(user_budget_pages(g)==3 && user_budget_virtual_pages(g)==(USER_VIRTUAL_MAX-room)/PAGE_SIZE);
    user_space_share(&shared,&a);user_space_destroy(&a);
    kassert(user_budget_pages(g)==3 && shared.vm);
    user_space_destroy(&shared);
    kassert(!user_budget_pages(g) && user_budget_virtual_pages(g)==(filler-room)/PAGE_SIZE);
    kassert(user_space_create(&a) && user_space_budget_attach(&a,g,0));
    reserve_virtual_selftest(&a,first,room/2);
    kassert(!user_ranges_protect(&a,first+PAGE_SIZE,3*PAGE_SIZE,VM_RDWR));
    kassert(user_space_clone(&next,&a) && user_budget_pages(g)==6);
    kassert(user_budget_virtual_pages(g)==filler/PAGE_SIZE);
    user_space_destroy(&next);user_space_destroy(&a);user_space_destroy(&b);
    kassert(!user_budget_pages(g) && !user_budget_virtual_pages(g) && !user_budget_records(g));
    user_budget_put(g);
    pr_notice("ELECTRON_VIRTUAL_BUDGET_SELFTEST_PASS 16384GiB exact-boundary/overflow/single-reservation/inherited-limits/fork/exec-overlap/rollback/full-refunds\n");
}
void electron_budget_selftest(void) {
    profile_cache_selftest();
    struct user_budget *g = user_budget_create(8);
    struct address_space a = {0}, b = {0}, shared = {0}, next = {0};
    kassert(g && user_space_create(&a) && user_space_budget_attach(&a, g, 0));
    kassert(user_map(&a, USER_MIN, VM_RDWR));
    kassert(user_map(&a, USER_MIN + PAGE_SIZE, 0));
    kassert(user_map_file_eof(&a, USER_MIN + 2 * PAGE_SIZE, VM_READ));
    user_space_share(&shared, &a);
    kassert(user_budget_pages(g) == 3);
    kassert(user_space_clone(&b, &a));
    kassert(user_budget_pages(g) == 6 && !user_space_clone(&next, &a));
    kassert(user_space_create(&next) && user_space_budget_attach(&next, g, 2));
    kassert(user_map(&next, USER_MIN, VM_RDWR) && user_map(&next, USER_MIN + PAGE_SIZE, VM_RDWR));
    kassert(user_budget_pages(g) == 8 && !user_map(&b, USER_MIN + 3 * PAGE_SIZE, VM_RDWR));
    user_space_limit(&next, 7);
    kassert(next.vm->page_limit == 2);
    user_space_destroy(&a);
    kassert(user_budget_pages(g) == 8);
    user_space_destroy(&shared);
    kassert(user_budget_pages(g) == 5);
    user_space_destroy(&b);
    user_space_destroy(&next);
    kassert(!user_budget_pages(g) && user_budget_peak(g) == 8);
    user_budget_put(g);
    g = user_budget_create(ELECTRON_GROUP_PAGES);
    /* Zygote forks eagerly copy their backing. Keep a finite tree ceiling,
       including exact-boundary and arithmetic-overflow refusal. */
    kassert(ELECTRON_GROUP_PAGES==2097152ULL);
    kassert(user_budget_charge(g,262144) && user_budget_charge(g,65536) && user_budget_charge(g,1));
    kassert(user_budget_charge(g,ELECTRON_GROUP_PAGES-327681));
    kassert(user_budget_pages(g)==ELECTRON_GROUP_PAGES && user_budget_peak(g)==ELECTRON_GROUP_PAGES);
    kassert(!user_budget_charge(g, 1) && !user_budget_charge(g, UINT64_MAX));
    user_budget_uncharge(g, ELECTRON_GROUP_PAGES);
    kassert(!user_budget_pages(g));
    user_budget_put(g);
    /* Actual loader construction, inherited ceiling, failed exec rollback. */
    g = user_budget_create(4096);
    kassert(g && user_space_create(&a) && user_space_budget_attach(&a, g, 3000));
    kassert(user_map(&a, USER_MIN, VM_RDWR));
    register_t frame;
    char *args[] = {"/bin/check", NULL};
    kassert(!load_elf("/bin/check", args, NULL, &next, &frame, &a, 2800, NULL, NULL, NULL));
    kassert(next.vm->budget == g && next.vm->page_limit == 2800 && user_budget_pages(g) == a.vm->pages + next.vm->pages);
    user_space_destroy(&next);
    kassert(user_budget_pages(g) == 1);
    kassert(load_elf("/bin/check", args, NULL, &next, &frame, &a, 2, NULL, NULL, NULL) == -E_NOMEM);
    kassert(!next.root && user_budget_pages(g) == 1 && user_access(&a, USER_MIN, 1, true));
    user_space_destroy(&a);
    user_budget_put(g);
    extern void electron_reservation_syscall_selftest(void);
    electron_reservation_syscall_selftest();
    virtual_v1_selftest();
#if CONFIG_EMPTY_DISCARD_TEST
    extern void empty_discard_selftest(void);
    empty_discard_selftest();
#endif
    extern void user_resident_selftest(void);
    user_resident_selftest();
    pr_notice("ELECTRON_RESERVATION_SELFTEST_PASS 1380GiB/32768GiB/fork/commit/cleanup\n");
    pr_notice("ELECTRON_BUDGET_SELFTEST_PASS shared VM/fork/exec rollback/minimum limits/4096MiB bound\n");
}
#endif

/* Resolve bounded, executable #! chains before entering the ELF loader. Keep
   each rewritten argv in heap storage: recursive ELF-loader stack frames
   would exhaust a kernel stack while constructing a compiler's argument list. */
static int load(const char *path, char *const argv[], char *const env[],
                struct address_space *space, register_t *frame,
                const struct address_space *inherit, uint64_t role_limit, struct file **identity, struct userns_budget_claim *claim, struct exec_credentials *exec_ids) {
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
        result = executable_open(executable, &file);
        if (!result && ((file->node.mode & 0170000) != S_IFREG || !(file->node.mode & 0111)))
            result = -E_ACCES;
        if (!result) result=credentials_node(&file->node,1);
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
            result = load_elf(executable, (char *const *)args, env, space, frame, inherit, role_limit, identity, claim, exec_ids);
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
    generic_seccomp_release(t);
    landlock_task_release(t);
    userns_budget_finish(&t->userns_budget_claim,false);
    kfree(t->environment_data);
    t->environment_data = NULL;
    memset(t->environment, 0, sizeof(t->environment));
    t->environment_count = 0;
    display_drop(t->pid);
    task_files_unpin(t);
    if (t->context && !t->context->live) {
        mutex_lock(&vfs_mutex);
        for (unsigned i = 0; i < OS_FD_MAX; i++) {
            vfs_record_release(t->fds[i], t->tgid);
            vfs_close(t->fds[i]);
            t->fds[i] = NULL;
        }
        mutex_unlock(&vfs_mutex);
    }
    if (t->context && __atomic_fetch_sub(&t->context->references, 1, __ATOMIC_ACQ_REL) == 1) {
        if (t->context->job_terminal) {
            mutex_lock(&vfs_mutex);
            vfs_close(t->context->job_terminal);
            mutex_unlock(&vfs_mutex);
            t->context->job_terminal = NULL;
        }
        mutex_lock(&vfs_mutex);
        vfs_close(t->context->executable);
        t->context->executable = NULL;
        mutex_unlock(&vfs_mutex);
        memfd_account_release(t->context->memory_account);
        kfree(t->context);
    }
    pidns_signal_cleanup(t);
    userns_task_free(t);
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
    credentials_inherit(t->context,parent);
    t->context->uid=credentials_uid(parent);
    t->context->gid=credentials_gid(parent);
    t->context->gid_dropped=parent && parent->user && parent->context->gid_dropped;
    t->context->desktop_login_uid=parent && parent->user?parent->context->desktop_login_uid:0;
    t->context->account_session=parent && parent->user?parent->context->account_session:0;
    t->context->account_owner=parent && parent->user?parent->context->account_owner:0;
    t->context->account_console=parent && parent->user && parent->context->account_console;
    t->context->desktop_fsuid=parent && parent->user?parent->context->desktop_fsuid:0;
    t->context->account_uid=parent && parent->user?parent->context->account_uid:0;
    t->context->account_nnp=parent && parent->user && parent->context->account_nnp;
    t->context->creation_mask = parent && parent->user ? s1_mask(parent,false,0) : 0022;
    e=userns_inherit(t,parent,false,false);
    if(e){task_discard(t);return e;}
    if(pidns_active(parent))t->ppid=parent->tgid;

    struct address_space space = {0};
    register_t frame;
    /* Scheduler CR3 borrowing is not caller identity. Only the actual user
       initiator supplies the inherited accounting domain, never ppid/env. */
    const struct address_space *inherit = parent && parent->user ? &parent->space : NULL;
    uint64_t role_limit = browser ? 131072 : web_role ? (web_role == OS_WEB_RENDERER ? 196608 : 131072) : 0;
    struct exec_credentials exec_ids;
    e = load(path, argv, env, &space, &frame, inherit, role_limit, &t->context->executable, &t->userns_budget_claim, &exec_ids);
    if (e) {
        task_discard(t);
        return e;
    }

    t->space = space;
    credentials_exec_commit(t,&exec_ids);
    accounts_broker_commit(t,path,t->context->executable);
    userns_exec_commit(t);
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
       A background parent cannot take the terminal from its current owner.
       Pagers with piped stdin use a readable inherited output terminal. */
    struct task *caller=this_cpu()->current;
    unsigned input_owner=caller && caller->tgid==ppid?task_input_identity(caller):ppid;
    if (foreground && !streams) {
        display_drop(ppid);
        tty_transfer_foreground(input_owner, t->pid);
    }
    if(streams)
        for(unsigned i=0;i<3;i++)
            if(streams[i]->pty && !streams[i]->pty_master)
                pty_foreground(streams[i]->pty,foreground?input_owner:0,t->pid);

    e=task_publish(t);
    if(e){task_discard(t);return e;}
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
    struct file *identity = NULL;
    struct exec_credentials exec_ids;
    int e = load(path, argv, env, &replacement, &initial, &t->space, 0, &identity, &t->userns_budget_claim, &exec_ids);
    if (e)
        return e;

    e = process_environment_set(t, env);
    if (e) {
        mutex_lock(&vfs_mutex);vfs_close(identity);mutex_unlock(&vfs_mutex);
        user_space_destroy(&replacement);
        userns_budget_finish(&t->userns_budget_claim,false);
        return e;
    }

    credentials_exec_commit(t,&exec_ids);
    accounts_broker_commit(t,path,identity);
    t->clear_tid = 0;
    userns_budget_finish(&t->userns_budget_claim,true);
    userns_exec_commit(t);
    t->signal_depth = 0;
    __atomic_store_n(&t->context->executed, true, __ATOMIC_RELEASE);
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
    struct file *old_identity = t->context->executable;
    t->context->executable = identity;
    vfs_close(old_identity);
    for (unsigned i = 0; i < OS_FD_MAX; i++)
        if (t->fds[i] && (t->fd_flags[i] & FD_CLOEXEC)) {
            vfs_record_release(t->fds[i], t->tgid);
            vfs_close(t->fds[i]);
            t->fds[i] = NULL;
            t->fd_flags[i] = 0;
        }

    mutex_unlock(&vfs_mutex);

    return 0;
}
