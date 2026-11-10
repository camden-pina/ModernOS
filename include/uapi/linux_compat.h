#ifndef COAST_LINUX_COMPAT_H
#define COAST_LINUX_COMPAT_H
#include <stdint.h>
/* x86-64 Linux UAPI layouts: linux/stat.h, asm-generic/statfs.h. */
struct os_statx_timestamp {
    int64_t sec;
    uint32_t nsec;
    int32_t reserved;
};
struct os_statx {
    uint32_t mask, blksize;
    uint64_t attributes;
    uint32_t nlink, uid, gid;
    uint16_t mode, spare0;
    uint64_t ino, size, blocks, attributes_mask;
    struct os_statx_timestamp atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    uint64_t mnt_id;
    uint32_t dio_mem_align, dio_offset_align;
    uint64_t spare[12];
};
struct os_statfs {
    int64_t type, bsize;
    uint64_t blocks, bfree, bavail, files, ffree;
    int32_t fsid[2];
    int64_t namelen, frsize, flags, spare[4];
};
struct os_flock {
    int16_t type, whence;
    uint32_t padding;
    int64_t start, length;
    int32_t pid;
    uint32_t spare;
};
enum { OS_F_GETLK = 5, OS_F_SETLK = 6, OS_F_SETLKW = 7 };
_Static_assert(sizeof(struct os_flock) == 32, "Linux x86-64 flock ABI");
_Static_assert(sizeof(struct os_statx) == 256, "Linux statx ABI");
_Static_assert(sizeof(struct os_statfs) == 120, "Linux statfs ABI");
#endif
