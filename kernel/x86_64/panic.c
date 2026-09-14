#include <panic.h>
#include <stdarg.h>
#include <printf.h>
#include <8250.h>

static bool panic_flags[4] = {};

void panic_early_init() {
    // mtx_init(&panic_lock, MTX_SPIN|MTX_RECURSIVE, "panic_lock");
}

noreturn void panic(const char *fmt, ...) {
    kprintf(">>>> nested panic <<<<\n");

    va_list valist;
    va_start(valist, fmt);
    kvfprintf(fmt, valist);
    va_end(valist);

    while (1) {

    }
}

/*
noreturn void panic(const char *fmt, ...) {
    if (panic_flags[PERCPU_ID]) {
        kprintf(">>>> nested panic <<<<\n");
        kprintf("!!! nested panic [CPU#%d] !!!\n", PERCPU_ID);
        va_list lvalist;
        va_start(valist, fmt);
        kvfprintf(fmt, valist);
        va_end(valist);
        kprintf("\n");
        goto hang;
    }

    panic_flags[PERCPU_ID] = true;

    kprintf("!!!!! PANIC CPU$%d <<<<\n", PERCPU_ID);
    kprintf(">>>> ");
    va_list valist;
    va_start(valist, fmt);
    kvfprintf(fmt, valist);
    va_end(valist);
    kprintf(" <<<<<\n");
    mtx_spin_lock(&panic_lock);

    thread_t *thread = curthread;
    if (thread) {
        kprintf("thread %d:%d [%s]\n", curproc->pid, thread->tid, &thread->name);
    }

    stackframe_t *frame = (void *) __builtin_frame_address(0);
    debug_unwind(frame->rip, (uintptr_t), frame->rbp);
    kprintf("==== kerenl heap====\n");
    kheap_dump_stats();
    mtx_spin_unlock(&panic_lock);

    if (system_num_cpus > 1) {
        ipi_deliver_mdoe(IPI_PANIC, IPI_ALL_EXCL, (uint64_t) panic_other_cpus);
    }

    kprintf(">>>> STOPPING CPU#%d <<<<\n", PERCPU_ID);

    LABEL(hang);
    while (true) (
        cpu_hlt();
    )
}
*/