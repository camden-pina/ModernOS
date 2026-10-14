#include <fs/file_time.h>
#include <fs/vfs.h>
#include <mm/pmm.h>
#include <mm/user.h>
#include <string.h>

#define RAM_MAX_FILE (256 * 1024 * 1024)

struct ram_node {
    uint64_t ino;
    char name[OS_NAME_MAX + 1];
    unsigned mode;
    int64_t atime, mtime, ctime;
    size_t size, capacity;
    uint64_t *pages;
    struct ram_node *parent, *children, *next, *previous, *hash_next;
    struct ram_node **buckets, *iterate;
    unsigned bucket_count, child_count;
    uint64_t iterate_index;
    /* Limine keeps module memory reserved. Imported file bytes can reference
       that immutable archive until a write needs private RAMFS pages. */
    const uint8_t *boot_data;
};

static const struct fs_ops operations;
static uint64_t next_inode = 1;

static void node_info(struct mount *m, struct ram_node *n, struct vnode *v) {
    *v = (struct vnode){.mount = m,
                        .id = (uintptr_t)n,
                        .size = n->size,
                        .mode = n->mode,
                        .aux = {[3] = n->ino},
                        .atime = n->atime,
                        .mtime = n->mtime,
                        .ctime = n->ctime};
}

static int ram_root(struct mount *m, struct vnode *out) {
    node_info(m, m->data, out);
    return 0;
}

/* The shared VFS mutex owns lookup/mutation/enumeration. Hashing is only a
   cache: allocation failure keeps the existing linked-list implementation. */
static unsigned name_hash(const char *name) {
    unsigned hash = 2166136261u;
    for (; *name; name++) {
        hash = (hash ^ (unsigned char)*name) * 16777619u;
    }
    return hash;
}

static struct ram_node *child(struct ram_node *dir, const char *name) {
    struct ram_node *n =
        dir->buckets ? dir->buckets[name_hash(name) & (dir->bucket_count - 1)] : dir->children;
    for (; n; n = dir->buckets ? n->hash_next : n->next) {
        if (!strcmp(n->name, name)) {
            return n;
        }
    }
    return NULL;
}

static bool index_grow(struct ram_node *dir) {
    unsigned count = dir->bucket_count ? dir->bucket_count * 2 : 64;
    struct ram_node **buckets = kmallocz(count * sizeof(*buckets));
    if (!buckets) {
        return false;
    }
    for (struct ram_node *n = dir->children; n; n = n->next) {
        unsigned bucket = name_hash(n->name) & (count - 1);
        n->hash_next = buckets[bucket];
        buckets[bucket] = n;
    }
    kfree(dir->buckets);
    dir->buckets = buckets;
    dir->bucket_count = count;
    return true;
}

static void attach(struct ram_node *dir, struct ram_node *n) {
    n->parent = dir;
    n->previous = NULL;
    n->next = dir->children;
    if (n->next) {
        n->next->previous = n;
    }
    dir->children = n;
    dir->child_count++;
    dir->iterate = NULL;
    dir->iterate_index = 0;
    if (dir->child_count >= 32 &&
        (!dir->buckets || (dir->child_count > dir->bucket_count * 2 && dir->bucket_count < 8192))) {
        if (index_grow(dir)) {
            return; /* Rehash includes the newly attached node. */
        }
    }
    if (dir->buckets) {
        unsigned bucket = name_hash(n->name) & (dir->bucket_count - 1);
        n->hash_next = dir->buckets[bucket];
        dir->buckets[bucket] = n;
    }
}

static int ram_lookup(const struct vnode *dir, const char *name, struct vnode *out) {
    if ((dir->mode & 0170000) != S_IFDIR) {
        return -E_NOTDIR;
    }
    struct ram_node *d = (void *)(uintptr_t)dir->id, *n = child(d, name);
    if (!n) {
        return -E_NOENT;
    }
    node_info(dir->mount, n, out);
    return 0;
}

static long ram_read(struct vnode *v, uint64_t off, void *data, size_t size) {
    struct ram_node *n = (void *)(uintptr_t)v->id;
    if (off >= n->size) {
        return 0;
    }
    size = min((uint64_t)size, n->size - off);
    if (n->boot_data) {
        memcpy(data, n->boot_data + off, size);
        return size;
    }
    size_t done = 0;
    while (done < size) {
        size_t part = min(size - done, PAGE_SIZE - off % PAGE_SIZE);
        memcpy((uint8_t *)data + done,
               (uint8_t *)phys_ptr(n->pages[off / PAGE_SIZE]) + off % PAGE_SIZE, part);
        off += part;
        done += part;
    }
    return done;
}

static int ram_materialize(struct ram_node *n) {
    if (!n->boot_data)
        return 0;

    size_t count = (n->size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t *pages = count ? kmalloc(count * sizeof(*pages)) : NULL;
    if (count && !pages)
        return -E_NOMEM;

    for (size_t i = 0; i < count; i++) {
        pages[i] = pmm_alloc_pages(1);
        if (!pages[i]) {
            while (i)
                pmm_free_pages(pages[--i], 1);
            kfree(pages);
            return -E_NOMEM;
        }
        size_t bytes = min(PAGE_SIZE, n->size - i * PAGE_SIZE);
        memcpy(phys_ptr(pages[i]), n->boot_data + i * PAGE_SIZE, bytes);
        if (bytes < PAGE_SIZE)
            memset((uint8_t *)phys_ptr(pages[i]) + bytes, 0, PAGE_SIZE - bytes);
    }

    n->pages = pages;
    n->capacity = count;
    n->boot_data = NULL;
    return 0;
}

int ramfs_import(struct vnode *v, const void *data, size_t size) {
    if (v->mount->ops != &operations || (v->mode & 0170000) != S_IFREG || size > RAM_MAX_FILE)
        return -E_INVAL;
    struct ram_node *n = (void *)(uintptr_t)v->id;
    if (n->size || n->pages || n->boot_data)
        return -E_EXIST;
    n->boot_data = data;
    n->size = v->size = size;
    return 0;
}

static long ram_write(struct vnode *v, uint64_t off, const void *data, size_t size) {
    struct ram_node *n = (void *)(uintptr_t)v->id;
    if (off > RAM_MAX_FILE || size > RAM_MAX_FILE - off) {
        return -E_FBIG;
    }

    int error = ram_materialize(n);
    if (error)
        return error;

    size_t needed = (off + size + PAGE_SIZE - 1) / PAGE_SIZE,
           old_pages = (n->size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (needed > n->capacity) {
        size_t capacity = max(needed, n->capacity ? 2 * n->capacity : 8);
        uint64_t *pages = krealloc(n->pages, capacity * sizeof(*pages));
        if (!pages) {
            return -E_NOMEM;
        }
        n->pages = pages;
        n->capacity = capacity;
    }

    for (size_t i = old_pages; i < needed; i++) {
        n->pages[i] = pmm_alloc_pages(1);
        if (!n->pages[i]) {
            while (i > old_pages) {
                pmm_free_pages(n->pages[--i], 1);
            }
            return -E_NOMEM;
        }
        memset(phys_ptr(n->pages[i]), 0, PAGE_SIZE);
    }

    if (off > n->size && (n->size % PAGE_SIZE)) {
        size_t gap = min(off - n->size, PAGE_SIZE - n->size % PAGE_SIZE);
        memset((uint8_t *)phys_ptr(n->pages[n->size / PAGE_SIZE]) + n->size % PAGE_SIZE, 0, gap);
    }

    size_t done = 0;
    while (done < size) {
        size_t part = min(size - done, PAGE_SIZE - off % PAGE_SIZE);
        memcpy((uint8_t *)phys_ptr(n->pages[off / PAGE_SIZE]) + off % PAGE_SIZE,
               (const uint8_t *)data + done, part);
        off += part;
        done += part;
    }

    n->size = max(n->size, off);
    v->size = n->size;
    n->mtime = n->ctime = fs_time_now();
    v->mtime = n->mtime;
    v->ctime = n->ctime;
    return done;
}

static int ram_truncate(struct vnode *v) {
    struct ram_node *n = (void *)(uintptr_t)v->id;
    for (size_t i = 0; n->pages && i < (n->size + PAGE_SIZE - 1) / PAGE_SIZE; i++) {
        pmm_free_pages(n->pages[i], 1);
    }
    kfree(n->pages);
    n->pages = NULL;
    n->boot_data = NULL;
    n->capacity = n->size = 0;
    v->size = 0;
    n->mtime = n->ctime = fs_time_now();
    return 0;
}

static int ram_create(const struct vnode *dir, const char *name, unsigned mode, struct vnode *out) {
    struct ram_node *d = (void *)(uintptr_t)dir->id;
    if ((d->mode & 0170000) != S_IFDIR) {
        return -E_NOTDIR;
    }
    if (!name[0] || strlen(name) > OS_NAME_MAX) {
        return -E_INVAL;
    }
    if (child(d, name)) {
        return -E_EXIST;
    }
    struct ram_node *n = kmallocz(sizeof(*n));
    if (!n) {
        return -E_NOMEM;
    }
    n->ino = next_inode++;
    strcpy(n->name, name);
    n->mode = mode;
    n->mtime = n->ctime = fs_time_now();
    d->mtime = d->ctime = n->mtime;
    attach(d, n);
    node_info(dir->mount, n, out);
    return 0;
}

static void detach(struct ram_node *n) {
    struct ram_node *dir = n->parent;
    if (n->previous) {
        n->previous->next = n->next;
    } else {
        dir->children = n->next;
    }
    if (n->next) {
        n->next->previous = n->previous;
    }
    if (dir->buckets) {
        struct ram_node **slot = &dir->buckets[name_hash(n->name) & (dir->bucket_count - 1)];
        while (*slot && *slot != n) {
            slot = &(*slot)->hash_next;
        }
        if (*slot) {
            *slot = n->hash_next;
        }
    }
    dir->child_count--;
    dir->iterate = NULL;
    dir->iterate_index = 0;
    n->next = n->previous = n->hash_next = NULL;
}

static int ram_unlink(const struct vnode *dir, const char *name, bool directory) {
    struct ram_node *d = (void *)(uintptr_t)dir->id, *n = child(d, name);
    if (!n) {
        return -E_NOENT;
    }
    if (((n->mode & 0170000) == S_IFDIR) != directory) {
        return directory ? -E_NOTDIR : -E_ISDIR;
    }
    if (n->children) {
        return -E_NOTEMPTY;
    }
    struct vnode v;
    node_info(dir->mount, n, &v);
    ram_truncate(&v);
    detach(n);
    kfree(n->buckets);
    kfree(n);
    return 0;
}

static int ram_rename(const struct vnode *old, const char *name, const struct vnode *dest,
                      const char *newname) {
    struct ram_node *d = (void *)(uintptr_t)old->id, *n = child(d, name),
                    *target = (void *)(uintptr_t)dest->id;
    if (!n) {
        return -E_NOENT;
    }
    for (struct ram_node *p = target; p; p = p->parent) {
        if (p == n) {
            return -E_INVAL;
        }
    }

    struct ram_node *replaced = child(target, newname);
    if (replaced == n) {
        return 0;
    }
    if (replaced) {
        bool directory = (n->mode & 0170000) == S_IFDIR;
        if (((replaced->mode & 0170000) == S_IFDIR) != directory) {
            return directory ? -E_NOTDIR : -E_ISDIR;
        }
        if (replaced->children) {
            return -E_NOTEMPTY;
        }
        /* The VFS rejects open/busy targets. All validation precedes this
           allocation-free replacement, under the shared filesystem mutex. */
        int e = ram_unlink(dest, newname, directory);
        if (e) {
            return e;
        }
    }

    detach(n);
    strcpy(n->name, newname);
    attach(target, n);
    n->ctime = fs_time_now();
    d->mtime = d->ctime = target->mtime = target->ctime = n->ctime;
    return 0;
}

static int ram_readdir(const struct vnode *v, uint64_t *offset, struct directory_entry *out) {
    struct ram_node *dir = (void *)(uintptr_t)v->id;
    if ((dir->mode & 0170000) != S_IFDIR) {
        return -E_NOTDIR;
    }

    struct ram_node *n = dir->children;
    if (*offset && *offset == dir->iterate_index) {
        n = dir->iterate;
    } else {
        uint64_t index = 0;
        while (n && index < *offset) {
            n = n->next;
            index++;
        }
    }

    if (!n) {
        return 0;
    }

    out->id = n->ino;
    out->mode = n->mode;
    strcpy(out->name, n->name);
    (*offset)++;
    dir->iterate = n->next;
    dir->iterate_index = *offset;
    return 1;
}

static int ram_stat(struct vnode *v) {
    node_info(v->mount, (void *)(uintptr_t)v->id, v);
    return 0;
}

static int ram_chmod(struct vnode *v, unsigned mode) {
    struct ram_node *n = (void *)(uintptr_t)v->id;
    n->mode = (n->mode & 0170000) | (mode & 0777);
    v->mode = n->mode;
    return 0;
}

static int ram_resize(struct vnode *v, uint64_t length) {
    struct ram_node *n = (void *)(uintptr_t)v->id;
    if (length > RAM_MAX_FILE) {
        return -E_FBIG;
    }
    if (length == n->size) {
        return 0;
    }
    if (!length) {
        return ram_truncate(v);
    }
    if (length > n->size) {
        const uint8_t zero = 0;
        long e = ram_write(v, length - 1, &zero, 1);
        return e == 1 ? 0 : e < 0 ? (int)e : -E_IO;
    }
    int error = ram_materialize(n);
    if (error)
        return error;
    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t i = pages; i < (n->size + PAGE_SIZE - 1) / PAGE_SIZE; i++) {
        pmm_free_pages(n->pages[i], 1);
    }
    if (length % PAGE_SIZE) {
        memset((uint8_t *)phys_ptr(n->pages[pages - 1]) + length % PAGE_SIZE, 0,
               PAGE_SIZE - length % PAGE_SIZE);
    }
    n->size = v->size = length;
    n->mtime = n->ctime = fs_time_now();
    v->mtime = n->mtime;
    v->ctime = n->ctime;
    return 0;
}

static int ram_set_times(struct vnode *v, int64_t atime, int64_t mtime, unsigned changed) {
    struct ram_node *n = (void *)(uintptr_t)v->id;
    if (changed & 1) {
        n->atime = atime;
    }
    if (changed & 2) {
        n->mtime = mtime;
    }
    n->ctime = fs_time_now();
    node_info(v->mount, n, v);
    return 0;
}

static int ram_symlink(const struct vnode *dir, const char *name, const char *target) {
    struct vnode v;
    int e = ram_create(dir, name, S_IFLNK | 0777, &v);
    if (e) {
        return e;
    }
    size_t size = strlen(target);
    long n = ram_write(&v, 0, target, size);
    if (n == (long)size) {
        return 0;
    }
    ram_unlink(dir, name, false);
    return n < 0 ? (int)n : -E_IO;
}

static const struct fs_ops operations = {.root = ram_root,
                                         .lookup = ram_lookup,
                                         .read = ram_read,
                                         .write = ram_write,
                                         .truncate = ram_truncate,
                                         .create = ram_create,
                                         .unlink = ram_unlink,
                                         .rename = ram_rename,
                                         .readdir = ram_readdir,
                                         .stat = ram_stat,
                                         .chmod = ram_chmod,
                                         .resize = ram_resize,
                                         .set_times = ram_set_times,
                                         .symlink = ram_symlink};

int ramfs_mount(struct mount *m) {
    struct ram_node *root = kmallocz(sizeof(*root));
    if (!root) {
        return -E_NOMEM;
    }
    root->ino = next_inode++;
    root->mode = S_IFDIR | 0755;
    m->data = root;
    m->ops = &operations;
    strcpy(m->type, "ramfs");
    return 0;
}
