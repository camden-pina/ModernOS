#include <sched.h>
#include <sandbox.h>
#include <string.h>

/* vfs_mutex protects file identity/path updates and the retained reference.
   Only the calling process's shared context is used by the syscall route. */
long process_executable_readlink(const struct task *t, const char *path, void *out, size_t size) {
    if (!size) return -E_INVAL;
    if (strcmp(path, "/proc/self/exe")) return -E_NOENT;
    int permission = sandbox_path(path, SANDBOX_META);
    if (permission) return permission;
    const struct file *file = t->context->executable;
    if (!file || !file->references || file->path[0] != '/') return -E_NOENT;
    permission = sandbox_file(file, SANDBOX_META);
    if (permission) return permission;
    const char suffix[] = " (deleted)";
    size_t path_size = strlen(file->path);
    size_t length = path_size + (file->unlinked ? sizeof(suffix)-1 : 0);
    size_t count = min(length,size), first = min(count,path_size);
    memcpy(out,file->path,first);
    if (count>first) memcpy((char *)out+first,suffix,count-first);
    return count; /* readlink deliberately does not append a NUL. */
}
