#include <sync.h>
#include <sched.h>

void mutex_lock(mutex_t *l) {
    while (__atomic_exchange_n(&l->value, 1, __ATOMIC_ACQUIRE)) {
        if (sched_ready())
            sched_sleep(1000);
        else
            __asm__ volatile("pause" ::: "memory");
    }
}

void mutex_unlock(mutex_t *l) {
    __atomic_store_n(&l->value, 0, __ATOMIC_RELEASE);
}
