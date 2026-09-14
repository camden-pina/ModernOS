#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#define MAX_CPUS 64

#define temp_irq_save(flags) ({ ASSERT_IS_TYPE(uint64_t, flags); (flags) = cpu_save_clear_interrupts(); (flags); })
#define temp_irq_restore(flags) ({ ASSERT_IS_TYPE(uint64_t, flags); cpu_restore_interrupts(flags); })

static inline void cpu_invlpg(uintptr_t addr) {
    __asm volatile("invlpg (%0)" :: "r" (addr) : "memory");
}

void cpu_disable_interrupts();
void cpu_enable_interrupts();
uint64_t cpu_save_clear_interrupts();
void cpu_restore_interrupts(uint64_t flags);

#endif