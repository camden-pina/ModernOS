#include <sched.h>
#include <tty.h>
#include <pty.h>
#include <string.h>
#include <mm/pmm.h>

#define CLONE_VM 0x100
#define CLONE_VFORK 0x4000
#define THREAD_REQUIRED 0x10f00ULL /* VM, FS, FILES, SIGHAND, THREAD */
#define THREAD_OPTIONAL 0x1380000ULL /* SETTLS, PARENT_SETTID, CHILD_{SET,CLEAR}TID */

static long clone_thread(struct task *parent, register_t *frame, uint64_t flags, uint64_t stack) {
    if ((flags & THREAD_REQUIRED) != THREAD_REQUIRED ||
        (flags & ~(THREAD_REQUIRED | THREAD_OPTIONAL | 0x440000ULL)))
        return -E_NOTSUP;
    if (parent->vfork_parent || parent->context->exiting)
        return -E_NOTSUP;
    if (!stack || (stack & 7) || stack < USER_MIN + 16 ||
        !user_access(&parent->space, stack - 16, 16, true))
        return -E_FAULT;
    if ((flags & 0x80000) && frame->r8 >= USER_TOP)
        return -E_PERM;
    if ((flags & 0x100000) && !user_access(&parent->space, frame->rdx, 4, true))
        return -E_FAULT;
    if ((flags & (0x1000000 | 0x200000)) &&
        ((frame->r10 & 3) || !user_access(&parent->space, frame->r10, 4, true)))
        return -E_FAULT;
    struct task *child = task_allocate(parent->name, true, parent->ppid, parent->cpu);
    if (!child)
        return -E_AGAIN;
    kfree(child->context);
    __atomic_fetch_add(&parent->context->references, 1, __ATOMIC_ACQ_REL);
    task_context_bind(child, parent->context);
    child->browser_sandbox = parent->browser_sandbox;
    child->browser_local_file = parent->browser_local_file;
    memcpy(child->browser_home, parent->browser_home, sizeof(child->browser_home));
    memcpy(child->browser_local, parent->browser_local, sizeof(child->browser_local));
    user_space_share(&child->space, &parent->space);
    *child->frame = *frame;
    child->frame->rax = 0;
    child->frame->user_rsp = stack;
    child->fsbase = (flags & 0x80000) ? frame->r8 : parent->fsbase;
    __asm__ volatile("fxsave64 %0" : "=m"(child->fpu));
    child->signal_mask = parent->signal_mask;
    uint32_t tid = child->pid;
    if ((flags & 0x100000) && !user_copy_out(&parent->space, frame->rdx, &tid, 4)) {
        task_discard(child);
        return -E_FAULT;
    }
    if ((flags & 0x1000000) && !user_copy_out(&child->space, frame->r10, &tid, 4)) {
        task_discard(child);
        return -E_FAULT;
    }
    if (flags & 0x200000)
        child->clear_tid = frame->r10;
    task_publish(child);
    return tid;
}

long process_clone(struct task *parent, register_t *frame, uint64_t flags, uint64_t stack) {
    if (flags & 0x10000)
        return clone_thread(parent, frame, flags, stack);
    if (parent->pid != parent->tgid || parent->context->references != 1)
        return -E_NOTSUP;

    bool shared = flags == (CLONE_VM | CLONE_VFORK | 17);
    /* Processes, not threads. Shared VM is supported only with the parent
       suspended until exec/exit, as used by musl posix_spawn and vfork. */
    if (flags != 17 && !shared)
        return -E_NOTSUP;
    if (parent->vfork_parent)
        return -E_NOTSUP;
    if (stack && ((stack & 7) || stack < USER_MIN + 16 ||
                  !user_access(&parent->space, stack - 16, 16, true)))
        return -E_FAULT;

    /* A private child can be admitted on a less busy CPU. Shared-vfork
       children stay with the suspended parent; no live address-space
       migration or cross-CPU VM shootdown is introduced. */
    struct task *child =
        task_allocate(parent->name, true, parent->pid, shared ? (int)parent->cpu : -1);
    if (!child)
        return -E_NOMEM;

    if (shared)
        user_space_share(&child->space, &parent->space);
    else if (!user_space_clone(&child->space, &parent->space)) {
        task_discard(child);
        return -E_NOMEM;
    }

    *child->frame = *frame;
    child->frame->rax = 0;
    if (stack)
        child->frame->user_rsp = stack;
    child->fsbase = parent->fsbase;
    __asm__ volatile("fxsave64 %0" : "=m"(child->fpu));
    child->signal_mask = parent->signal_mask;
    memcpy(child->signal_actions, parent->signal_actions, sizeof(child->context->signal_actions));
    memcpy(child->signal_frames, parent->signal_frames, sizeof(child->signal_frames));
    child->signal_depth = parent->signal_depth;
    child->context->creation_mask = parent->context->creation_mask;

    if (process_environment_set(child, parent->environment)) {
        task_discard(child);
        return -E_NOMEM;
    }
    memcpy(child->cwd, parent->cwd, OS_PATH_MAX);

    if (shared)
        child->vfork_parent = parent->pid;

    mutex_lock(&vfs_mutex);
    for (unsigned i = 0; i < OS_FD_MAX; i++)
        if (parent->fds[i]) {
            child->fds[i] = parent->fds[i];
            child->fds[i]->references++;
            child->fd_flags[i] = parent->fd_flags[i];
        }
    mutex_unlock(&vfs_mutex);

    if (child->fds[0] && child->fds[0]->tty)
        tty_transfer_foreground(parent->pid, child->pid);
    if (child->fds[0] && child->fds[0]->pty && !child->fds[0]->pty_master)
        pty_foreground(child->fds[0]->pty, parent->pid, child->pid);

    unsigned pid = child->pid;
    task_publish(child);

    if (shared)
        task_vfork_wait(pid);
    return pid;
}
