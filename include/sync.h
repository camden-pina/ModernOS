#ifndef KERNEL_SYNC_H
#define KERNEL_SYNC_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    unsigned value;
} spinlock_t;

typedef struct {
    unsigned value;
} mutex_t;

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f)::"memory");
    return f;
}

static inline void irq_restore(uint64_t f) {
    if (f & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

static inline uint64_t spin_lock_irqsave(spinlock_t *l) {
    uint64_t f = irq_save();
    while (__atomic_exchange_n(&l->value, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
    return f;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) {
    __atomic_store_n(&l->value, 0, __ATOMIC_RELEASE);
    irq_restore(f);
}

void mutex_lock(mutex_t *l);
void mutex_unlock(mutex_t *l);
#endif
