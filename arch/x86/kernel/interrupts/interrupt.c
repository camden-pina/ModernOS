#include <descriptor_tables/isr.h>
#include <interrupts/lapic.h>
#include <sched.h>
#include <panic.h>
#include <printk.h>
#include <power.h>

static void (*handlers[256])(void);

void register_interrupt_handler(unsigned n, void (*handler)(void)) {
    if (n < 256)
        handlers[n] = handler;
}

register_t *interrupt_dispatch(register_t *r) {
    unsigned n = r->interrupt;
    if (n == 2)
        return r; /* NMI: IST frame, no scheduling/locking */
    if (n < 32) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        if ((r->cs & 3) && sched_ready()) {
            struct task *task = this_cpu()->current;
            if (n == 14 && !task_pending_signal(task) &&
                user_zero_fault(&task->space,cr2,r->errorcode)) return r;
            unsigned signal = 4;
            if (n == 0 || n == 16 || n == 19)
                signal = 8; /* SIGFPE */
            else if (n == 1 || n == 3)
                signal = 5; /* SIGTRAP */
            else if (n == 11 || n == 12 || n == 17)
                signal = 7; /* SIGBUS */
            else if (n == 4 || n == 5 || n == 10 || n == 13 || n == 14)
                signal = 11; /* SIGSEGV */
            bool file_eof = n == 14 && user_file_eof_fault(&task->space, cr2, r->errorcode);
            if (file_eof) signal = 7; /* permitted access to an EOF guard: SIGBUS */
            uint64_t address = n == 14 ? cr2 : r->rip;
            int code = n == 0    ? 1
                       : n == 6  ? 2
                       : n == 14 ? (user_mapped(&task->space, cr2) ? 2 : 1)
                                 : 128;
            if (file_eof) code = 2; /* BUS_ADRERR */
            /* Preserve the original fault even if a userspace crash handler
               consumes it. Bounded metadata only; signal delivery is unchanged. */
            if (signal == 11 && task->space.vm->budget) {
                static unsigned observed;
                unsigned index=__atomic_fetch_add(&observed,1,__ATOMIC_RELAXED);
                if(index<16)pr_warn("ELECTRON_USER_FAULT pid=%u tid=%u vector=%u error=%llx rip=%llx rsp=%llx address=%llx rdi=%llx rsi=%llx rax=%llx pending_signal=%u\n",
                    task->tgid,task->pid,n,r->errorcode,r->rip,r->user_rsp,(unsigned long long)address,r->rdi,r->rsi,r->rax,task_pending_signal(task));
            }
            if (signal_fault(task, r, signal, code, address))
                return r;
            pr_err("process %u: exception %u error=%llx rip=%llx address=%llx name=%s\n", task->pid,
                   n, r->errorcode, r->rip, (unsigned long long)address, task->name);
            task_exit_group(task_pending_signal(task) ? task_pending_signal(task) : signal);
        }
        panic("exception %u error=%llx rip=%llx address=%llx", n, r->errorcode, r->rip,
              (unsigned long long)cr2);
    }
    if (n == 0xe1) {
        apic_send_eoi_if_necessary(n);
        power_ap_checkpoint();
        return r;
    }
    if (n == 0xe0 || n == 0xf0) {
        apic_send_eoi_if_necessary(n);
        power_ap_checkpoint();
        return sched_interrupt(r);
    }
    struct cpu_local *cpu = sched_ready() ? this_cpu() : NULL;
    if (cpu)
        cpu->irq_depth++;
    if (n < 256 && handlers[n])
        handlers[n]();
    /* Level-triggered device sources must be cleared before EOI. */
    apic_send_eoi_if_necessary(n);
    if (cpu)
        cpu->irq_depth--;
    return r;
}
