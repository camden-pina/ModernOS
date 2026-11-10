#include <sync.h>
#include <sched.h>
#include <power.h>
#include <panic.h>

void mutex_lock(mutex_t *l) {
    struct task *waiter=CONFIG_USERNS_TEST && sched_ready()?this_cpu()->current:NULL;
    mutex_t *previous=waiter?__atomic_load_n(&waiter->waiting_mutex,__ATOMIC_ACQUIRE):NULL;
    for (;;) {
        power_checkpoint();
        /* IF stays clear from exchange through ownership publication. A park
           IPI cannot observe an acquired mutex with a zero aggregate count. */
        uint64_t flags = irq_save();
        if (!__atomic_exchange_n(&l->value, 1, __ATOMIC_ACQUIRE)) {
            l->counted = sched_ready();
            l->owner = l->counted ? this_cpu()->current : NULL;
            l->owner_cpu = l->counted ? this_cpu()->id : 0;
            power_mutex_acquired();
            if(waiter)__atomic_store_n(&waiter->waiting_mutex,previous,__ATOMIC_RELEASE);
            irq_restore(flags);
            return;
        }
        if(waiter)__atomic_store_n(&waiter->waiting_mutex,l,__ATOMIC_RELEASE);
        irq_restore(flags);
        if (sched_ready())
            sched_sleep(1000);
        else
            __asm__ volatile("pause" ::: "memory");
    }
}

void mutex_unlock(mutex_t *l) {
    uint64_t flags = irq_save();
    if (l->counted && (!sched_ready() || l->owner != this_cpu()->current ||
                       l->owner_cpu != this_cpu()->id))
        panic("power: native mutex released by non-owner");
    bool counted = l->counted;
    l->owner = NULL;
    l->counted = false;
    __atomic_store_n(&l->value, 0, __ATOMIC_RELEASE);
    if (counted)
        power_mutex_released();
    /* Never park from generic spin_unlock: the scheduler carries a lock over
       its stack switch and uses raw releases. Last-mutex release is safe only
       when the entry IF was set and no nested device handler is active. */
    if (flags & 0x200)
        power_ap_checkpoint();
    irq_restore(flags);
    if (flags & 0x200)
        power_checkpoint();
}
