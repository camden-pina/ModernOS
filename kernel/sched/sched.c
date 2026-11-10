#include <pidns.h>
#include <userns.h>
#include <filesystem.h>
#include <credentials.h>
#include <sched.h>
#include <descriptor_tables/gdt.h>
#include <descriptor_tables/idt.h>
#include <interrupts/lapic.h>
#include <mm/pmm.h>
#include <string.h>
#include <tsc.h>
#include <panic.h>
#include <video/display.h>
#include <tty.h>
#include <pty.h>
#include <sandbox.h>
#include <drivers/audio.h>
#include <power.h>
#include <seccomp.h>
#include <seccomp_bpf.h>

static struct cpu_local cpus[MAX_CPUS];
static struct task tasks[TASK_MAX];
_Static_assert(sizeof(tasks)<=3*1024*1024,"global task table bound");
static struct task_context idle_contexts[MAX_CPUS];
static unsigned cpu_count = 1, next_pid = 1, next_cpu;
static spinlock_t scheduler_lock;
static bool ready;
static unsigned zombie_threads;

#include "generic_seccomp.inc"

void task_context_bind(struct task *t, struct task_context *context) {
    t->context = context;
    t->tgid = context->tgid;
    t->cwd = context->cwd;
    t->fds = context->fds;
    t->fd_flags = context->fd_flags;
    t->signal_actions = context->signal_actions;
}

struct file *task_file_pin(struct task *t, unsigned fd) {
    if (fd >= OS_FD_MAX || t->pin_count == OS_FD_MAX)
        return NULL;
    /* The process lock orders table mutations; VFS owns file references.
       Pins are task-owned, so fatal exits can release an interrupted syscall. */
    if (!t->syscall_locked)
        mutex_lock(&t->context->syscall_lock);
    mutex_lock(&vfs_mutex);
    struct file *file = t->fds[fd];
    if (file) {
        file->references++;
        t->file_pins[t->pin_count++] = file;
    }
    mutex_unlock(&vfs_mutex);
    if (!t->syscall_locked)
        mutex_unlock(&t->context->syscall_lock);
    return file;
}

void task_files_unpin(struct task *t) {
    if (!t->pin_count)
        return;
    mutex_lock(&vfs_mutex);
    while (t->pin_count) {
        unsigned i = --t->pin_count;
        vfs_close(t->file_pins[i]);
        t->file_pins[i] = NULL;
    }
    mutex_unlock(&vfs_mutex);
}

void task_reap_threads(void) {
    if (!__atomic_load_n(&zombie_threads, __ATOMIC_ACQUIRE))
        return;
    power_transaction_begin();
    for (unsigned i = 0; i < TASK_MAX; i++) {
        uint64_t flags = spin_lock_irqsave(&scheduler_lock);
        struct task *t = &tasks[i];
        bool reap = t->state == TASK_ZOMBIE && t->pid != t->tgid && cpus[t->cpu].current != t;
        if (reap) {
            t->state = TASK_NEW;
            __atomic_fetch_sub(&zombie_threads, 1, __ATOMIC_ACQ_REL);
        }
        spin_unlock_irqrestore(&scheduler_lock, flags);
        if (reap)
            task_discard(t);
    }
    power_transaction_end();
}

int task_terminal_cwd(struct file *master, unsigned pid, char out[OS_PATH_MAX]) {
    if (!master || !master->pty || !master->pty_master)
        return -E_PERM;
    int error = -E_SRCH;
    mutex_lock(&vfs_mutex);
    uint64_t irq = spin_lock_irqsave(&scheduler_lock);
    for (unsigned i = 0; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];
        if (t->pid != pid || t->state < TASK_RUNNABLE || t->state >= TASK_ZOMBIE)
            continue;
        error = -E_PERM;
        for (unsigned fd = 0; fd < 3; fd++) {
            struct file *stream = t->fds[fd];
            if (stream && stream->pty == master->pty && !stream->pty_master) {
                memcpy(out, t->cwd, OS_PATH_MAX);
                error = 0;
                break;
            }
        }
        break;
    }
    spin_unlock_irqrestore(&scheduler_lock, irq);
    mutex_unlock(&vfs_mutex);
    return error;
}

int task_browser_grant(unsigned parent, unsigned pid, const char *path) {
    /* Resolve as the trusted desktop, then publish a complete grant while
       VFS operations and task reaping are excluded. Never hold the scheduler
       spinlock while performing a filesystem operation. */
    struct task *scratch = kmalloc(sizeof(*scratch));
    if (!scratch)
        return -E_NOMEM;
    memset(scratch, 0, sizeof(*scratch));
    mutex_lock(&vfs_mutex);
    int e = sandbox_set_local(scratch, path);
    if (!e) {
        e = -E_SRCH;
        uint64_t flags = spin_lock_irqsave(&scheduler_lock);
        for (unsigned i = 0; i < TASK_MAX; i++) {
            struct task *t = &tasks[i];
            if (t->pid != pid || t->state < TASK_RUNNABLE || t->state >= TASK_ZOMBIE)
                continue;
            e = -E_PERM;
            if (t->ppid == parent && t->browser_sandbox) {
                memcpy(t->browser_local, scratch->browser_local, sizeof(t->browser_local));
                t->browser_local_file = scratch->browser_local_file;
                e = 0;
            }
            break;
        }
        spin_unlock_irqrestore(&scheduler_lock, flags);
    }
    mutex_unlock(&vfs_mutex);
    kfree(scratch);
    return e;
}

void task_release_input(struct task *t, struct file *input) {
    if (t->context->job_managed || !input ||
        (!input->tty && (!input->pty || input->pty_master)))
        return;
    /* Background shell children redirect stdin after fork. Restore the live
     * parent while exit/reparent is excluded, so they cannot strand input. */
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    unsigned parent = t->ppid;
    if (!parent)
        parent = 1;
    for (unsigned depth = 0; depth < TASK_MAX; depth++) {
        struct task *found = NULL;
        for (unsigned i = 0; i < TASK_MAX; i++)
            if (tasks[i].pid == parent && tasks[i].state != TASK_UNUSED) {
                found = &tasks[i];
                break;
            }
        if (!found) {
            parent = 1;
            break;
        }
        if (found->state >= TASK_RUNNABLE && found->state < TASK_ZOMBIE) {
            parent=task_input_identity(found);
            break;
        }
        parent = found->ppid;
    }
    if (input->tty)
        tty_transfer_foreground(t->tgid, parent);
    if (input->pty)
        pty_foreground(input->pty, t->tgid, parent);
    spin_unlock_irqrestore(&scheduler_lock, flags);
}


unsigned task_input_identity(const struct task *t) {
    return __atomic_load_n(&t->context->job_managed, __ATOMIC_ACQUIRE)
        ? __atomic_load_n(&t->context->pgid, __ATOMIC_ACQUIRE) : t->tgid;
}
static bool terminal_file(const struct file *f) {
    return f && (f->tty || (f->pty && !f->pty_master));
}
static bool same_terminal(const struct file *a, const struct file *b) {
    return terminal_file(a) && terminal_file(b) &&
        ((a->tty && b->tty) || (a->pty && a->pty == b->pty));
}
static unsigned terminal_foreground(const struct file *f) {
    return f->tty ? tty_job_foreground() : pty_job_foreground(f->pty);
}
static void terminal_restore(struct file *f, unsigned from, unsigned to,
                             const struct os_termios *mode) {
    if (f->tty)
        tty_job_restore(from, to, mode);
    else
        pty_job_restore(f->pty, from, to, mode);
}
/* Caller holds its process syscall lock; scheduler_lock pins identities and
   ancestry. A return capability never grants access to another terminal. */
static bool terminal_authority(struct task *t) {
    unsigned identity = task_input_identity(t);
    for (unsigned fd = 0; fd < OS_FD_MAX; fd++)
        if (terminal_file(t->fds[fd]) && terminal_foreground(t->fds[fd]) == identity)
            return true;
    return t->context->job_terminal &&
        terminal_foreground(t->context->job_terminal) == t->context->job_foreground;
}
long task_getpgid(struct task *caller, int pid, bool session) {
    if (pid < 0)
        return -E_INVAL;
    if (!pid)pid=caller->tgid;
    else {pid=pidns_resolve(caller,pid);if(!pid)return -E_SRCH;}
    long result = -E_SRCH;
    uint64_t irq = spin_lock_irqsave(&scheduler_lock);
    for (unsigned i = 0; i < TASK_MAX; i++)
        if (tasks[i].tgid == (unsigned)pid && tasks[i].state >= TASK_RUNNABLE &&
            tasks[i].state <= TASK_ZOMBIE && tasks[i].context) {
            result = pidns_view(caller,session ? tasks[i].context->sid : tasks[i].context->pgid);
            break;
        }
    spin_unlock_irqrestore(&scheduler_lock, irq);
    return result;
}
int task_setpgid(struct task *caller, int pid, int group) {
    if (pid < 0 || group < 0)
        return -E_INVAL;
    if (!pid)pid=caller->tgid;
    else {pid=pidns_resolve(caller,pid);if(!pid)return -E_SRCH;}
    if (!group)group=pid;
    else {group=pidns_resolve(caller,group);if(!group)return -E_PERM;}
    int result = -E_SRCH;
    uint64_t irq = spin_lock_irqsave(&scheduler_lock);
    struct task *target = NULL;
    for (unsigned i = 0; i < TASK_MAX; i++)
        if (tasks[i].pid == (unsigned)pid && tasks[i].pid == tasks[i].tgid &&
            tasks[i].state >= TASK_RUNNABLE && tasks[i].state < TASK_ZOMBIE) {
            target = &tasks[i]; break;
        }
    if (!target) goto done;
    result = -E_PERM;
    if ((target->tgid != caller->tgid && target->ppid != caller->tgid) ||
        target->context->sid != caller->context->sid) goto done;
    /* Parent and child make the same assignment to close the fork race. */
    if (target->context->job_managed && target->context->pgid == (unsigned)group) {
        result = 0; goto done;
    }
    if (target != caller && target->context->executed) goto done;
    bool exists = group == pid, sibling_group = false;
    for (unsigned i = 0; i < TASK_MAX; i++) {
        struct task *member = &tasks[i];
        if (member->context && member->state >= TASK_RUNNABLE && member->state <= TASK_ZOMBIE &&
            member->context->sid == caller->context->sid && member->context->pgid == (unsigned)group) {
            if (member->ppid == caller->tgid || member->tgid == caller->tgid) exists = true;
            /* Bash's pipeline children join a sibling's group after the
               shell may already have handed that group the foreground. */
            if (target->tgid == caller->tgid && caller->ppid && member->ppid == caller->ppid &&
                credentials_uid(member) == credentials_uid(caller)) {
                sibling_group = true;
                exists = true;
            }
        }
    }
    if (!terminal_authority(caller) && !sibling_group) goto done;
    if (exists) {
        result=pidns_group_set(target,group,target->context->sid);if(result)goto done;
        __atomic_store_n(&target->context->pgid, (unsigned)group, __ATOMIC_RELEASE);
        __atomic_store_n(&target->context->job_managed, true, __ATOMIC_RELEASE);
        result = 0;
    }
done:
    spin_unlock_irqrestore(&scheduler_lock, irq);
    return result;
}
/* An admitted ordinary process may create an empty Linux session, including
   a daemon with no PTY. This grants no terminal: the existing ownership and
   process-group rules still apply independently. */
long task_private_setsid(struct task *t) {
    if(!t || !t->user || !t->context || t->browser_sandbox ||
       t->context->web_role || pidns_active(t))return -E_PERM;
    /* Ordinary native daemons use setsid too. Namespace callers retain the
       existing editor admission; this never grants a controlling terminal. */
    if(t->userns && (!CONFIG_ELECTRON_PROFILE || credentials_uid(t)!=1000 ||
       credentials_gid(t)!=1000 || !userns_admitted_account(t)))return -E_PERM;
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);
    long result=-E_PERM;
    if(t->pid==t->tgid && t->context->pgid!=t->tgid && !t->context->job_terminal) {
        result=pidns_group_set(t,t->tgid,t->tgid);
        if(!result) {
            t->context->pgid=t->context->sid=t->tgid;
            t->context->job_managed=true;
            result=t->tgid;
        }
    }
    spin_unlock_irqrestore(&scheduler_lock,irq);return result;
}
/* A private PTY creator can recover an abandoned native-spawn foreground.
   A live process/group always retains ownership. No broad session override. */
bool task_terminal_access(struct task *caller, struct file *terminal) {
    if (!terminal || !terminal->pty || terminal->pty_master)
        return false;
    bool allowed = false;
    uint64_t irq = spin_lock_irqsave(&scheduler_lock);
    unsigned current = pty_job_foreground(terminal->pty), own = task_input_identity(caller);
    if (current == own)
        allowed = true;
    else if (pty_job_creator(terminal->pty) == own) {
        bool live = false;
        for (unsigned i = 0; i < TASK_MAX; i++)
            if (tasks[i].user && tasks[i].context && tasks[i].state >= TASK_RUNNABLE &&
                tasks[i].state < TASK_ZOMBIE && task_input_identity(&tasks[i]) == current) {
                live = true;
                break;
            }
        if (!live) {
            pty_job_restore(terminal->pty, current, own, NULL);
            allowed = true;
        }
    }
    spin_unlock_irqrestore(&scheduler_lock, irq);
    return allowed;
}

long task_terminal_set(struct task *caller, struct file *terminal, unsigned group) {
    if (!terminal_file(terminal)) return -E_NOTTY;
    if (!group || group > 2147483647u) return -E_INVAL;
    struct task_context *context = caller->context;
    struct file *release = NULL;
    int result = -E_PERM;
    /* Existing VFS -> scheduler lock order. Keep the PTY alive even if every
       descriptor is closed while a job runs. */
    mutex_lock(&vfs_mutex);
    uint64_t irq = spin_lock_irqsave(&scheduler_lock);
    unsigned current = terminal_foreground(terminal), own = task_input_identity(caller);
    bool reclaim = context->job_terminal && same_terminal(context->job_terminal, terminal) &&
                   (current == context->job_foreground || current == own) && group == own;
    if (!context->job_managed || (!reclaim && current != own)) goto done;
    bool eligible = group == own;
    for (unsigned i = 0; i < TASK_MAX && !eligible; i++) {
        struct task *member = &tasks[i];
        if (member->context && member->state >= TASK_RUNNABLE && member->state <= TASK_ZOMBIE &&
            member->context->sid == context->sid && member->context->pgid == group &&
            member->ppid == caller->tgid) eligible = true;
    }
    if (!eligible || (!reclaim && context->job_terminal)) goto done;
    if (reclaim) {
        terminal_restore(terminal, current, own, &context->job_termios);
        release = context->job_terminal;
        context->job_terminal = NULL;
        context->job_foreground = 0;
    } else if (group != own) {
        terminal->references++;
        context->job_terminal = terminal;
        context->job_foreground = group;
        if (terminal->tty) tty_get_config(&context->job_termios);
        else pty_job_config(terminal->pty, &context->job_termios);
        terminal_restore(terminal, current, group, NULL);
    }
    result = 0;
done:
    spin_unlock_irqrestore(&scheduler_lock, irq);
    if (release) vfs_close(release);
    mutex_unlock(&vfs_mutex);
    return result;
}
/* No VFS operations here: the capability's reference lives until reap. */
static void terminal_controller_exit_locked(struct task *t) {
    struct task_context *context = t->context;
    if (!context->job_terminal) return;
    unsigned replacement = 1;
    for (unsigned i = 0; i < TASK_MAX; i++)
        if (tasks[i].tgid == t->ppid && tasks[i].context &&
            tasks[i].state >= TASK_RUNNABLE && tasks[i].state < TASK_ZOMBIE) {
            replacement = task_input_identity(&tasks[i]); break;
        }
    terminal_restore(context->job_terminal, context->job_foreground, replacement,
                     &context->job_termios);
    for (unsigned i = 0; i < TASK_MAX; i++)
        if (tasks[i].context && tasks[i].context->pgid == context->job_foreground &&
            tasks[i].context->sid == context->sid && tasks[i].state >= TASK_RUNNABLE &&
            tasks[i].state < TASK_ZOMBIE && tasks[i].tgid != t->tgid)
            __atomic_fetch_or(&tasks[i].signal_pending, 1, __ATOMIC_RELEASE); /* SIGHUP */
}

static_assert(offsetof(struct cpu_local, handoff) == 24);
static uint8_t initial_fpu[512] __attribute__((aligned(16)));
uint32_t system_num_cpus = 1;
bool is_smp_enabled;

static void write_msr(unsigned n, uint64_t v) {
    __asm__ volatile("wrmsr" ::"c"(n), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

static uint64_t read_msr(unsigned n) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(n));
    return lo | ((uint64_t)hi << 32);
}

static void setup_cpu(struct cpu_local *c) {
    c->self = c;
    write_msr(0xc0000101, (uintptr_t)c);
    write_msr(0xc0000102, 0);
    user_cpu_init();
    syscall_cpu_init();
    c->current = &c->idle;
    task_context_bind(&c->idle, &idle_contexts[c->id]);
    c->idle.idle = true;
    c->idle.cpu = c->id;
    c->idle.state = TASK_RUNNING;
    c->idle.stack_top = c->kernel_stack;
    memcpy(c->idle.fpu, initial_fpu, 512);
    gdt_set_rsp0(c->id, c->kernel_stack);
}

unsigned sched_cpu_count(void) { return cpu_count; }
struct cpu_local *sched_cpu(unsigned id) { return id < cpu_count ? &cpus[id] : NULL; }

bool sched_power_drained(void) {
    bool drained = true;
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    for (unsigned i = 0; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_UNUSED || t->power_role)
            continue;
        if (__atomic_load_n(&t->held_mutexes, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&t->no_park_depth, __ATOMIC_ACQUIRE) || t->power_creator ||
            __atomic_load_n(&t->vfork_parent, __ATOMIC_ACQUIRE)) {
            drained = false;
            break;
        }
    }
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return drained;
}

bool sched_ready(void) {
    return __atomic_load_n(&ready, __ATOMIC_ACQUIRE);
}

static void enqueue(struct task *t) {
    struct cpu_local *c = &cpus[t->cpu];
    t->next = NULL;
    if (c->tail)
        c->tail->next = t;
    else
        c->head = t;
    c->tail = t;
}

static void kernel_entry(void) {
    struct task *t = this_cpu()->current;
    t->entry(t->argument);
    task_exit(0);
}

static unsigned take_pid(unsigned *sequence) {
    if (!*sequence || *sequence > INT32_MAX)
        return 0;
    return (*sequence)++;
}
#if CONFIG_SOCKET_CREDENTIALS_VERIFY
void sched_pid_sequence_selftest(void) {
    unsigned sequence=INT32_MAX-1;
    kassert(take_pid(&sequence)==INT32_MAX-1 && take_pid(&sequence)==INT32_MAX);
    kassert(sequence==(unsigned)INT32_MAX+1 && !take_pid(&sequence) && sequence==(unsigned)INT32_MAX+1);
    sequence=UINT32_MAX;kassert(!take_pid(&sequence) && sequence==UINT32_MAX);
    sequence=0;kassert(!take_pid(&sequence) && !sequence);
}
#endif
struct task *task_allocate(const char *name, bool user, unsigned ppid, int affinity) {
    /* Allocation-to-publication belongs to the creating task, which stays
       pinned. Keep its lifecycle transaction live across every failure path. */
    power_transaction_begin();
    task_reap_threads();
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    struct task *t = NULL;
    for (unsigned i = 0; i < TASK_MAX; i++)
        if (tasks[i].state == TASK_UNUSED) {
            t = &tasks[i];
            break;
        }

    /* Never wrap/reuse a process identity that can remain in queued IPC.
       Linux's public PID ABI is signed; fail allocation at finite exhaustion. */
    unsigned pid = t ? take_pid(&next_pid) : 0;
    if (!pid)
        t = NULL;
    if (t) {
        memset(t, 0, sizeof(*t));
        t->state = TASK_NEW;
        t->power_creator = sched_ready() ? this_cpu()->current : NULL;
        t->pid = pid;
        t->ppid = ppid;
        if (affinity >= 0)
            t->cpu = (unsigned)affinity % cpu_count;
        else {
            /* Admission-time balancing. Tasks keep their CPU while alive,
               avoiding shared address-space TLB migration/shootdown races. */
            unsigned best = next_cpu++ % cpu_count, least = UINT32_MAX;
            for (unsigned step = 0; step < cpu_count; step++) {
                unsigned c = (best + step) % cpu_count, load = 0;
                if (!__atomic_load_n(&cpus[c].online, __ATOMIC_ACQUIRE))
                    continue;
                for (unsigned j = 0; j < TASK_MAX; j++)
                    if (&tasks[j] != t && tasks[j].cpu == c && tasks[j].state >= TASK_RUNNABLE &&
                        tasks[j].state < TASK_ZOMBIE)
                        load++;
                if (load < least) {
                    least = load;
                    t->cpu = c;
                }
            }
        }

        t->user = user;
        snprintf(t->name, sizeof(t->name), "%s", name);
        memcpy(t->fpu, initial_fpu, 512);
    }

    spin_unlock_irqrestore(&scheduler_lock, f);
    if (!t) {
        pr_warn("SCHED_TASK_ALLOCATION_DENIED limit=%u pid=%u next_pid=%u\n",TASK_MAX,pid,next_pid);
        power_transaction_end();
        return NULL;
    }

    struct task_context *context = kmalloc(sizeof(*context));
    if (!context) {
        task_discard(t);
        return NULL;
    }

    memset(context, 0, sizeof(*context));
    context->references = 1;
    context->tgid = context->pgid = context->sid = t->pid;
    strcpy(context->cwd, "/");
    task_context_bind(t, context);

    t->stack_phys = pmm_alloc_pages(TASK_STACK_PAGES);
    if (!t->stack_phys) {
        task_discard(t);
        return NULL;
    }
    memset(phys_ptr(t->stack_phys), 0, TASK_STACK_PAGES * PAGE_SIZE);
    t->stack_top = (uintptr_t)phys_ptr(t->stack_phys) + TASK_STACK_PAGES * PAGE_SIZE;

    /* Kernel entry obeys the SysV entry alignment, like a CALL with a
       return address. User entry receives an aligned ELF initial stack. */
    t->frame = (void *)(t->stack_top - 8 - sizeof(register_t));
    *t->frame = (register_t){.rip = (uintptr_t)kernel_entry,
                             .cs = KERNEL_CS,
                             .rflags = 0x202,
                             .user_rsp = t->stack_top - 8,
                             .ss = GDT_KERNEL_DS};

    return t;
}

void task_discard(struct task *t) {
    power_transaction_begin();
    if (t->held_mutexes || t->no_park_depth)
        panic("power: discarding resource owner");
    struct task *creator = t->power_creator;
    if (creator && creator != this_cpu()->current)
        panic("power: cross-task allocation completion");
    process_cleanup(t);
    if (t->stack_phys)
        pmm_free_pages(t->stack_phys, TASK_STACK_PAGES);
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    memset(t, 0, sizeof(*t));
    spin_unlock_irqrestore(&scheduler_lock, f);
    if (creator)
        power_transaction_end();
    power_transaction_end();
}

int task_publish(struct task *t) {
    userns_budget_finish(&t->userns_budget_claim,true);
    struct task *creator = t->power_creator;
    if (creator && creator != this_cpu()->current)
        panic("power: cross-task publication");
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    if(pidns_active(t) && t->context->exiting){spin_unlock_irqrestore(&scheduler_lock,f);return -E_AGAIN;}
    int error=pidns_publish(t);
    if(error){spin_unlock_irqrestore(&scheduler_lock,f);return error;}
    t->context->live++;
    userns_task_live(t,true);
    if (t->context->exiting)
        __atomic_store_n(&t->kill_signal, 9, __ATOMIC_RELEASE);
    t->state = TASK_RUNNABLE;
    t->power_creator = NULL;
    enqueue(t);
    spin_unlock_irqrestore(&scheduler_lock, f);
    if (creator)
        power_transaction_end();
    return 0;
}

/* Private namespace threads: validate identity/state and TID memory under the
 * publication lock. All shared-VM threads retain their existing same CPU. */
int task_publish_thread_p1(struct task *t,struct task *parent,uint64_t parent_tid,uint64_t child_tid) {
    if(!CONFIG_PID_NAMESPACE_TEST || t->context!=parent->context || t->cpu!=parent->cpu ||
       parent!=this_cpu()->current || t->power_creator!=parent || !parent->syscall_locked)return -E_PERM;
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);int error=0;
    if(parent->context->exiting){error=-E_AGAIN;goto done;}
    if((parent_tid && !user_access(&parent->space,parent_tid,4,true)) ||
       (child_tid && !user_access(&t->space,child_tid,4,true))){error=-E_FAULT;goto done;}
    error=pidns_publish(t);if(error)goto done;
    uint32_t local=pidns_view(t,t->pid);
    if(parent_tid)kassert(user_copy_out(&parent->space,parent_tid,&local,4));
    if(child_tid)kassert(user_copy_out(&t->space,child_tid,&local,4));
    t->context->live++;userns_task_live(t,true);t->state=TASK_RUNNABLE;t->power_creator=NULL;enqueue(t);
 done:spin_unlock_irqrestore(&scheduler_lock,irq);
    if(!error)power_transaction_end();
    return error;
}

int task_publish_s1(struct task *t,struct task *parent) {
    if(!CONFIG_FILESYSTEM_SANDBOX_TEST || parent!=this_cpu()->current || !parent->syscall_locked ||
       t->power_creator!=parent || t->context==parent->context || t->space.vm!=parent->space.vm ||
       t->cpu!=parent->cpu || t->vfork_parent!=parent->pid)return -E_PERM;
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);int e=-E_AGAIN;
    if(parent->context->exiting || parent->context->live!=1 || parent->vfork_parent)goto done;
    if(s1_fault(parent,5)){e=-E_NOMEM;goto done;}
    e=pidns_publish(t);if(e)goto done;
    if(s1_fault(parent,6)){e=-E_NOMEM;goto done;}
    if(s1_fault(parent,7)){e=-E_NOMEM;goto done;}
    e=s1_publish(t,parent);if(e)goto done;
#if CONFIG_ZYGOTE_TRACE
    unsigned trace=credentials_zygote_record(parent);
    if(trace) {
        register_t q={.rax=SYS_prctl,.rdi=39};long pn=userns_call(parent,&q),cn=userns_call(t,&q);
        q.rdi=21;long pf=userns_call(parent,&q),cf=userns_call(t,&q);
        pr_info("S1_HELPER_COMMIT seq=%u parent_outer=%u child_outer=%u child_inner=%u shared_vm=1 copied_FD_table=1 separate_context=1 distinct_TLS=%u nnp_parent=%ld nnp_child=%ld filter_parent=%ld filter_child=%ld\n",trace,parent->pid,t->pid,pidns_view(t,t->pid),t->fsbase!=parent->fsbase,pn,cn,pf,cf);
    }
#endif
    t->context->live++;userns_task_live(t,true);t->state=TASK_RUNNABLE;t->power_creator=NULL;enqueue(t);
 done:spin_unlock_irqrestore(&scheduler_lock,irq);if(!e)power_transaction_end();return e;
}

int task_kernel(const char *name, void (*entry)(void *), void *arg, unsigned cpu) {
    struct task *t = task_allocate(name, false, 0, cpu);
    if (!t)
        return -E_NOMEM;
    t->entry = entry;
    t->argument = arg;
    task_publish(t);
    return t->pid;
}

/* G2 only: the caller still owns its context lock and fork barrier. Commit
 * publication and thaw under the same scheduler lock so no sibling can race
 * an exiting-parent decision. Ordinary task_publish/C2 routes are unchanged. */
int task_publish_private_fork(struct task *t,struct task *parent,unsigned fail_stage) {
    if(!CONFIG_PRIVATE_FORK_TEST || parent!=this_cpu()->current || t->power_creator!=parent ||
       t->context==parent->context || t->space.vm==parent->space.vm ||
       !t->space.root || !t->userns || t->space.vm->budget!=parent->space.vm->budget ||
       credentials_uid(t)!=1000 || credentials_gid(t)!=1000)return -E_PERM;
    kassert(!t->userns_budget_claim.active);
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);
    if(parent->context->exiting || parent->vfork_parent || parent->context->fork_owner!=parent->pid ||
       !parent->syscall_locked || parent->context->syscall_lock.owner!=parent) {
        spin_unlock_irqrestore(&scheduler_lock,irq);return -E_AGAIN;
    }
#if CONFIG_PRIVATE_FORK_VERIFY
    if(private_fork_fault(fail_stage,7)) {
        spin_unlock_irqrestore(&scheduler_lock,irq);return -E_NOMEM;
    }
#else
    (void)fail_stage;
#endif
    int error=pidns_publish(t);
    if(error){spin_unlock_irqrestore(&scheduler_lock,irq);return error;}
    /* No fallible work remains. Preserve the ordinary foreground handoff,
       using the established scheduler -> terminal spinlock order, only at
       commit so a rejected publication cannot strand terminal ownership. */
    if(!parent->context->job_managed && t->fds[0]) {
        if(t->fds[0]->tty)tty_transfer_foreground(parent->pid,t->pid);
        if(t->fds[0]->pty && !t->fds[0]->pty_master)pty_foreground(t->fds[0]->pty,parent->pid,t->pid);
    }
    t->context->live++;
    userns_task_live(t,true);
    t->state=TASK_RUNNABLE;t->power_creator=NULL;enqueue(t);
    parent->context->fork_owner=0;
    spin_unlock_irqrestore(&scheduler_lock,irq);
    power_transaction_end(); /* matching task_allocate's creator pin */
    return 0;
}

static long futex_wake_locked(uint64_t root, uint64_t address, unsigned count, uint32_t mask) {
    long woken = 0;
    for (unsigned i = 0; i < TASK_MAX && (unsigned)woken < count; i++) {
        struct task *waiter = &tasks[i];
        if (!waiter->futex_waiting || waiter->futex_root != root ||
            waiter->futex_address != address || !(waiter->futex_mask & mask))
            continue;
        waiter->futex_waiting = false;
        waiter->futex_result = 0;
        if (waiter->state == TASK_SLEEPING) {
            waiter->state = TASK_RUNNABLE;
            enqueue(waiter);
        }
        woken++;
    }
    return woken;
}

/* All helpers below require scheduler_lock, including signal_deliver's
   default-stop callback. Parent events are consumed by wait4, never by reaping. */
static void notify_job_parent_locked(struct task *t) {
    for(unsigned i=0;i<TASK_MAX;i++) {
        struct task *p=&tasks[i];
        if(p->tgid==t->ppid && p->state>=TASK_RUNNABLE && p->state<TASK_ZOMBIE &&
           p->signal_actions[16].handler>1 && !(p->signal_actions[16].flags&SA_NOCLDSTOP))
            __atomic_fetch_or(&p->signal_pending,1ULL<<16,__ATOMIC_RELEASE);
    }
}
void task_stop_request_locked(struct task *t,unsigned signal) {
    struct task_context *c=t->context;
    if(c->exiting)return;
    /* Generating a stop discards pending SIGCONT for every group member. */
    for(unsigned i=0;i<TASK_MAX;i++)if(tasks[i].context==c)
        if(!pidns_signal_drop(&tasks[i],18))__atomic_fetch_and(&tasks[i].signal_pending,~(1ULL<<17),__ATOMIC_ACQ_REL);
    c->continue_event=false;
    if(!c->stop_requested) {
        c->stop_signal=signal;c->stop_event=false;
        __atomic_store_n(&c->stop_requested,signal,__ATOMIC_RELEASE);
    }
}
static void stop_completed_locked(struct task *t) {
    unsigned count=0;
    for(unsigned i=0;i<TASK_MAX;i++)
        if(tasks[i].context==t->context&&tasks[i].state==TASK_STOPPED)count++;
    if(t->context->stop_requested&&count==t->context->live&&!t->context->stopped) {
        t->context->stopped=true;t->context->stop_event=true;
        notify_job_parent_locked(t);
    }
}
static void park_stopped_locked(struct task *t) {
    if(!t->context->stop_requested||task_pending_signal(t)||t->context->exiting)return;
    t->state=TASK_STOPPED;
    stop_completed_locked(t);
}
static void continue_group_locked(struct task *t) {
    struct task_context *c=t->context;bool was_stopped=c->stopped;
    __atomic_store_n(&c->stop_requested,0,__ATOMIC_RELEASE);
    c->stopped=false;c->stop_event=false;
    for(unsigned i=0;i<TASK_MAX;i++)if(tasks[i].context==c) {
        for(unsigned sig=19;sig<=22;sig++)if(!pidns_signal_drop(&tasks[i],sig))__atomic_fetch_and(&tasks[i].signal_pending,~(1ULL<<(sig-1)),__ATOMIC_ACQ_REL);
        if(tasks[i].state==TASK_STOPPED){tasks[i].state=TASK_RUNNABLE;enqueue(&tasks[i]);}
    }
    if(was_stopped){c->continue_event=true;notify_job_parent_locked(t);}
}
/* Pure timing waits own no mutex, descriptor pin or transaction. Park them
   in place so their original monotonic deadline survives a default stop; do
   not manufacture EINTR for applications which installed no signal handler. */
bool task_stop_checkpoint(struct task *t) {
    bool stopped=false;
    for(;;) {
        uint64_t irq=spin_lock_irqsave(&scheduler_lock);
        uint64_t pending=__atomic_load_n(&t->signal_pending,__ATOMIC_ACQUIRE)&~t->signal_mask;
        if(!t->context->stop_requested&&!task_pending_signal(t))
            for(unsigned sig=20;sig<=22;sig++)
                if((pending&(1ULL<<(sig-1)))&&!t->signal_actions[sig-1].handler) {
                    if(!pidns_signal_drop(t,sig))__atomic_fetch_and(&t->signal_pending,~(1ULL<<(sig-1)),__ATOMIC_ACQ_REL);
                    task_stop_request_locked(t,sig);break;
                }
        if(t->context->stop_requested&&!task_pending_signal(t)&&!t->context->exiting) {
            if(t->held_mutexes||t->no_park_depth)panic("job control: unsafe timing stop");
            park_stopped_locked(t);stopped=true;
            __atomic_store_n(&scheduler_lock.value,0,__ATOMIC_RELEASE);
            __asm__ volatile("int $0xf0" ::: "memory");
            irq_restore(irq);continue;
        }
        spin_unlock_irqrestore(&scheduler_lock,irq);return stopped;
    }
}
/* Called only after syscall locks, descriptor pins and transactions are cleaned
   up. The timer path stops only ring-3 frames. No stopped task owns a kernel
   mutex or returns to untrusted registers without the existing validation. */
void task_signal_checkpoint(struct task *t,register_t *r,bool restart,uint64_t mask) {
    bool stopped=false,restart_pending=restart;
    for(;;) {
        uint64_t irq=spin_lock_irqsave(&scheduler_lock);
        if(!task_pending_signal(t)&&!t->context->stop_requested) {
            unsigned depth=t->signal_depth;
            signal_deliver(t,r,restart_pending,mask);
            if(t->signal_depth!=depth)restart_pending=false;
        }
        if(t->context->stop_requested&&!task_pending_signal(t)&&!t->context->exiting) {
            if(t->held_mutexes||t->no_park_depth)panic("job control: unsafe stop checkpoint");
            park_stopped_locked(t);stopped=true;
            __atomic_store_n(&scheduler_lock.value,0,__ATOMIC_RELEASE);
            __asm__ volatile("int $0xf0" ::: "memory");
            irq_restore(irq);continue;
        }
        spin_unlock_irqrestore(&scheduler_lock,irq);break;
    }
    if(stopped&&restart_pending){r->rip-=2;r->rax=r->rcx;}
}
int task_background_terminal(struct task *t,unsigned signal) {
    if(!t->context->job_managed)return -E_PERM;
    if((t->signal_mask&(1ULL<<(signal-1)))||t->signal_actions[signal-1].handler==1)
        return -E_IO; /* Still denied; never bypass foreground/controller checks. */
    int e=task_kill_group(t->context->pgid,signal);
    return e?e:-E_INTR;
}

static void group_exit_locked(struct task *t, int status) {
    __atomic_store_n(&t->context->stop_requested,0,__ATOMIC_RELEASE);
    t->context->stopped=t->context->stop_event=t->context->continue_event=false;
    if (!t->context->exiting) {
        t->context->exiting = true;
        t->context->exit_status = status;
    }
    for (unsigned i = 0; i < TASK_MAX; i++)
        if (tasks[i].context == t->context && tasks[i].state >= TASK_RUNNABLE &&
            tasks[i].state < TASK_ZOMBIE)
        {
            __atomic_store_n(&tasks[i].kill_signal, 9, __ATOMIC_RELEASE);
            if(tasks[i].state==TASK_STOPPED){tasks[i].state=TASK_RUNNABLE;enqueue(&tasks[i]);}
        }
}

long task_subreaper(struct task *t,register_t *r) {
    if(!CONFIG_ELECTRON_PROFILE)return -E_NOSYS;
    /* This initial implementation is for the trusted outer supervisor.
       Namespace init adoption remains authoritative inside PID namespaces. */
    if(credentials_restricted(t) || pidns_active(t))return -E_PERM;
    if(r->rdx || r->r10 || r->r8 || (r->rdi==36 && r->rsi>1))return -E_INVAL;
    uint64_t flags=spin_lock_irqsave(&scheduler_lock);
    unsigned value=t->context->child_subreaper;
    if(r->rdi==36)t->context->child_subreaper=r->rsi;
    spin_unlock_irqrestore(&scheduler_lock,flags);
    return r->rdi==36?0:user_copy_out(&t->space,r->rsi,&value,sizeof(value))?0:-E_FAULT;
}

/* scheduler_lock held. Only substitute an explicit outer ancestor when the
   PID namespace machinery would otherwise adopt into the root init process. */
static unsigned orphan_reaper(struct task *child,struct task *dead) {
    unsigned reaper=pidns_orphan_reaper(child,dead->tgid);
    if(reaper!=1)return reaper;
    unsigned ancestor=dead->ppid;
    for(unsigned depth=0;ancestor>1 && depth<TASK_MAX;depth++) {
        struct task *parent=NULL;
        for(unsigned i=0;i<TASK_MAX;i++)if(tasks[i].tgid==ancestor && tasks[i].context &&
            tasks[i].context->live && tasks[i].state>=TASK_RUNNABLE && tasks[i].state<TASK_ZOMBIE) {
            parent=&tasks[i];break;
        }
        if(!parent)break;
        if(parent->context->child_subreaper)return parent->tgid;
        ancestor=parent->ppid;
    }
    return reaper;
}

static void become_zombie(struct task *t, int status) {
    if (t->context->exiting)
        status = t->context->exit_status;
    unsigned trace=credentials_zygote_record(t);
    if(trace)pr_info("G1_EXIT seq=%u pid=%u tid=%u parent=%u status=%d group_exiting=%u live_before=%u\n",
        trace,t->tgid,t->pid,t->ppid,status,t->context->exiting,t->context->live);
    if (t->clear_tid) {
        /* scheduler_lock is held. A zombie/borrowed-space reference retains
           pages but cannot be a userspace joiner. Look for another live holder
           of this VM, including a suspended vfork parent in another context. */
        bool shared_vm = false;
        if (__atomic_load_n(&t->space.vm->references, __ATOMIC_ACQUIRE) > 1)
            for (unsigned i = 0; i < TASK_MAX; i++)
                if (&tasks[i] != t && tasks[i].state >= TASK_RUNNABLE &&
                    tasks[i].state < TASK_ZOMBIE && tasks[i].space.vm == t->space.vm) {
                    shared_vm = true;
                    break;
                }
        if (shared_vm) {
            uint32_t zero = 0;
            user_copy_out(&t->space, t->clear_tid, &zero, 4);
            futex_wake_locked(t->space.root, t->clear_tid, 1, UINT32_MAX);
        }
        t->clear_tid = 0;
    }
    t->futex_waiting = false;
    t->state = TASK_ZOMBIE;
    userns_task_live(t,false);
    t->exit_status = status;
    if (t->pid != t->tgid)
        __atomic_fetch_add(&zombie_threads, 1, __ATOMIC_RELEASE);
    t->context->live--;
    if(t->context->live)stop_completed_locked(t);
    s1_helper_exit(t);
    __atomic_store_n(&t->vfork_parent, 0, __ATOMIC_RELEASE);
    if (t->context->live)
        return;
    terminal_controller_exit_locked(t);
    if(pidns_init_exit(t))for(unsigned i=0;i<TASK_MAX;i++) {
        struct task *victim=&tasks[i];
        if(victim->user && victim->context!=t->context && victim->state>=TASK_RUNNABLE && victim->state<TASK_ZOMBIE && pidns_dying(victim))group_exit_locked(victim,9);
    }
    if (t->context->web_role == OS_WEB_BROKER) {
        for (unsigned i = 0; i < TASK_MAX; i++) {
            struct task *child = &tasks[i];
            if (child->ppid == t->tgid && child->context &&
                child->context->web_role == OS_WEB_RENDERER &&
                child->context->web_domain == t->tgid && child->state >= TASK_RUNNABLE &&
                child->state < TASK_ZOMBIE)
                group_exit_locked(child, 9);
        }
    }
    for (unsigned i = 0; i < TASK_MAX; i++) {
        struct task *parent = &tasks[i];
        if (parent->tgid == t->ppid && parent->state >= TASK_RUNNABLE &&
            parent->state < TASK_ZOMBIE && parent->signal_actions[16].handler > 1) {
            unsigned status_code=(status&127)?2:1,status_value=(status&127)?status&127:(unsigned)status>>8;
            int recorded=pidns_signal_record(t,parent,17,status_code,status_value);kassert(recorded>=0);
            if(!recorded)__atomic_fetch_or(&parent->signal_pending,1ULL<<16,__ATOMIC_RELEASE);
        }
    }
    for (unsigned i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].ppid == t->tgid)
            tasks[i].ppid = orphan_reaper(&tasks[i],t);
}

register_t *sched_interrupt(register_t *frame) {
    if (!sched_ready())
        return frame;

    struct cpu_local *c = this_cpu();
    if (frame->interrupt == 0xe0 && !c->id && !power_frozen())
        audio_tick();

    spin_lock_irqsave(&scheduler_lock);
    struct task *old = c->current;
    old->frame = frame;
    if (frame->interrupt == 0xe0) {
        c->ticks++;
        old->runtime_ticks++;
        if (frame->cs & 3)
            old->user_ticks++;
        else
            old->system_ticks++;
        if (old->context) {
            if (frame->cs & 3)
                old->context->user_ticks++;
            else
                old->context->system_ticks++;
        }
    }

    uint64_t now = tsc_uptime_us();
    for (unsigned i = 0; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];
        if (t->cpu == c->id && t->state >= TASK_RUNNABLE && t->state < TASK_ZOMBIE &&
            t->context->real_timer_at && t->context->real_timer_at <= now) {
            /* Real interval timers coalesce missed expirations into SIGALRM.
               A masked signal remains pending; repeating deadlines retain
               their original phase instead of drifting with scheduler delay. */
            int recorded=pidns_signal_record(NULL,t,14,128,0);kassert(recorded>=0);
            if(!recorded)__atomic_fetch_or(&t->signal_pending,1ULL<<13,__ATOMIC_RELEASE);
            uint64_t delay = t->context->real_timer_interval;
            if (delay) {
                uint64_t remaining = delay - (now - t->context->real_timer_at) % delay;
                t->context->real_timer_at = remaining <= UINT64_MAX - now ? now + remaining : 0;
            } else
                t->context->real_timer_at = 0;
        }
        if (t->cpu == c->id && t->state == TASK_SLEEPING &&
            (task_pending_signal(t) || signal_pending(t) || t->wake_at <= now)) {
            if (t->futex_waiting) {
                t->futex_waiting = false;
                t->futex_result =
                    task_pending_signal(t) || signal_pending(t) ? -E_INTR : -E_TIMEDOUT;
            }
            t->state = TASK_RUNNABLE;
            enqueue(t);
        }
    }

    /* Never abandon a kernel stack that might own a mutex. A kill is
       delivered when the interrupted context is ring 3, or at syscall exit. */
    if ((frame->cs & 3) && !task_pending_signal(old) && !old->context->stop_requested)
        signal_deliver(old, frame, false, old->signal_mask);
    if ((frame->cs & 3) && old->context->stop_requested && !task_pending_signal(old))
        park_stopped_locked(old);
    unsigned signal = task_pending_signal(old);
    if (signal && (frame->cs & 3)) {
        group_exit_locked(old, signal);
        become_zombie(old, signal);
    }

    if (old->state == TASK_RUNNING) {
        old->state = TASK_RUNNABLE;
        if (!old->idle)
            enqueue(old);
    }

    struct task *previous = NULL, *next = c->head;
    while (next) {
        if(next->user&&(next->frame->cs&3)&&next->context->stop_requested&&!task_pending_signal(next)) {
            struct task *after=next->next;
            if(previous)previous->next=after;else c->head=after;
            if(c->tail==next)c->tail=previous;
            next->next=NULL;park_stopped_locked(next);next=after;continue;
        }
        if((!next->context->fork_owner || next->context->fork_owner==next->pid) && power_task_may_run(next))break;
        previous=next;next=next->next;
    }
    if (next) {
        if (previous)
            previous->next = next->next;
        else
            c->head = next->next;
        if (c->tail == next)
            c->tail = previous;
        next->next = NULL;
    } else
        next = &c->idle;
    next->state = TASK_RUNNING;

    if (next != old) {
        /* Kernel code does not use FPU/SIMD; the isolated video-copy assembly
           preserves its registers. Keep live user state through input/idle
           workers. Save/restore only when a different user process is selected.
           The task array is permanent; the PID check rejects a reaped/reused
           slot before writing saved state, under the scheduler lock. */
        if (next->user && c->fpu_pid != next->pid) {
            struct task *live = c->fpu_task;
            if (live && live->pid == c->fpu_pid && live->state >= TASK_RUNNABLE &&
                live->state < TASK_ZOMBIE) {
                __asm__ volatile("fxsave64 %0" : "=m"(live->fpu));
                live->fsbase = read_msr(0xc0000100);
            }
            __asm__ volatile("fxrstor64 %0" ::"m"(next->fpu));
            write_msr(0xc0000100, next->fsbase);
            c->fpu_pid = next->pid;
            c->fpu_task = next;
        }
        /* Lazy kernel address-space borrowing avoids flushing the desktop's
           TLB on every 1 ms input poll. A real reference prevents an address
           space from being freed while a kernel worker is running in it. */
        const struct address_space *borrow = old->user && old->state != TASK_ZOMBIE ? &old->space
                                             : !old->user && old->borrowed_space.root
                                                 ? &old->borrowed_space
                                                 : NULL;
        if (!next->user && borrow)
            user_space_share(&next->borrowed_space, borrow);
        c->current = next;
        c->kernel_stack = next->stack_top;
        gdt_set_rsp0(c->id, next->stack_top);
        uint64_t root = next->user                  ? next->space.root
                        : next->borrowed_space.root ? next->borrowed_space.root
                                                    : kernel_cr3();
        uint64_t active;
        __asm__ volatile("mov %%cr3,%0" : "=r"(active));
        if ((active & 0x000ffffffffff000ULL) != root)
            __asm__ volatile("mov %0,%%cr3" ::"r"(root) : "memory");
        if (!old->user)
            user_space_destroy(&old->borrowed_space);
        next->switches++;
        c->switches++;
    }
    register_t *result = next->frame;
    /* The old stack cannot be reaped until assembly has changed RSP.
       Keep the lock across the C return and release it on the new stack. */
    c->handoff = 1;
    return result;
}

void sched_finish_switch(void) {
    this_cpu()->handoff = 0;
    __atomic_store_n(&scheduler_lock.value, 0, __ATOMIC_RELEASE);
}

void sched_yield(void) {
    if (sched_ready())
        __asm__ volatile("int $0xf0" ::: "memory");
}

void sched_sleep(uint64_t us) {
    if (!sched_ready())
        return;
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    struct task *t = this_cpu()->current;
    t->wake_at = tsc_uptime_us() + min(us, UINT64_MAX - tsc_uptime_us());
    t->state = TASK_SLEEPING;
    /* Keep IF clear across state change and the trap, preventing a timer
       from resuming this task before its voluntary switch is saved. */
    __atomic_store_n(&scheduler_lock.value, 0, __ATOMIC_RELEASE);
    __asm__ volatile("int $0xf0" ::: "memory");
    irq_restore(f);
}

void task_vfork_wait(unsigned pid) {
    for (;;) {
        uint64_t f = spin_lock_irqsave(&scheduler_lock);
        bool blocked = false;
        for (unsigned i = 0; i < TASK_MAX; i++)
            if (tasks[i].pid == pid && __atomic_load_n(&tasks[i].vfork_parent, __ATOMIC_ACQUIRE)) {
                blocked = true;
                break;
            }
        spin_unlock_irqrestore(&scheduler_lock, f);
        if (!blocked)
            return;
        /* Even a signal cannot resume the parent's userspace while its VM
           and stack are borrowed. The child may still be killed normally. */
        sched_sleep(1000);
    }
}

void task_real_timer(struct task *t, bool set, uint64_t value, uint64_t interval,
                     struct os_itimerval *old) {
    uint64_t f = spin_lock_irqsave(&scheduler_lock), now = tsc_uptime_us();
    uint64_t remaining = t->context->real_timer_at > now ? t->context->real_timer_at - now : 0;
    *old = (struct os_itimerval){.interval = {t->context->real_timer_interval / 1000000,
                                              t->context->real_timer_interval % 1000000},
                                 .value = {remaining / 1000000, remaining % 1000000}};
    if (set) {
        t->context->real_timer_interval = interval;
        t->context->real_timer_at = value ? now + min(value, UINT64_MAX - now) : 0;
    }
    spin_unlock_irqrestore(&scheduler_lock, f);
}

void task_exit(int status) {
    power_transaction_begin();
    display_drop(this_cpu()->current->pid);
    struct task *exiting = this_cpu()->current;
    if (exiting->syscall_locked) {
        exiting->syscall_locked = false;
        mutex_unlock(&exiting->context->syscall_lock);
    }
    task_files_unpin(exiting);
    /* A thread's exit preserves shared descriptors. The final task releases
       pipe writers immediately, including when the leader exited earlier. */
    if (exiting->context->live == 1) {
        mutex_lock(&vfs_mutex);
        for (unsigned i = 0; i < OS_FD_MAX; i++) {
            vfs_record_release(exiting->fds[i], exiting->tgid);
            vfs_close(exiting->fds[i]);
            exiting->fds[i] = NULL;
            exiting->fd_flags[i] = 0;
        }
        mutex_unlock(&vfs_mutex);
    }
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    struct task *t = this_cpu()->current;
    power_task_exit();
    become_zombie(t, status);
    __atomic_store_n(&scheduler_lock.value, 0, __ATOMIC_RELEASE);
    __asm__ volatile("int $0xf0" ::: "memory");
    irq_restore(f);
    panic("zombie task resumed");
}

void task_exit_group(int status) {
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    group_exit_locked(this_cpu()->current, status);
    spin_unlock_irqrestore(&scheduler_lock, flags);
    task_exit(status);
}

long task_futex(struct task *t, register_t *r) {
    unsigned op = r->rsi & ~(128u | 256u);
    if (r->rsi & ~(127ULL | 128ULL | 256ULL))
        return -E_INVAL;
    if (op != 0 && op != 1 && op != 3 && op != 4 && op != 5 && op != 9 && op != 10)
        return -E_NOTSUP;
    if ((r->rsi & 256) && op != 9)
        return -E_NOTSUP;

    uint64_t address = r->rdi;
    if (address & 3)
        return -E_INVAL;
    if (!user_access(&t->space, address, 4, false))
        return -E_FAULT;

    uint32_t mask = op == 9 || op == 10 ? (uint32_t)r->r9 : UINT32_MAX;
    if (!mask || ((op == 1 || op == 3 || op == 4 || op == 5 || op == 10) && (int32_t)r->rdx < 0))
        return -E_INVAL;

    uint64_t deadline = UINT64_MAX;
    if ((op == 0 || op == 9) && r->r10) {
        struct os_timespec timeout;
        if (!user_copy_in(&t->space, &timeout, r->r10, sizeof(timeout)))
            return -E_FAULT;
        if (timeout.sec < 0 || timeout.nsec < 0 || timeout.nsec >= 1000000000 ||
            (uint64_t)timeout.sec > (UINT64_MAX - (timeout.nsec + 999) / 1000) / 1000000)
            return -E_INVAL;
        uint64_t duration = (uint64_t)timeout.sec * 1000000 + (timeout.nsec + 999) / 1000;
        uint64_t now = tsc_uptime_us();
        if (!op)
            deadline = now + min(duration, UINT64_MAX - now);
        else if (r->rsi & 256) {
            extern int64_t kernel_boot_epoch;
            if (!kernel_boot_epoch)
                return -E_NOTSUP;
            uint64_t epoch = (uint64_t)kernel_boot_epoch * 1000000;
            deadline = duration > epoch ? duration - epoch : 0;
        } else
            deadline = duration;
    }

    if ((op == 3 || op == 4 || op == 5) && ((r->r8 & 3) || (int32_t)r->r10 < 0))
        return -E_INVAL;
    if ((op == 3 || op == 4 || op == 5) && !user_access(&t->space, r->r8, 4, op==5))
        return -E_FAULT;

    uint32_t encoded=r->r9,atomic_op=(encoded>>28)&7,comparison=(encoded>>24)&15;
    int32_t operand=(int32_t)(encoded<<8)>>20,compare=(int32_t)(encoded<<20)>>20;
    if(op==5 && (atomic_op>4 || comparison>5))return -E_NOSYS;

    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    uint32_t value;
    if (!user_copy_in(&t->space, &value, address, 4)) {
        spin_unlock_irqrestore(&scheduler_lock, flags);
        return -E_FAULT;
    }

    if(op==5) {
        uint32_t old,argument=encoded&0x80000000u?1u<<((uint32_t)operand&31):(uint32_t)operand;
        if(!user_futex_atomic32(&t->space,r->r8,atomic_op,argument,&old)) {spin_unlock_irqrestore(&scheduler_lock,flags);return -E_FAULT;}
        int32_t signed_old=(int32_t)old;
        bool matches=comparison==0?signed_old==compare:comparison==1?signed_old!=compare:
                     comparison==2?signed_old<compare:comparison==3?signed_old<=compare:
                     comparison==4?signed_old>compare:signed_old>=compare;
        long count=futex_wake_locked(t->space.root,address,(uint32_t)r->rdx,UINT32_MAX);
        if(matches)count+=futex_wake_locked(t->space.root,r->r8,(uint32_t)r->r10,UINT32_MAX);
        spin_unlock_irqrestore(&scheduler_lock,flags);return count;
    }

    if (op == 1 || op == 10 || op == 3 || op == 4) {
        if (op == 4 && value != (uint32_t)r->r9) {
            spin_unlock_irqrestore(&scheduler_lock, flags);
            return -E_AGAIN;
        }
        long count = futex_wake_locked(t->space.root, address, (uint32_t)r->rdx, mask);
        if (op == 3 || op == 4) {
            unsigned moved = 0;
            for (unsigned i = 0; i < TASK_MAX && moved < (uint32_t)r->r10; i++) {
                struct task *waiter = &tasks[i];
                if (waiter->futex_waiting && waiter->futex_root == t->space.root &&
                    waiter->futex_address == address) {
                    waiter->futex_address = r->r8;
                    moved++;
                }
            }
            count += moved;
        }
        spin_unlock_irqrestore(&scheduler_lock, flags);
        return count;
    }

    if (value != (uint32_t)r->rdx) {
        spin_unlock_irqrestore(&scheduler_lock, flags);
        return -E_AGAIN;
    }
    if (task_pending_signal(t) || signal_pending(t)) {
        spin_unlock_irqrestore(&scheduler_lock, flags);
        return -E_INTR;
    }
    if (deadline <= tsc_uptime_us()) {
        spin_unlock_irqrestore(&scheduler_lock, flags);
        return -E_TIMEDOUT;
    }

    t->futex_root = t->space.root;
    t->futex_address = address;
    t->futex_mask = mask;
    t->futex_result = 0;
    t->futex_waiting = true;
    t->wake_at = deadline;
    t->state = TASK_SLEEPING;

    /* Compare, enqueue, and transition are atomic with WAKE. Keep interrupts
       disabled until the trap saved this kernel stack on the wait queue. */
    __atomic_store_n(&scheduler_lock.value, 0, __ATOMIC_RELEASE);
    __asm__ volatile("int $0xf0" ::: "memory");
    irq_restore(flags);
    return t->futex_result;
}

void task_cpu_usage(struct task *task, bool children, uint64_t *user_us, uint64_t *system_us) {
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    *user_us = (children ? task->context->child_user_ticks : task->context->user_ticks) * 1000;
    *system_us =
        (children ? task->context->child_system_ticks : task->context->system_ticks) * 1000;
    spin_unlock_irqrestore(&scheduler_lock, flags);
}

void task_thread_cpu_usage(struct task *task, uint64_t *user_us, uint64_t *system_us) {
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    *user_us = task->user_ticks * 1000;
    *system_us = task->system_ticks * 1000;
    spin_unlock_irqrestore(&scheduler_lock, flags);
}

static int wait_usage(int pid, int *status, unsigned options, struct os_rusage *usage, unsigned *child_sid,unsigned *view_pid,unsigned *view_uid) {
    /* WUNTRACED/WCONTINUED consume live-state notifications without reaping. */
    if (options & ~(1u | 2u | 8u))
        return -E_INVAL;
    struct task *owner = this_cpu()->current;
    unsigned parent = owner->tgid;
    unsigned wanted_group=pid==0?owner->context->pgid:0;
    if(pid>0){pid=pidns_resolve(owner,pid);if(!pid)return -E_CHILD;}
    else if(pid<-1 && pid!=(-2147483647-1)){wanted_group=pidns_resolve(owner,(unsigned)-pid);if(!wanted_group)return -E_CHILD;}
    if (pid == (-2147483647 - 1))
        return -E_INVAL;
    for (;;) {
        power_transaction_begin();
        uint64_t f = spin_lock_irqsave(&scheduler_lock);
        bool found = false;
        struct task *dead = NULL;
        struct task *event=NULL;int event_status=0;
        for (unsigned i = 0; i < TASK_MAX; i++) {
            struct task *t = &tasks[i];
            if (t->state == TASK_UNUSED || t->state == TASK_NEW || t->pid != t->tgid ||
                t->ppid != parent || (wanted_group && t->context->pgid != wanted_group) ||
                (pid > 0 && t->pid != (unsigned)pid))
                continue;
            found = true;
            if (t->state == TASK_ZOMBIE && !t->context->live && cpus[t->cpu].current != t) {
                dead = t;
                break;
            }
            if(!event && ((options&2)&&t->context->stop_event)) {event=t;event_status=(t->context->stop_signal<<8)|0x7f;}
            else if(!event && ((options&8)&&t->context->continue_event)){event=t;event_status=0xffff;}
        }
        if(!dead&&event) {
            if(event_status==0xffff)event->context->continue_event=false;
            else event->context->stop_event=false;
            *status=event_status;if(child_sid)*child_sid=event->context->sid;
            if(usage)memset(usage,0,sizeof(*usage));
            int result=event->pid;if(view_pid)*view_pid=pidns_view(owner,event->pid);
            spin_unlock_irqrestore(&scheduler_lock,f);power_transaction_end();return result;
        }
        unsigned result = 0;
        if (dead) {
            result = dead->pid;if(view_pid)*view_pid=pidns_view(owner,dead->pid);
            /* Snapshot before task_discard releases the child context/ID.
               Eligibility stays canonical PPID==caller's thread group. */
            if(view_uid)*view_uid=userns_uid_view(owner,credentials_uid(dead));
            if(child_sid)*child_sid=dead->context->sid;
            *status = dead->context->exiting ? dead->context->exit_status : dead->exit_status;
            struct task *owner = this_cpu()->current;
            uint64_t user = dead->context->user_ticks + dead->context->child_user_ticks,
                     system = dead->context->system_ticks + dead->context->child_system_ticks;
            owner->context->child_user_ticks += user;
            owner->context->child_system_ticks += system;
            uint64_t peak = max(dead->space.vm->peak_pages, dead->context->child_peak_pages);
            owner->context->child_peak_pages = max(owner->context->child_peak_pages, peak);
            if (usage) {
                memset(usage, 0, sizeof(*usage));
                usage->user = (struct os_timeval){user / 1000, (user % 1000) * 1000};
                usage->system = (struct os_timeval){system / 1000, (system % 1000) * 1000};
                usage->other[0] = peak * (PAGE_SIZE / 1024);
            }
            dead->state = TASK_NEW; /* reserve slot while releasing resources */
        }
        spin_unlock_irqrestore(&scheduler_lock, f);
        if (dead) {
            task_discard(dead);
            power_transaction_end();
            return result;
        }
        power_transaction_end();
        if (!found)
            return -E_CHILD;
        if (options & 1)
            return 0;
        unsigned signal = task_pending_signal(this_cpu()->current);
        if (signal)
            task_exit(signal);
        if (signal_pending(this_cpu()->current))
            return -E_INTR;
        sched_sleep(1000);
    }
}

int task_wait_usage(int pid,int *status,unsigned options,struct os_rusage *usage,unsigned *child_sid,unsigned *view_pid) {
    return wait_usage(pid,status,options,usage,child_sid,view_pid,NULL);
}
int task_wait_exit(int *status,struct os_rusage *usage,unsigned *view_pid,unsigned *view_uid,unsigned *child_sid,bool nonblock) {
    /* P_ALL/WEXITED only: never consumes wait4 stop/continue notifications. */
    return wait_usage(-1,status,nonblock?1u:0u,usage,child_sid,view_pid,view_uid);
}
int task_wait(int pid, int *status, unsigned options) {
    unsigned visible=0;int result=task_wait_usage(pid,status,options,NULL,NULL,&visible);return result>0?(int)visible:result;
}

static int task_signal(unsigned group, unsigned tid, unsigned signal) {
    if (signal > 64)
        return -E_INVAL;
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    struct task *sender = this_cpu()->current;
    bool restricted = sender->context->web_role;
    int result = restricted ? -E_PERM : -E_SRCH;
    for (unsigned i = 0; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];
        if ((!tid || t->pid == tid) && (!group || t->tgid == group) && t->state >= TASK_RUNNABLE &&
            t->state < TASK_ZOMBIE && t->user) {
            /* tkill supplies no group ID. Check the resolved task's actual
             * group under the lock so self signals retain their normal
             * semantics without allowing signals into another renderer. */
            if (restricted && t->tgid != sender->tgid &&
                !(sender->context->web_role == OS_WEB_BROKER && signal == 9 &&
                  t->context->web_role == OS_WEB_RENDERER && t->ppid == sender->tgid &&
                  t->context->web_domain == sender->context->web_domain)) {
                result = -E_PERM;
                break;
            }
            if ((t->tgid == 1 && signal) || (credentials_restricted(sender) &&
                credentials_ruid(sender)!=credentials_ruid(t) && credentials_ruid(sender)!=t->context->suid &&
                credentials_uid(sender)!=credentials_ruid(t) && credentials_uid(sender)!=t->context->suid &&
                !(signal==18 && sender->context->sid==t->context->sid)))
                result = -E_PERM;
            else {
                int authority=pidns_signal_permission(sender,t,signal);
                if(authority){result=authority;break;}
                struct task *receiver=t;
                if(!tid&&signal&&signal!=9&&signal!=19)
                    for(unsigned j=0;j<TASK_MAX;j++)
                        if(tasks[j].context==t->context&&tasks[j].state>=TASK_RUNNABLE&&tasks[j].state<TASK_ZOMBIE&&
                           !(tasks[j].signal_mask&(1ULL<<(signal-1)))){receiver=&tasks[j];break;}
                bool queued=signal && signal!=9 && signal!=19 && t->signal_actions[signal-1].handler!=1 &&
                    (t->signal_actions[signal-1].handler || (signal!=17 && signal!=18 && signal!=28));
                int recorded=queued?pidns_signal_record(sender,receiver,signal,tid?-6:0,0):0;
                if(recorded<0){result=recorded;break;} /* no stop/continue effects before reservation */
                if(signal>=19&&signal<=22) {
                    for(unsigned j=0;j<TASK_MAX;j++)if(tasks[j].context==t->context)
                        if(!pidns_signal_drop(&tasks[j],18))__atomic_fetch_and(&tasks[j].signal_pending,~(1ULL<<17),__ATOMIC_ACQ_REL);
                    t->context->continue_event=false;
                }
                if(signal==18)continue_group_locked(t);
                if (signal == 9)
                    group_exit_locked(t, signal);
                else if(signal==19)task_stop_request_locked(t,signal);
                else if(queued && !recorded)__atomic_fetch_or(&receiver->signal_pending,1ULL<<(signal-1),__ATOMIC_RELEASE);
                result = 0;
            }
            break;
        }
    }
    spin_unlock_irqrestore(&scheduler_lock, f);
    return result;
}

int task_kill(unsigned pid, unsigned signal) {
    if (!pid)
        return -E_NOTSUP;
    return task_signal(pid, 0, signal);
}


int task_kill_group(unsigned group, unsigned signal) {
    if (!group || signal > 64) return -E_INVAL;
    unsigned members[TASK_MAX], count = 0;
    uint64_t irq = spin_lock_irqsave(&scheduler_lock);
    for (unsigned i = 0; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];
        if (!t->context || !t->user || t->state < TASK_RUNNABLE || t->state >= TASK_ZOMBIE ||
            t->context->pgid != group || !pidns_view(this_cpu()->current,t->tgid)) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < count; j++) duplicate |= members[j] == t->tgid;
        if (!duplicate) members[count++] = t->tgid;
    }
    spin_unlock_irqrestore(&scheduler_lock, irq);
    int result = -E_SRCH;
    for (unsigned i = 0; i < count; i++) {
        int error = task_kill(members[i], signal);
        if (!error || result == -E_SRCH) result = error;
    }
    return result;
}

/* Legacy programs use PID terminal ownership; opted-in jobs use PGIDs.
   User killpg always follows the logical inherited process group instead. */
int task_signal_foreground(unsigned identity, unsigned signal) {
    bool managed=false;
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);
    for(unsigned i=0;i<TASK_MAX;i++)
        if(tasks[i].context && tasks[i].context->job_managed &&
           tasks[i].context->pgid==identity && tasks[i].state>=TASK_RUNNABLE &&
           tasks[i].state<TASK_ZOMBIE) {managed=true;break;}
    spin_unlock_irqrestore(&scheduler_lock,irq);
    return managed?task_kill_group(identity,signal):task_kill(identity,signal);
}

int task_tgkill(unsigned group, unsigned tid, unsigned signal) {
    if (!tid)
        return -E_INVAL;
    struct task *caller=this_cpu()->current;
    if(group){group=pidns_resolve(caller,group);if(!group)return -E_SRCH;}
    tid=pidns_resolve(caller,tid);return tid?task_signal(group,tid,signal):-E_SRCH;
}

/* All admitted tasks use the normal, equally weighted time-sharing policy.
   Answer against a live namespace-visible task, rather than making threading
   libraries retry and log an unsupported priority query at every mutex wait.
   This does not implement priority changes or real-time scheduling. */
long task_sched_query(struct task *caller, int pid) {
    if (pid < 0) return -E_INVAL;
    unsigned target = pid ? pidns_resolve(caller, (unsigned)pid) : caller->pid;
    if (!target) return -E_SRCH;
    long result = -E_SRCH;
    uint64_t irq = spin_lock_irqsave(&scheduler_lock);
    for (unsigned i = 0; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];
        if (t->pid != target || t->state < TASK_RUNNABLE || t->state >= TASK_ZOMBIE)
            continue;
        result = caller->browser_sandbox && t->context != caller->context ? -E_PERM : 0;
        break;
    }
    spin_unlock_irqrestore(&scheduler_lock, irq);
    return result;
}

size_t sched_info(char *out, size_t cap, bool processes) {
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    size_t used = 0;
    if (processes) {
        used += snprintf(out + used, cap - used, "PID  PPID CPU STATE       TICKS NAME\n");
        static const char *states[] = {"unused",  "new",      "runnable",
                                       "running", "sleeping", "stopped", "zombie"};
        for (unsigned i = 0; i < TASK_MAX && used < cap; i++)
            if (tasks[i].state != TASK_UNUSED) {
                struct task *t = &tasks[i];
                int n = snprintf(out + used, cap - used, "%u %u %u %s %llu %s\n", t->pid, t->ppid,
                                 t->cpu, states[t->state], (unsigned long long)t->runtime_ticks,
                                 t->name);
                used += min((size_t)max(n, 0), cap - used);
            }
    } else
        for (unsigned i = 0; i < cpu_count && used < cap; i++) {
            int n = snprintf(out + used, cap - used,
                             "cpu%u lapic=%u online=%u ticks=%llu switches=%llu\n", i,
                             cpus[i].lapic_id, cpus[i].online, (unsigned long long)cpus[i].ticks,
                             (unsigned long long)cpus[i].switches);
            used += min((size_t)max(n, 0), cap - used);
        }
    spin_unlock_irqrestore(&scheduler_lock, f);
    return min(used, cap ? cap - 1 : 0);
}

/* Caller holds vfs_mutex, which also serializes cwd changes and inheritance. */
bool sched_path_busy(const char *path) {
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    size_t n = strlen(path);
    bool busy = false;
    for (unsigned i = 0; i < TASK_MAX; i++)
        if (tasks[i].user && tasks[i].cwd && tasks[i].state != TASK_UNUSED &&
            tasks[i].state != TASK_ZOMBIE && !strncmp(tasks[i].cwd, path, n) &&
            (!tasks[i].cwd[n] || tasks[i].cwd[n] == '/')) {
            busy = true;
            break;
        }
    spin_unlock_irqrestore(&scheduler_lock, f);
    return busy;
}

static void idle_loop(void) {
    for (;;)
        __asm__ volatile("sti; hlt" ::: "memory");
}

static void thread_reaper(void *unused) {
    (void)unused;
    for (;;) {
        task_reap_threads();
        pidns_reap_deferred();
        sched_sleep(100000);
    }
}

static void ap_entry(struct boot_cpu *cpu, uintptr_t argument) {
    __asm__ volatile("cli" ::: "memory");
    struct cpu_local *c = &cpus[argument];
    (void)cpu;
    uint64_t root = kernel_cr3();
    __asm__ volatile("mov %0,%%cr3" ::"r"(root) : "memory");
    gdt_init_cpu(c->id, c->kernel_stack);
    idt_load();
    setup_cpu(c);
    apic_init_cpu();
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    while (!sched_ready())
        __asm__ volatile("pause" ::: "memory");
    apic_timer_start();
    idle_loop();
}

void sched_init(struct boot_smp *smp) {
    kernel_cr3();
    user_cpu_init();
    uint32_t mxcsr = 0x1f80;
    __asm__ volatile("ldmxcsr %1; fxsave64 %0" : "=m"(initial_fpu) : "m"(mxcsr));

    /* New processes receive no register contents left by firmware. */
    memset(initial_fpu + 32, 0, sizeof(initial_fpu) - 32);

    if (!smp)
        smp = boot_smp_native_init();

    cpu_count = 1;
    uintptr_t stack;
    __asm__ volatile("mov %%rsp,%0" : "=r"(stack));
    cpus[0].id = 0;
    cpus[0].lapic_id = smp->bsp_lapic_id;
    cpus[0].kernel_stack = stack;
    setup_cpu(&cpus[0]);
    cpus[0].online = true;

    if (smp->start) {
        for (uint64_t i = 0; i < smp->cpu_count && cpu_count < MAX_CPUS; i++) {
            struct boot_cpu *cpu = &smp->cpus[i];
            if (cpu->lapic_id == smp->bsp_lapic_id)
                continue;
            unsigned c = cpu_count;
            cpus[c].id = c;
            cpus[c].lapic_id = cpu->lapic_id;
            uintptr_t physical = pmm_alloc_pages(TASK_STACK_PAGES);
            if (!physical)
                panic("CPU %u stack allocation failed", c);
            cpus[c].kernel_stack = (uintptr_t)phys_ptr(physical) + TASK_STACK_PAGES * PAGE_SIZE;
            if (!smp->start(smp, cpu, ap_entry, c, cpus[c].kernel_stack))
                panic("CPU %u could not be started", c);
            /* Native APs share a low trampoline handoff. Wait for this CPU to
             * leave it before publishing the next CPU's stack and callback. */
            uint64_t start = tsc_uptime_us();
            while (!__atomic_load_n(&cpus[c].online, __ATOMIC_ACQUIRE)) {
                if (tsc_uptime_us() - start > 5000000)
                    panic("CPU %u startup timed out", c);
                __asm__ volatile("pause" ::: "memory");
            }
            cpu_count++;
        }
    }
    system_num_cpus = cpu_count;
    is_smp_enabled = cpu_count > 1;
    pr_info("sched: %u CPU(s) online, per-CPU round robin, 1 ms preemption\n", cpu_count);
}

void sched_start(void) {
    if (task_kernel("thread-reaper", thread_reaper, NULL, 0) < 0)
        panic("thread reaper allocation failed");
    __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
    apic_timer_start();
    sched_yield();
    idle_loop();
}

/* Same scheduler lock as publication: stopped/vfork peers remain live users. */
bool credentials_private_transition(struct task *t) {
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);
    bool ok=t->pid==t->tgid && t->context->references==1 && !t->vfork_parent && t->space.vm;
    for(unsigned i=0;i<TASK_MAX && ok;i++)if(&tasks[i]!=t && tasks[i].user &&
        tasks[i].state>=TASK_RUNNABLE && tasks[i].state<TASK_ZOMBIE && tasks[i].space.vm==t->space.vm)ok=false;
    spin_unlock_irqrestore(&scheduler_lock,irq);return ok;
}
/* C2-only private fork barrier. Acquire VFS first, then freeze same-CPU
 * siblings without locks/park obligations; no cross-CPU shared-VM freeze. */
static bool fork_peer_safe(const struct task *t,const struct task *other,bool *context_wait) {
    *context_wait=__atomic_load_n(&other->waiting_mutex,__ATOMIC_ACQUIRE)==&t->context->syscall_lock && other->no_park_depth==1;
    return other->context==t->context && other->cpu==t->cpu && !other->held_mutexes &&
        (!other->no_park_depth || *context_wait);
}
static bool fork_freeze(struct task *t,bool native_spawn) {
    if(native_spawn) {
        if(!CONFIG_LARGE_BUILDS || !t || !t->user || !t->space.vm ||
           t->space.vm->budget || t->browser_sandbox || t->context->web_role)return false;
    } else if(!userns_enabled(t))return false;
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);
    bool ok=!t->vfork_parent && !t->context->exiting && !t->context->fork_owner;
    unsigned context_waiters=0,bad_pid=0,bad_held=0,bad_park=0;
    for(unsigned i=0;i<TASK_MAX && ok;i++) {
        struct task *other=&tasks[i];
        if(other==t || !other->user || other->state<TASK_RUNNABLE || other->state>=TASK_ZOMBIE || other->space.vm!=t->space.vm)continue;
        /* A sibling blocked before acquiring our context lock owns no lock.
           Its one dispatch transaction pin cannot complete until we release
           that lock, so freezing this known waiter is safe and necessary.
           Other pinned/locked obligations remain rejected. */
        bool context_wait;
        if(!fork_peer_safe(t,other,&context_wait)) {
            bad_pid=other->pid;bad_held=other->held_mutexes;bad_park=other->no_park_depth;ok=false;
        } else if(context_wait)context_waiters++;
    }
    if(ok)t->context->fork_owner=t->pid;
    spin_unlock_irqrestore(&scheduler_lock,irq);
    if(t->space.vm->budget && (!ok || context_waiters))
        pr_info("C2_FORK_BARRIER ok=%u context_waiters=%u rejected_pid=%u held=%u pin=%u\n",ok,context_waiters,bad_pid,bad_held,bad_park);
    return ok;
}
bool userns_single_live(struct task *t) {
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);
    bool ok=userns_enabled(t) && t->pid==t->tgid && t->context->live==1 &&
        !t->vfork_parent && !t->context->exiting && t->space.vm;
    for(unsigned i=0;i<TASK_MAX && ok;i++)if(&tasks[i]!=t && tasks[i].user &&
        tasks[i].state>=TASK_RUNNABLE && tasks[i].state<TASK_ZOMBIE && tasks[i].space.vm==t->space.vm)ok=false;
    spin_unlock_irqrestore(&scheduler_lock,irq);return ok;
}
bool userns_fork_freeze(struct task *t) {return fork_freeze(t,false);}
bool task_native_spawn_freeze(struct task *t) {return fork_freeze(t,true);}
void userns_fork_thaw(struct task *t) {
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);
    kassert(t->context->fork_owner==t->pid);t->context->fork_owner=0;
    spin_unlock_irqrestore(&scheduler_lock,irq);
}
#if CONFIG_PRIVATE_FORK_VERIFY
void private_fork_barrier_selftest(void) {
    struct task *a=kmallocz(sizeof(*a)),*b=kmallocz(sizeof(*b));
    struct task_context *c=kmallocz(sizeof(*c)),*d=kmallocz(sizeof(*d));kassert(a && b && c && d);
    a->context=c;a->cpu=0;b->context=c;b->cpu=0;bool waiter;
    kassert(fork_peer_safe(a,b,&waiter) && !waiter);
    b->cpu=1;kassert(!fork_peer_safe(a,b,&waiter));b->cpu=0;
    b->context=d;kassert(!fork_peer_safe(a,b,&waiter));b->context=c;
    b->held_mutexes=1;kassert(!fork_peer_safe(a,b,&waiter));b->held_mutexes=0;
    b->no_park_depth=1;kassert(!fork_peer_safe(a,b,&waiter));
    b->waiting_mutex=&c->syscall_lock;kassert(fork_peer_safe(a,b,&waiter) && waiter);
    b->no_park_depth=2;kassert(!fork_peer_safe(a,b,&waiter));b->no_park_depth=1;
    b->held_mutexes=1;kassert(!fork_peer_safe(a,b,&waiter));b->held_mutexes=0;
    b->waiting_mutex=&d->syscall_lock;kassert(!fork_peer_safe(a,b,&waiter));
    kfree(a);kfree(b);kfree(c);kfree(d);
    pr_info("G2_BARRIER_SELFTEST_PASS unchanged_same_context_CPU_lock_pin_predicate synthetic_records_not_published\n");
}
#endif

/* Local logout ends every terminal descendant, including reparented children
 * and elevated commands. The physical login console and trusted brokers are
 * separate contexts and remain available for authenticated recovery. */
void task_account_revoke(unsigned uid) {
    uint64_t irq=spin_lock_irqsave(&scheduler_lock);
    for(unsigned i=0;i<TASK_MAX;i++) {
        struct task *t=&tasks[i];
        if(t->user && t->context && t->context->account_owner==uid &&
           t->tgid!=1 && t->state>=TASK_RUNNABLE && t->state<TASK_ZOMBIE)
            group_exit_locked(t,9);
    }
    spin_unlock_irqrestore(&scheduler_lock,irq);
}
