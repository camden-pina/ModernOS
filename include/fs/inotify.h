#ifndef COAST_INOTIFY_H
#define COAST_INOTIFY_H
#include <fs/vfs.h>
struct task;
struct inode_watch_queue;
struct inode_watch_queue *inotify_allocate(void); /* VFS mutex held */
void inotify_release(struct inode_watch_queue *);
long inotify_control(struct task *, void *);
long inotify_read(struct task *,struct file *,uint64_t,size_t);
unsigned inotify_poll(struct file *,unsigned);
bool inotify_mount_busy(const struct mount *);
void inotify_emit(const struct vnode *,uint32_t,uint32_t,const char *);
void inotify_file(const struct file *,uint32_t);
uint32_t inotify_cookie(void);
enum {
    IN_ACCESS=1,IN_MODIFY=2,IN_ATTRIB=4,IN_CLOSE_WRITE=8,IN_CLOSE_NOWRITE=16,
    IN_OPEN=32,IN_MOVED_FROM=64,IN_MOVED_TO=128,IN_CREATE=256,IN_DELETE=512,
    IN_DELETE_SELF=1024,IN_MOVE_SELF=2048,IN_Q_OVERFLOW=16384,IN_IGNORED=32768,
    IN_ISDIR=0x40000000u
};
#endif
