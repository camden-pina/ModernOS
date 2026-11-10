/* Bounded nonblocking POSIX process-associated advisory byte-range locks.
 * All entry points require vfs_mutex. BSD flock remains independent.
 * Descriptor-close hooks release every lock of this owner on the inode;
 * generic VFS references and syscall pins must never invoke that hook. */
#define RECORD_LOCK_MAX 256
struct record_lock {
    struct mount *mount;
    uint64_t inode, first, last;
    unsigned owner, type;
};
static struct record_lock record_locks[RECORD_LOCK_MAX];
static struct record_lock record_scratch[RECORD_LOCK_MAX * 2 + 1];
static unsigned record_count;

static bool record_same_file(const struct record_lock *lock, const struct file *file) {
    return lock->mount == file->node.mount && lock->inode == file->node.id;
}

void vfs_record_release(struct file *file, unsigned owner) {
    if (!file) return;
    for (unsigned i = 0; i < record_count;) {
        if (record_locks[i].owner == owner && record_same_file(&record_locks[i], file))
            record_locks[i] = record_locks[--record_count];
        else i++;
    }
}

int vfs_record_lock(struct file *file, unsigned owner, bool query, struct os_flock *lock) {
    if (!file->node.mount || file->memory || (file->node.mode & 0170000) != S_IFREG)
        return -E_NOTSUP;
    if (lock->type < 0 || lock->type > 2 || (query && lock->type == 2))
        return -E_INVAL;
    if (!query && ((lock->type == 0 && (file->flags & 3) == O_WRONLY) ||
                   (lock->type == 1 && !(file->flags & 3))))
        return -E_BADF;
    __int128 base;
    if (lock->whence == 0) base = 0;
    else if (lock->whence == 1) base = file->offset;
    else if (lock->whence == 2) {
        if (file->node.mount->ops->stat) {
            int error = file->node.mount->ops->stat(&file->node);
            if (error) return error;
        }
        base = file->node.size;
    } else return -E_INVAL;
    __int128 first = base + lock->start, last;
    if (first < 0) return -E_INVAL;
    if (first > INT64_MAX) return -E_OVERFLOW;
    if (lock->length > 0) last = first + lock->length - 1;
    else if (lock->length < 0) { last = first - 1; first += lock->length; }
    else last = INT64_MAX;
    if (first < 0 || last < 0) return -E_INVAL;
    if (last > INT64_MAX) return -E_OVERFLOW;
    for (unsigned i = 0; i < record_count; i++) {
        const struct record_lock *other = &record_locks[i];
        if (other->owner == owner || !record_same_file(other, file) ||
            other->first > (uint64_t)last || (uint64_t)first > other->last ||
            (lock->type != 1 && other->type != 1) || lock->type == 2) continue;
        if (!query) return -E_AGAIN;
        lock->type = other->type;
        lock->whence = 0;
        lock->start = other->first;
        lock->length = other->last == INT64_MAX ? 0 : other->last - other->first + 1;
        lock->pid = other->owner;
        return 0;
    }
    if (query) { lock->type = 2; return 0; }
    unsigned count = 0;
    /* Construct a transaction before changing the live inventory. A failed
     * split or full-table request leaves every existing range intact. */
    for (unsigned i = 0; i < record_count; i++) {
        struct record_lock old = record_locks[i];
        if (old.owner != owner || !record_same_file(&old, file) ||
            old.first > (uint64_t)last || (uint64_t)first > old.last) {
            record_scratch[count++] = old;
            continue;
        }
        if (old.first < (uint64_t)first) {
            struct record_lock left = old; left.last = first - 1;
            record_scratch[count++] = left;
        }
        if (old.last > (uint64_t)last) {
            old.first = last + 1;
            record_scratch[count++] = old;
        }
    }
    if (lock->type != 2)
        record_scratch[count++] = (struct record_lock){file->node.mount, file->node.id,
                                                     first, last, owner, lock->type};
    for (unsigned i = 0; i < count; i++) {
        struct record_lock *a = &record_scratch[i];
        for (unsigned j = i + 1; j < count;) {
            struct record_lock *b = &record_scratch[j];
            if (a->mount == b->mount && a->inode == b->inode && a->owner == b->owner &&
                a->type == b->type && a->first <= b->last + 1 && b->first <= a->last + 1) {
                a->first = min(a->first, b->first); a->last = max(a->last, b->last);
                record_scratch[j] = record_scratch[--count]; j = i + 1;
            } else j++;
        }
    }
    if (count > RECORD_LOCK_MAX) return -E_NOLCK;
    memcpy(record_locks, record_scratch, count * sizeof(*record_locks));
    record_count = count;
    return 0;
}
