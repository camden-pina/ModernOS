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

static struct cpu_local cpus[MAX_CPUS];
static struct task tasks[TASK_MAX];
static struct task_context idle_contexts[MAX_CPUS];
static unsigned cpu_count = 1, next_pid = 1, next_cpu;
static spinlock_t scheduler_lock;
static bool ready;
static unsigned zombie_threads;

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
    if (!input || (!input->tty && (!input->pty || input->pty_master)))
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
        if (found->state >= TASK_RUNNABLE && found->state < TASK_ZOMBIE)
            break;
        parent = found->ppid;
    }
    if (input->tty)
        tty_transfer_foreground(t->tgid, parent);
    if (input->pty)
        pty_foreground(input->pty, t->tgid, parent);
    spin_unlock_irqrestore(&scheduler_lock, flags);
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

struct task *task_allocate(const char *name, bool user, unsigned ppid, int affinity) {
    task_reap_threads();
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    struct task *t = NULL;
    for (unsigned i = 0; i < TASK_MAX; i++)
        if (tasks[i].state == TASK_UNUSED) {
            t = &tasks[i];
            break;
        }

    if (t) {
        memset(t, 0, sizeof(*t));
        t->state = TASK_NEW;
        t->pid = next_pid++;
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
    if (!t)
        return NULL;

    struct task_context *context = kmalloc(sizeof(*context));
    if (!context) {
        task_discard(t);
        return NULL;
    }

    memset(context, 0, sizeof(*context));
    context->references = 1;
    context->tgid = t->pid;
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
    process_cleanup(t);
    if (t->stack_phys)
        pmm_free_pages(t->stack_phys, TASK_STACK_PAGES);
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    memset(t, 0, sizeof(*t));
    spin_unlock_irqrestore(&scheduler_lock, f);
}

void task_publish(struct task *t) {
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    t->context->live++;
    if (t->context->exiting)
        __atomic_store_n(&t->kill_signal, 9, __ATOMIC_RELEASE);
    t->state = TASK_RUNNABLE;
    enqueue(t);
    spin_unlock_irqrestore(&scheduler_lock, f);
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

static void group_exit_locked(struct task *t, int status) {
    if (!t->context->exiting) {
        t->context->exiting = true;
        t->context->exit_status = status;
    }
    for (unsigned i = 0; i < TASK_MAX; i++)
        if (tasks[i].context == t->context && tasks[i].state >= TASK_RUNNABLE &&
            tasks[i].state < TASK_ZOMBIE)
            __atomic_store_n(&tasks[i].kill_signal, 9, __ATOMIC_RELEASE);
}

static void become_zombie(struct task *t, int status) {
    if (t->context->exiting)
        status = t->context->exit_status;
    if (t->clear_tid) {
        uint32_t zero = 0;
        user_copy_out(&t->space, t->clear_tid, &zero, 4);
        futex_wake_locked(t->space.root, t->clear_tid, 1, UINT32_MAX);
        t->clear_tid = 0;
    }
    t->futex_waiting = false;
    t->state = TASK_ZOMBIE;
    t->exit_status = status;
    if (t->pid != t->tgid)
        __atomic_fetch_add(&zombie_threads, 1, __ATOMIC_RELEASE);
    t->context->live--;
    __atomic_store_n(&t->vfork_parent, 0, __ATOMIC_RELEASE);
    if (t->context->live)
        return;
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
            parent->state < TASK_ZOMBIE && parent->signal_actions[16].handler > 1)
            __atomic_fetch_or(&parent->signal_pending, 1ULL << 16, __ATOMIC_RELEASE);
    }
    for (unsigned i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].ppid == t->tgid)
            tasks[i].ppid = 1;
}

register_t *sched_interrupt(register_t *frame) {
    if (!sched_ready())
        return frame;

    struct cpu_local *c = this_cpu();
    if (frame->interrupt == 0xe0 && !c->id)
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
            __atomic_fetch_or(&t->signal_pending, 1ULL << 13, __ATOMIC_RELEASE);
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
    if ((frame->cs & 3) && !task_pending_signal(old))
        signal_deliver(old, frame, false, old->signal_mask);
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

    struct task *next = c->head;
    if (next) {
        c->head = next->next;
        if (!c->head)
            c->tail = NULL;
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
            vfs_close(exiting->fds[i]);
            exiting->fds[i] = NULL;
            exiting->fd_flags[i] = 0;
        }
        mutex_unlock(&vfs_mutex);
    }
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    struct task *t = this_cpu()->current;
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
    if (op != 0 && op != 1 && op != 3 && op != 4 && op != 9 && op != 10)
        return -E_NOTSUP;
    if ((r->rsi & 256) && op != 9)
        return -E_NOTSUP;

    uint64_t address = r->rdi;
    if (address & 3)
        return -E_INVAL;
    if (!user_access(&t->space, address, 4, false))
        return -E_FAULT;

    uint32_t mask = op == 9 || op == 10 ? (uint32_t)r->r9 : UINT32_MAX;
    if (!mask || ((op == 1 || op == 3 || op == 4 || op == 10) && (int32_t)r->rdx < 0))
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

    if ((op == 3 || op == 4) && ((r->r8 & 3) || (int32_t)r->r10 < 0))
        return -E_INVAL;
    if ((op == 3 || op == 4) && !user_access(&t->space, r->r8, 4, false))
        return -E_FAULT;

    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    uint32_t value;
    if (!user_copy_in(&t->space, &value, address, 4)) {
        spin_unlock_irqrestore(&scheduler_lock, flags);
        return -E_FAULT;
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

int task_wait_usage(int pid, int *status, unsigned options, struct os_rusage *usage) {
    /* WUNTRACED/WCONTINUED are accepted; this kernel has no stopped-task state. */
    if (options & ~(1u | 2u | 8u))
        return -E_INVAL;
    unsigned parent = this_cpu()->current->tgid;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&scheduler_lock);
        bool found = false;
        struct task *dead = NULL;
        for (unsigned i = 0; i < TASK_MAX; i++) {
            struct task *t = &tasks[i];
            if (t->state == TASK_UNUSED || t->state == TASK_NEW || t->pid != t->tgid ||
                t->ppid != parent || (pid != -1 && t->pid != (unsigned)pid))
                continue;
            found = true;
            if (t->state == TASK_ZOMBIE && !t->context->live && cpus[t->cpu].current != t) {
                dead = t;
                break;
            }
        }
        unsigned result = 0;
        if (dead) {
            result = dead->pid;
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
            return result;
        }
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

int task_wait(int pid, int *status, unsigned options) {
    return task_wait_usage(pid, status, options, NULL);
}

static int task_signal(unsigned group, unsigned tid, unsigned signal) {
    if (signal > 64)
        return -E_INVAL;
    if (signal == 19 || signal == 20 || signal == 21 || signal == 22)
        return -E_NOTSUP;
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
            if (t->tgid == 1)
                result = -E_PERM;
            else {
                if (signal == 9)
                    group_exit_locked(t, signal);
                else if (signal && t->signal_actions[signal - 1].handler != 1 &&
                         (t->signal_actions[signal - 1].handler ||
                          (signal != 17 && signal != 18 && signal != 28)))
                    __atomic_fetch_or(&t->signal_pending, 1ULL << (signal - 1), __ATOMIC_RELEASE);
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

int task_tgkill(unsigned group, unsigned tid, unsigned signal) {
    if (!tid)
        return -E_INVAL;
    return task_signal(group, tid, signal);
}

size_t sched_info(char *out, size_t cap, bool processes) {
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    size_t used = 0;
    if (processes) {
        used += snprintf(out + used, cap - used, "PID  PPID CPU STATE       TICKS NAME\n");
        static const char *states[] = {"unused",  "new",      "runnable",
                                       "running", "sleeping", "zombie"};
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
