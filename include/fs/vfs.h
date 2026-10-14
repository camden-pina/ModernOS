#ifndef VFS_H
#define VFS_H
#include <uapi/abi.h>
#include <sync.h>
#include <stddef.h>
#include <boot/info.h>

struct block_device;
struct mount;
struct vnode {
    struct mount *mount;
    uint64_t id, size, aux[4], cache[2];
    uint32_t mode;
    int64_t atime, mtime, ctime;
};

struct directory_entry {
    uint64_t id;
    uint32_t mode;
    char name[OS_NAME_MAX + 1];
};

struct fs_ops {
    int (*root)(struct mount *, struct vnode *);
    int (*lookup)(const struct vnode *, const char *, struct vnode *);
    long (*read)(struct vnode *, uint64_t, void *, size_t);
    long (*write)(struct vnode *, uint64_t, const void *, size_t);
    int (*truncate)(struct vnode *);
    int (*create)(const struct vnode *, const char *, unsigned, struct vnode *);
    int (*unlink)(const struct vnode *, const char *, bool);
    int (*rename)(const struct vnode *, const char *, const struct vnode *, const char *);
    int (*readdir)(const struct vnode *, uint64_t *, struct directory_entry *);
    int (*stat)(struct vnode *);
    int (*chmod)(struct vnode *, unsigned);
    int (*sync)(struct mount *);
    int (*resize)(struct vnode *, uint64_t);
    int (*set_times)(struct vnode *, int64_t, int64_t, unsigned);
    int (*symlink)(const struct vnode *, const char *, const char *);
};

struct mount {
    char path[OS_PATH_MAX], type[8];
    const struct fs_ops *ops;
    struct block_device *device;
    void *data;
    bool readonly;
};

struct file {
    struct vnode node;
    char path[OS_PATH_MAX];
    uint64_t offset;
    unsigned flags, references;
    unsigned watcher_references;
    unsigned capability_domain, capability_renderer;
    bool tty;
    unsigned device; /* 1: null; 2: zero; 3/4: random/urandom. */
    struct pipe_buffer *pipe;
    struct net_socket *socket;
    struct unix_socket *unix_socket;
    struct memory_object *memory;
    struct event_poll *epoll;
    struct event_counter *event;
    struct pty *pty;
    bool pty_master;
};

extern mutex_t vfs_mutex;
void vfs_init(const struct boot_module *modules, uint64_t module_count);
void vfs_discover(void);
int vfs_normalize(const char *cwd, const char *path, char out[OS_PATH_MAX]);
int vfs_lookup(const char *path, struct vnode *out);
int vfs_lstat(const char *path, struct vnode *out);
int vfs_realpath(const char *path, char out[OS_PATH_MAX]);
int vfs_lookup_write(const char *path, struct vnode *out);
long vfs_readlink(const char *path, void *out, size_t size);
unsigned vfs_poll(struct file *, unsigned events);
int vfs_open(const char *path, unsigned flags, unsigned mode, struct file **out);
void vfs_close(struct file *f);
int vfs_pipe(struct file *out[2], unsigned flags);
int vfs_socket(struct net_socket *, unsigned flags, struct file **out);
int vfs_unix_socket(struct unix_socket *, unsigned flags, struct file **out);
int vfs_memfd(unsigned flags, unsigned domain, struct file **out);
int vfs_epoll(struct file **out);
int vfs_eventfd(uint32_t initial, unsigned flags, struct file **out);
int vfs_pty(struct file *out[2], unsigned flags);
long pipe_read(struct file *f, void *data, size_t size);
long pipe_write(struct file *f, const void *data, size_t size);
long pipe_capacity(struct file *f, bool resize, uint64_t requested);
long vfs_read(struct file *f, void *data, size_t size);
long vfs_write(struct file *f, const void *data, size_t size);
int vfs_mkdir(const char *path, unsigned mode);
int vfs_unlink(const char *path, bool directory);
int vfs_rename(const char *oldpath, const char *newpath);
int vfs_rename_flags(const char *oldpath, const char *newpath, bool no_replace);
int vfs_chmod(const char *path, unsigned mode);
int vfs_symlink(const char *target, const char *path);
int vfs_readdir(struct file *f, struct directory_entry *entry);
int vfs_sync(void);
size_t vfs_mounts_text(char *out, size_t cap);
size_t vfs_lsblk_text(char *out, size_t cap);
int vfs_mount_device(const char *source, const char *target, const char *type, unsigned flags);
int vfs_unmount(const char *target, unsigned flags);
int ramfs_mount(struct mount *mount);
/* Boot-only import from a reserved, immutable Limine module. */
int ramfs_import(struct vnode *node, const void *data, size_t size);
int fat32_mount(struct mount *mount);
int ext_mount(struct mount *mount);
#endif
