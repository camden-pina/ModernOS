#include <descriptor_tables/gdt.h>
#include <sched.h>
#include <string.h>

/* Linux x86-64 signal context layout, with kernel-owned saved registers.
   Keeping the authoritative return frame in the task prevents forged selectors,
   kernel addresses, or FPU control bits from crossing the privilege boundary. */
struct signal_context {
    uint64_t flags, link, stack_address;
    uint32_t stack_flags, pad;
    uint64_t stack_size, registers[23], fpstate, reserved[8], mask, mask_padding[15], fp[64];
};

struct signal_frame {
    uint64_t restorer;
    struct signal_context context;
    uint8_t info[128];
};

bool signal_pending(struct task *t) {
    /* An ignored timer signal can still have a latched bit. Discard it before
       deciding to interrupt a blocking operation, rather than spuriously
       returning EINTR or retaining a temporary wait mask. */
    uint64_t available = __atomic_load_n(&t->signal_pending, __ATOMIC_ACQUIRE) & ~t->signal_mask;
    while (available) {
        unsigned sig = __builtin_ctzll(available) + 1;
        uint64_t handler = __atomic_load_n(&t->signal_actions[sig - 1].handler, __ATOMIC_ACQUIRE);
        if (handler != 1 && (handler || (sig != 17 && sig != 18 && sig != 28)))
            return true;
        uint64_t bit = 1ULL << (sig - 1);
        __atomic_fetch_and(&t->signal_pending, ~bit, __ATOMIC_ACQ_REL);
        available &= ~bit;
    }
    return false;
}

long signal_suspend(struct task *t, uint64_t mask) {
    /* Install the mask before inspecting pending bits. The scheduler latches
       signals while this kernel context sleeps; none can run a user handler
       between that change and the wait. The dispatcher keeps the original
       mask for the handler's authoritative return frame. */
    t->signal_mask = mask & ~((1ULL << 8) | (1ULL << 18));
    for (;;) {
        unsigned killed = task_pending_signal(t);
        if (killed)
            task_exit(killed);
        if (signal_pending(t))
            return -E_INTR;
        sched_sleep(1000);
    }
}

static bool deliver(struct task *t, register_t *r, bool restart, uint64_t return_mask, unsigned sig,
                    int code, uint64_t address) {
    uint64_t bit = 1ULL << (sig - 1);
    uint64_t irq = irq_save();
    struct os_sigaction a = t->signal_actions[sig - 1];
    if (a.flags & SA_RESETHAND)
        memset(&t->signal_actions[sig - 1], 0, sizeof(a));
    irq_restore(irq);

    if (a.handler == 1 || (!a.handler && (sig == 17 || sig == 18 || sig == 28))) {
        return true;
    }
    if (!a.handler || t->signal_depth == 4 || !a.restorer) {
        __atomic_store_n(&t->kill_signal, sig, __ATOMIC_RELEASE);
        return false;
    }

    uint64_t sp = ((r->user_rsp - 128 - sizeof(struct signal_frame)) & ~15ULL) - 8;
    if (sp >= r->user_rsp || !user_access(&t->space, sp, sizeof(struct signal_frame), true) ||
        !user_access(&t->space, a.handler, 1, false) ||
        !user_access(&t->space, a.restorer, 1, false)) {
        __atomic_store_n(&t->kill_signal, 11, __ATOMIC_RELEASE);
        return false;
    }

    struct signal_frame f = {.restorer = a.restorer};
    f.context.mask = return_mask;
    uint64_t *g = f.context.registers;
    g[0] = r->r8;
    g[1] = r->r9;
    g[2] = r->r10;
    g[3] = r->r11;
    g[4] = r->r12;
    g[5] = r->r13;
    g[6] = r->r14;
    g[7] = r->r15;
    g[8] = r->rdi;
    g[9] = r->rsi;
    g[10] = r->rbp;
    g[11] = r->rbx;
    g[12] = r->rdx;
    g[13] = r->rax;
    g[14] = r->rcx;
    g[15] = r->user_rsp;
    g[16] = r->rip;
    g[17] = r->rflags;
    g[18] = USER_CS;
    g[19] = r->errorcode;
    g[20] = r->interrupt;
    g[21] = return_mask;
    g[22] = r->interrupt == 14 ? address : 0;

    ((int *)f.info)[0] = sig;
    ((int *)f.info)[2] = code;
    memcpy(f.info + 16, &address, sizeof(address));

    unsigned depth = t->signal_depth;
    t->signal_frames[depth].frame = *r;
    t->signal_frames[depth].mask = return_mask;
    t->signal_frames[depth].stack = sp;
    __asm__ volatile("fxsave64 %0" : "=m"(t->signal_frames[depth].fpu));
    memcpy(f.context.fp, t->signal_frames[depth].fpu, 512);
    f.context.fpstate = sp + offsetof(struct signal_frame, context.fp);
    if (!user_copy_out(&t->space, sp, &f, sizeof(f))) {
        __atomic_store_n(&t->kill_signal, 11, __ATOMIC_RELEASE);
        return false;
    }

    if (restart && (a.flags & SA_RESTART)) {
        t->signal_frames[depth].frame.rip -= 2;
        /* The dispatcher supplies the interrupted syscall number in RCX. */
        t->signal_frames[depth].frame.rax = r->rcx;
    }

    t->signal_depth++;
    t->signal_mask |= a.mask | ((a.flags & SA_NODEFER) ? 0 : bit);
    t->signal_mask &= ~((1ULL << 8) | (1ULL << 18));

    r->rip = a.handler;
    r->user_rsp = sp;
    r->rdi = sig;
    r->rsi = sp + offsetof(struct signal_frame, info);
    r->rdx = sp + offsetof(struct signal_frame, context);
    r->rax = 0;
    return true;
}

bool signal_deliver(struct task *t, register_t *r, bool restart, uint64_t return_mask) {
    uint64_t available = __atomic_load_n(&t->signal_pending, __ATOMIC_ACQUIRE) & ~t->signal_mask;
    if (!available)
        return true;
    unsigned sig = __builtin_ctzll(available) + 1;
    __atomic_fetch_and(&t->signal_pending, ~(1ULL << (sig - 1)), __ATOMIC_ACQ_REL);
    return deliver(t, r, restart, return_mask, sig, 0, 0);
}

bool signal_fault(struct task *t, register_t *r, unsigned sig, int code, uint64_t address) {
    /* Synchronous faults are delivered directly rather than queued behind an
       unrelated signal. Ignoring or blocking them cannot resume the faulting
       instruction indefinitely. The same checked, kernel-owned frame is used. */
    if (!sig || sig > 64 || task_pending_signal(t) || t->signal_actions[sig - 1].handler <= 1 ||
        (t->signal_mask & (1ULL << (sig - 1))))
        return false;
    return deliver(t, r, false, t->signal_mask, sig, code, address);
}

long signal_return(struct task *t, register_t *r) {
    if (!t->signal_depth) {
        return -E_INVAL;
    }
    unsigned depth = t->signal_depth - 1;
    if (r->user_rsp != t->signal_frames[depth].stack + 8) {
        return -E_INVAL;
    }
    t->signal_mask = t->signal_frames[depth].mask;
    *r = t->signal_frames[depth].frame;
    __asm__ volatile("fxrstor64 %0" ::"m"(t->signal_frames[depth].fpu));
    t->signal_depth--;
    return r->rax;
}
