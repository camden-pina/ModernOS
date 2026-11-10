#include <pidns.h>
#include <filesystem.h>
#include <credentials.h>
#include <sched.h>
#include <tty.h>
#include <pty.h>
#include <string.h>
#include <mm/pmm.h>
#include <printk.h>
#include <seccomp.h>
#include <panic.h>

#define CLONE_VM 0x100
#define CLONE_VFORK 0x4000
#define CLONE_CHILD_SETTID 0x1000000ULL
#define CLONE_CHILD_CLEARTID 0x200000ULL
#define PROCESS_TID_FLAGS (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)
#define THREAD_REQUIRED 0x10f00ULL /* VM, FS, FILES, SIGHAND, THREAD */
#define THREAD_OPTIONAL 0x1380000ULL /* SETTLS, PARENT_SETTID, CHILD_{SET,CLEAR}TID */

static bool private_fork_scope(const struct task *t,uint64_t flags,uint64_t stack) {
    return CONFIG_PRIVATE_FORK_TEST && flags==0x01200011ULL && !stack && t &&
        t->user && !t->browser_sandbox && !t->context->web_role &&
        credentials_uid(t)==1000 && credentials_gid(t)==1000 && userns_admitted_account(t);
}
#if CONFIG_PRIVATE_FORK_VERIFY
/* Separate verifier only. Seven saturating rollback checkpoints, no user
 * control and no synthetic account ever passed to a userspace process. */
static unsigned private_fork_fault_progress;
static unsigned private_fork_fault_stage(void) {
    unsigned n=__atomic_load_n(&private_fork_fault_progress,__ATOMIC_RELAXED);return n<7?n+1:0;
}
#endif
bool private_fork_fault(unsigned stage,unsigned point) {
#if CONFIG_PRIVATE_FORK_VERIFY
    if(stage==point && stage && stage<=7) {
        unsigned expected=stage-1;
        if(__atomic_compare_exchange_n(&private_fork_fault_progress,&expected,stage,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {
            pr_info("G2_FAULT_INJECT point=%u pid=%u rollback_required=1 verifier_only=1\n",point,this_cpu()->current->pid);return true;
        }
    }
#else
    (void)stage;(void)point;
#endif
    return false;
}

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
    int e=userns_inherit(child,parent,true,false);
    if(e){task_discard(child);return e;}
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
    uint32_t tid=pidns_view(child,child->pid);
    if(pidns_active(parent)) {
        if(flags&0x200000)child->clear_tid=frame->r10;
        e=task_publish_thread_p1(child,parent,flags&0x100000?frame->rdx:0,flags&0x1000000?frame->r10:0);
        if(e){task_discard(child);return e;}return tid;
    }
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
    e=task_publish(child);
    if(e){task_discard(child);return e;}
    return tid;
}

long process_clone(struct task *parent, register_t *frame, uint64_t flags, uint64_t stack) {
    if (flags & 0x10000)
        return clone_thread(parent, frame, flags, stack);
    int fs_helper=s1_clone_check(parent,flags);if(fs_helper<0)return fs_helper;
    int new_pid=pidns_clone_check(parent,flags);if(new_pid<0)return new_pid;
    bool new_ns=userns_newuser_clone(parent,flags) || (new_pid && (flags&C2_CLONE_NEWUSER));
    bool private_fork=private_fork_scope(parent,flags,stack);
    bool shared_spawn=flags==(CLONE_VM | CLONE_VFORK | 17);
    bool native_spawn=CONFIG_LARGE_BUILDS && shared_spawn && parent->user && parent->space.vm &&
        !parent->space.vm->budget && !parent->browser_sandbox && !parent->context->web_role &&
        (parent->pid!=parent->tgid || parent->context->references!=1);
    if ((parent->pid != parent->tgid && !private_fork && !native_spawn) || (parent->context->references != 1 && !new_ns && !private_fork && !new_pid && !fs_helper && !native_spawn))
        return -E_NOTSUP;
    bool atomic_private=private_fork || new_pid;
    if((atomic_private || native_spawn) && (parent->vfork_parent || parent->context->exiting || parent->context->fork_owner ||
       !parent->syscall_locked || parent->context->syscall_lock.owner!=parent))return -E_AGAIN;

    bool shared = shared_spawn || fs_helper;
    /* Processes, not threads. Shared VM is supported only with the parent
       suspended until exec/exit, as used by musl posix_spawn and vfork. */
    if ((flags & ~PROCESS_TID_FLAGS) != 17 && !shared && !new_ns && !new_pid && !fs_helper)
        return -E_NOTSUP;
    if (parent->vfork_parent)
        return -E_NOTSUP;
    if (stack && ((stack & 7) || stack < USER_MIN + 16 ||
                  !user_access(&parent->space, stack - 16, 16, true)))
        return -E_FAULT;
    /* These two flags describe the child's own TID word; they do not grant
       a shared VM, new parent, or other process-creation authority. Preserve
       the existing aligned/writable TID-pointer policy used for threads. */
    if ((flags & PROCESS_TID_FLAGS) &&
        ((frame->r10 & 3) || !user_access(&parent->space, frame->r10, 4, true)))
        return -E_FAULT;

    if(fs_helper){int e=s1_pointers(parent,stack,frame->r8);if(e)return e;}
    uint32_t parent_tid_before=0;
    bool tid_snapshot=false;
#if CONFIG_ZYGOTE_TRACE
    if(private_fork)tid_snapshot=user_copy_in(&parent->space,&parent_tid_before,frame->r10,4);
#endif

    /* A private child can be admitted on a less busy CPU. Shared-vfork
       children stay with the suspended parent; no live address-space
       migration or cross-CPU VM shootdown is introduced. */
    unsigned fail_stage=0;
#if CONFIG_PRIVATE_FORK_VERIFY
    if(private_fork)fail_stage=private_fork_fault_stage();
#endif
    if(private_fork_fault(fail_stage,1) || (new_pid && pidns_verify_fault(parent,1)) || (fs_helper && s1_fault(parent,1)))return -E_NOMEM;
    struct task *child =
        task_allocate(parent->name, true, (private_fork || native_spawn) ? parent->tgid : parent->pid, shared ? (int)parent->cpu : -1);
    if (!child)
        return -E_NOMEM;

    int error=-E_NOMEM;bool frozen=false,vfs_held=false;
    if(private_fork_fault(fail_stage,2))goto fail;
    if(fs_helper)child->vfork_parent=parent->pid;
    error=userns_inherit_fs(child,parent,false,new_ns,fs_helper);
    if(error)goto fail;
    if((new_pid && pidns_verify_fault(parent,2)) || (fs_helper && s1_fault(parent,2))){error=-E_NOMEM;goto fail;}
    if(new_ns || atomic_private || native_spawn) {
        mutex_lock(&vfs_mutex);vfs_held=true;
        if(!(native_spawn?task_native_spawn_freeze(parent):userns_fork_freeze(parent))){error=-E_AGAIN;goto fail;}
        frozen=true;
    }
    if(new_pid){error=pidns_clone_prepare(child,parent,flags);if(error)goto fail;if(pidns_verify_fault(parent,3)){error=-E_NOMEM;goto fail;}}
    error=-E_NOMEM;if(private_fork_fault(fail_stage,3))goto fail;

    if (shared)
        user_space_share(&child->space, &parent->space);
    else if (!user_space_clone(&child->space, &parent->space)) {
        goto fail;
    }
    if(private_fork_fault(fail_stage,4) || (new_pid && pidns_verify_fault(parent,4)) || (fs_helper && s1_fault(parent,3)))goto fail;

    *child->frame = *frame;
    child->frame->rax = 0;
    if (stack)
        child->frame->user_rsp = stack;
    child->fsbase = fs_helper?frame->r8:parent->fsbase;
    __asm__ volatile("fxsave64 %0" : "=m"(child->fpu));
    child->signal_mask = parent->signal_mask;
    memcpy(child->signal_actions, parent->signal_actions, sizeof(child->context->signal_actions));
    memcpy(child->signal_frames, parent->signal_frames, sizeof(child->signal_frames));
    child->signal_depth = parent->signal_depth;
    credentials_inherit(child->context,parent);
    child->context->uid=parent->context->uid;
    child->context->gid=parent->context->gid;
    child->context->gid_dropped=parent->context->gid_dropped;
    child->context->desktop_login_uid=parent && parent->user?parent->context->desktop_login_uid:0;
    child->context->account_session=parent && parent->user?parent->context->account_session:0;
    child->context->account_owner=parent && parent->user?parent->context->account_owner:0;
    child->context->account_console=parent && parent->user && parent->context->account_console;
    child->context->desktop_fsuid=parent && parent->user?parent->context->desktop_fsuid:0;
    child->context->account_uid=parent && parent->user?parent->context->account_uid:0;
    child->context->account_nnp=parent && parent->user && parent->context->account_nnp;
    child->context->creation_mask = s1_mask(parent,false,0);
    child->context->pgid = parent->context->pgid;
    child->context->sid = parent->context->sid;
    child->context->job_managed = parent->context->job_managed;

    if (process_environment_set(child, parent->environment)) {
        goto fail;
    }
    if(child->cwd!=parent->cwd)memcpy(child->cwd, parent->cwd, OS_PATH_MAX);
    if(private_fork_fault(fail_stage,5))goto fail;

    if (shared)
        child->vfork_parent = parent->pid;

    if(!vfs_held){mutex_lock(&vfs_mutex);vfs_held=true;}
    child->context->executable = parent->context->executable;
    if (child->context->executable) child->context->executable->references++;
    for (unsigned i = 0; i < OS_FD_MAX; i++)
        if (parent->fds[i]) {
            child->fds[i] = parent->fds[i];
            child->fds[i]->references++;
            child->fd_flags[i] = parent->fd_flags[i];
        }
    if(frozen && !atomic_private && !native_spawn){userns_fork_thaw(parent);frozen=false;}
    mutex_unlock(&vfs_mutex);vfs_held=false;

    uint32_t tid=pidns_view(child,child->pid);
    if ((flags & CLONE_CHILD_SETTID) &&
        !user_copy_out(&child->space, frame->r10, &tid, sizeof(tid))) {
        error=-E_FAULT;goto fail;
    }
    if (flags & CLONE_CHILD_CLEARTID)
        child->clear_tid = frame->r10;
    if(private_fork_fault(fail_stage,6) || (new_pid && pidns_verify_fault(parent,5)) || (fs_helper && s1_fault(parent,4))){error=-E_NOMEM;goto fail;}

    if (!atomic_private && !parent->context->job_managed && child->fds[0] && child->fds[0]->tty)
        tty_transfer_foreground(parent->pid, child->pid);
    if (!atomic_private && !parent->context->job_managed && child->fds[0] && child->fds[0]->pty && !child->fds[0]->pty_master)
        pty_foreground(child->fds[0]->pty, parent->pid, child->pid);

    unsigned pid = child->pid,result_pid=pidns_view(parent,child->pid);
    if(atomic_private) {
        /* Capture truthful inherited state before publication, never override
           it. This metadata is absent when the existing trace flag is off. */
        unsigned trace=private_fork?credentials_zygote_record(parent):0;
        if(trace) {
            register_t q={.rax=SYS_prctl,.rdi=39};long pn=userns_call(parent,&q),cn=userns_call(child,&q);
            q.rdi=21;long pf=userns_call(parent,&q),cf=userns_call(child,&q);
            uint32_t parent_tid_after=0,child_tid=0;
            bool parent_unchanged=tid_snapshot && user_copy_in(&parent->space,&parent_tid_after,frame->r10,4) && parent_tid_before==parent_tid_after;
            bool child_matches=user_copy_in(&child->space,&child_tid,frame->r10,4) && child_tid==pidns_view(child,child->pid);
            pr_info("G2_READY seq=%u parent=%u child=%u private_vm=1 nnp_parent=%ld nnp_child=%ld filter_parent=%ld filter_child=%ld child_tid_match=%u parent_tid_unchanged=%u\n",trace,parent->pid,child->pid,pn,cn,pf,cf,child_matches,parent_unchanged);
        }
        error=task_publish_private_fork(child,parent,fail_stage);if(error)goto fail;
        frozen=false;
    } else if(fs_helper) {error=task_publish_s1(child,parent);if(error)goto fail;}
    else {error=task_publish(child);if(error)goto fail;}

    if (shared)task_vfork_wait(pid);
    /* Native posix_spawn keeps sibling threads frozen until the temporary
       child detaches at exec/exit. The child still cannot mutate this VM. */
    if(native_spawn){userns_fork_thaw(parent);frozen=false;}
    if(fs_helper)s1_verify_live(parent,true);
    return result_pid;
fail:
    if(frozen)userns_fork_thaw(parent);
    if(vfs_held)mutex_unlock(&vfs_mutex);
    task_discard(child);
    if(new_pid)pidns_clone_abort(parent);
    if(fs_helper)s1_clone_abort(parent);
    return error;
}

#include "private_fork_selftest.inc"
