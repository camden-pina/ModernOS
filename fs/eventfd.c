#include <fs/eventfd.h>
#include <mm/pmm.h>
#include <sched.h>
#include <string.h>

struct event_counter {
    spinlock_t lock;
    uint64_t value;
    bool semaphore;
};

struct event_counter *eventfd_allocate(uint32_t initial, bool semaphore) {
    struct event_counter *counter = kmalloc(sizeof(*counter));
    if (!counter)
        return NULL;
    memset(counter, 0, sizeof(*counter));
    counter->value = initial;
    counter->semaphore = semaphore;
    return counter;
}

void eventfd_release(struct event_counter *counter) {
    kfree(counter);
}

/* Descriptor duplication and fork share the same VFS file description. The
 * last reference closes it. Blocking transfers release this lock and never
 * hold the VFS lock, just like the existing pipe and PTY paths. */
long eventfd_transfer(struct file *file, void *data, size_t size, bool write) {
    if (size < sizeof(uint64_t) || (write && size != sizeof(uint64_t)))
        return -E_INVAL;
    uint64_t value = 0;
    if (write) {
        memcpy(&value, data, sizeof(value));
        if (value == UINT64_MAX)
            return -E_INVAL;
    }
    struct event_counter *counter = file->event;
    for (;;) {
        struct task *task = this_cpu()->current;
        unsigned signal = task_pending_signal(task);
        if (signal)
            task_exit(signal);
        if (signal_pending(task))
            return -E_INTR;
        uint64_t irq = spin_lock_irqsave(&counter->lock);
        if (write ? value <= UINT64_MAX - 1 - counter->value : counter->value != 0) {
            if (write)
                counter->value += value;
            else {
                value = counter->semaphore ? 1 : counter->value;
                counter->value -= value;
                memcpy(data, &value, sizeof(value));
            }
            spin_unlock_irqrestore(&counter->lock, irq);
            return sizeof(value);
        }
        spin_unlock_irqrestore(&counter->lock, irq);
        if (file->flags & O_NONBLOCK)
            return -E_AGAIN;
        sched_sleep(1000);
    }
}

unsigned eventfd_poll(struct file *file, unsigned events) {
    struct event_counter *counter = file->event;
    uint64_t irq = spin_lock_irqsave(&counter->lock);
    unsigned ready = (counter->value ? events & POLLIN : 0) |
                     (counter->value < UINT64_MAX - 1 ? events & POLLOUT : 0);
    spin_unlock_irqrestore(&counter->lock, irq);
    return ready;
}
