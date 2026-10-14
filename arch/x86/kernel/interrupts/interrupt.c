#include <descriptor_tables/isr.h>
#include <interrupts/lapic.h>
#include <sched.h>
#include <panic.h>
#include <printk.h>

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
            unsigned signal = 4;
            if (n == 0 || n == 16 || n == 19)
                signal = 8; /* SIGFPE */
            else if (n == 1 || n == 3)
                signal = 5; /* SIGTRAP */
            else if (n == 11 || n == 12 || n == 17)
                signal = 7; /* SIGBUS */
            else if (n == 4 || n == 5 || n == 10 || n == 13 || n == 14)
                signal = 11; /* SIGSEGV */
            uint64_t address = n == 14 ? cr2 : r->rip;
            int code = n == 0    ? 1
                       : n == 6  ? 2
                       : n == 14 ? (user_mapped(&task->space, cr2) ? 2 : 1)
                                 : 128;
            if (signal_fault(task, r, signal, code, address))
                return r;
            pr_err("process %u: exception %u error=%llx rip=%llx address=%llx name=%s\n", task->pid,
                   n, r->errorcode, r->rip, (unsigned long long)address, task->name);
            task_exit_group(task_pending_signal(task) ? task_pending_signal(task) : signal);
        }
        panic("exception %u error=%llx rip=%llx address=%llx", n, r->errorcode, r->rip,
              (unsigned long long)cr2);
    }
    apic_send_eoi_if_necessary(n);
    if (n == 0xe0 || n == 0xf0)
        return sched_interrupt(r);
    if (n < 256 && handlers[n])
        handlers[n]();
    return r;
}
