#include <filesystem.h>
#include <fs/vfs.h>
#include <string.h>
#include <tsc.h>

/* A finite read-only tree. No device/control files or mutable host state.
   The TSC value is the actual boot calibration, shared by all guest CPUs. */
enum { ROOT=1, DEVICES, SYSTEM, CPU, CPU0, TSC_FREQUENCY };
static const char *const names[] = {"", "", "devices", "system", "cpu", "cpu0", "tsc_freq_khz"};
static char frequency[32];
static size_t frequency_bytes;

static void make_node(struct mount *mount, unsigned id, struct vnode *out) {
    bool file = id == TSC_FREQUENCY;
    *out = (struct vnode){.mount=mount, .id=id,
        .mode=file ? S_IFREG|0444 : S_IFDIR|0555,
        .nlink=file ? 1 : id==CPU0 ? 2 : 3,
        .size=file ? frequency_bytes : 0};
    out->aux[3] = 0x5359530000000000ULL | id;
}

static int root(struct mount *mount, struct vnode *out) {
    make_node(mount,ROOT,out);
    return 0;
}

static int lookup(const struct vnode *dir, const char *name, struct vnode *out) {
    if (dir->id < ROOT || dir->id > CPU0) return -E_NOTDIR;
    unsigned id;
    if (!strcmp(name,".")) id=dir->id;
    else if (!strcmp(name,"..")) id=dir->id==ROOT ? ROOT : dir->id-1;
    else if (!strcmp(name,names[dir->id+1])) id=dir->id+1;
    else return -E_NOENT;
    make_node(dir->mount,id,out);
    return 0;
}

static long read_node(struct vnode *node, uint64_t offset, void *out, size_t size) {
    if (node->id != TSC_FREQUENCY) return -E_ISDIR;
    if (offset >= frequency_bytes) return 0;
    size = min(size,frequency_bytes-offset);
    memcpy(out,frequency+offset,size);
    return size;
}

static int readdir_node(const struct vnode *dir, uint64_t *offset, struct directory_entry *out) {
    if (dir->id < ROOT || dir->id > CPU0) return -E_NOTDIR;
    if (*offset >= 3) return 0;
    unsigned id = !*offset ? dir->id : *offset==1 ? (dir->id==ROOT ? ROOT : dir->id-1) : dir->id+1;
    const char *name = !*offset ? "." : *offset==1 ? ".." : names[id];
    *out = (struct directory_entry){.id=0x5359530000000000ULL|id,
        .mode=id==TSC_FREQUENCY ? S_IFREG|0444 : S_IFDIR|0555};
    strcpy(out->name,name);
    (*offset)++;
    return 1;
}

static const struct fs_ops ops = {.root=root, .lookup=lookup, .read=read_node, .readdir=readdir_node};

bool sysfs_readonly_mount(const struct mount *mount) {
    return mount && mount->ops==&ops && mount->readonly;
}

int sysfs_mount(struct mount *mount) {
    uint64_t khz = tsc_frequency_khz();
    if (!khz) return -E_NODEV;
    int count = snprintf(frequency,sizeof(frequency),"%llu\n",(unsigned long long)khz);
    if (count <= 0 || count >= (int)sizeof(frequency)) return -E_OVERFLOW;
    frequency_bytes=count;
    *mount=(struct mount){.ops=&ops, .readonly=true};
    strcpy(mount->path,"/sys");
    strcpy(mount->type,"sysfs");
    return 0;
}
