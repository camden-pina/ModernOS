#!/usr/bin/env python3
"""Exercise the actual ACPICA OSL with mocked privileged/kernel boundaries.

Host threads test real OSL synchronization and work queues. IRQ save/restore,
CPU-local storage, scheduler task publication and port I/O are replaced with
host-safe equivalents. The IRQ regression also replaces explicit MSR and
I/O APIC register boundaries and borrows a fake MMIO mapping. It preserves
the real OSL install/remove logic and models a delayed generic-dispatch EOI.
This does not verify page tables, APIC hardware or AML;
those require guest/hardware tests.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

KERNEL = r'''
#ifndef TEST_KERNEL_H
#define TEST_KERNEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <boot/info.h>
#define PAGE_SIZE 4096
struct task { void (*entry)(void *); void *argument; unsigned pid, power_role; bool idle; };
struct cpu_local { struct task *current; unsigned id; };
struct cpu_local *this_cpu(void);
bool sched_ready(void);
void sched_sleep(uint64_t);
struct task *task_allocate(const char *, bool, unsigned, int);
void task_discard(struct task *);
void task_publish(struct task *);
void *kmalloc(size_t);
void *kmallocz(size_t);
void kfree(void *);
void *phys_ptr(uint64_t);
uint64_t kernel_cr3(void);
bool vmem_translate(uint64_t, uint64_t *);
bool vmem_map_page_in_root(uint64_t, uint64_t, uint64_t);
void *vmem_map_mmio(uint64_t, size_t);
uint64_t tsc_uptime_us(void);
uint8_t inb(uint16_t);
uint16_t inw(uint16_t);
uint32_t indw(uint16_t);
void outb(uint16_t, uint8_t);
void outw(uint16_t, uint16_t);
void outdw(uint16_t, uint32_t);
unsigned apic_read(int);
void register_interrupt_handler(unsigned, void (*)(void));
void panic(const char *, ...) __attribute__((noreturn));
int printk(const char *, ...);
int vprintk(const char *, va_list);
#define pr_info(...) printk(__VA_ARGS__)
#define pr_debug(...) printk(__VA_ARGS__)
#define pr_warn(...) printk(__VA_ARGS__)
typedef struct { unsigned value; } spinlock_t;
static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->value, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    __atomic_store_n(&lock->value, 0, __ATOMIC_RELEASE);
}
#endif
'''

SOURCE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "test_kernel.h"
static uint64_t test_msr_read(uint32_t index);
static uint32_t test_ioapic_read(volatile uint32_t *mapping, unsigned reg);
static void test_ioapic_write(volatile uint32_t *mapping, unsigned reg, uint32_t value);
#include "OSL_SOURCE"

static bool ready;
static _Thread_local struct task main_task = {.pid = 99};
static _Thread_local struct cpu_local local_cpu;
static pthread_t workers[5];
static struct task *worker_tasks[5];
static unsigned worker_count, next_pid = 1;
struct boot_info boot_info;
static uint32_t cf8_address = 0x12345678;
static uint8_t pci_bytes[256];
static unsigned port_writes, last_write_width;
static unsigned live_allocations;
static bool irq_test_enabled;
static uint32_t fake_ioapic_mapping[8], fake_ioapic_registers[256];
static void (*registered_irq)(void);
static unsigned table_references;
static struct {
    ACPI_TABLE_MADT madt;
    ACPI_MADT_IO_APIC io;
    ACPI_MADT_INTERRUPT_OVERRIDE override;
} fake_madt;

struct cpu_local *this_cpu(void) {
    if (!local_cpu.current)
        local_cpu.current = &main_task;
    return &local_cpu;
}
bool sched_ready(void) { return __atomic_load_n(&ready, __ATOMIC_ACQUIRE); }
void sched_sleep(uint64_t us) {
    struct timespec delay = {us / 1000000, (us % 1000000) * 1000};
    nanosleep(&delay, NULL);
}
uint64_t tsc_uptime_us(void) {
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
void *kmalloc(size_t size) {
    void *memory = malloc(size);
    if (memory)
        __atomic_add_fetch(&live_allocations, 1, __ATOMIC_RELAXED);
    return memory;
}
void *kmallocz(size_t size) {
    void *memory = calloc(1, size);
    if (memory)
        __atomic_add_fetch(&live_allocations, 1, __ATOMIC_RELAXED);
    return memory;
}
void kfree(void *memory) {
    if (memory)
        __atomic_sub_fetch(&live_allocations, 1, __ATOMIC_RELAXED);
    free(memory);
}
void *phys_ptr(uint64_t address) { (void)address; abort(); }
uint64_t kernel_cr3(void) { abort(); }
bool vmem_translate(uint64_t va, uint64_t *pa) { (void)va; (void)pa; abort(); }
bool vmem_map_page_in_root(uint64_t root, uint64_t va, uint64_t pa) {
    (void)root; (void)va; (void)pa; abort();
}
void *vmem_map_mmio(uint64_t pa, size_t size) { (void)pa; (void)size; abort(); }
unsigned apic_read(int index) {
    assert(irq_test_enabled && index == 0x20);
    return 0;
}
void register_interrupt_handler(unsigned n, void (*handler)(void)) {
    assert(irq_test_enabled && n == ACPI_SCI_VECTOR);
    if (!handler)
        assert(fake_ioapic_registers[0x10 + 20 * 2] & (1u << 16));
    __atomic_store_n(&registered_irq, handler, __ATOMIC_RELEASE);
}
void panic(const char *format, ...) { (void)format; abort(); }
int printk(const char *format, ...) { (void)format; return 0; }
int vprintk(const char *format, va_list arguments) {
    (void)format; (void)arguments; return 0;
}
ACPI_STATUS AcpiGetTable(ACPI_STRING signature, UINT32 instance, ACPI_TABLE_HEADER **out) {
    assert(instance == 1 && out);
    if (!strcmp(signature, ACPI_SIG_MCFG))
        return AE_NOT_FOUND;
    assert(irq_test_enabled && !strcmp(signature, ACPI_SIG_MADT));
    *out = &fake_madt.madt.Header;
    table_references++;
    return AE_OK;
}
void AcpiPutTable(ACPI_TABLE_HEADER *table) {
    assert(table == &fake_madt.madt.Header && table_references);
    table_references--;
}
static uint64_t test_msr_read(uint32_t index) {
    assert(irq_test_enabled && index == 0x1b);
    return 0; /* xAPIC mode, destination APIC ID zero. */
}
static uint32_t test_ioapic_read(volatile uint32_t *mapping, unsigned reg) {
    assert(irq_test_enabled && mapping == fake_ioapic_mapping && reg < 256);
    return fake_ioapic_registers[reg];
}
static void test_ioapic_write(volatile uint32_t *mapping, unsigned reg, uint32_t value) {
    assert(irq_test_enabled && mapping == fake_ioapic_mapping && reg < 256);
    if (reg >= 0x10 && !(reg & 1)) {
        /* Remote-IRR is read-only; redirection writes cannot clear it. */
        value = (value & ~(1u << 14)) | (fake_ioapic_registers[reg] & (1u << 14));
    }
    fake_ioapic_registers[reg] = value;
}
void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS where, ACPI_SIZE length) {
    if (irq_test_enabled && where == 0xfec00000 && length == 32)
        return fake_ioapic_mapping;
    return test_original_map_memory(where, length);
}
static uint32_t port_read(unsigned port, unsigned bytes) {
    if (port == 0xcf8) {
        assert(bytes == 4);
        return cf8_address;
    }
    uint32_t value = 0;
    if (port >= 0xcfc && port + bytes <= 0xd00)
        memcpy(&value, pci_bytes + (cf8_address & 0xfc) + port - 0xcfc, bytes);
    return value;
}
static void port_write(unsigned port, uint32_t value, unsigned bytes) {
    if (port == 0xcf8) {
        assert(bytes == 4);
        cf8_address = value;
    } else if (port >= 0xcfc && port + bytes <= 0xd00) {
        memcpy(pci_bytes + (cf8_address & 0xfc) + port - 0xcfc, &value, bytes);
        port_writes++;
        last_write_width = bytes;
    }
}
uint8_t inb(uint16_t p) { return port_read(p, 1); }
uint16_t inw(uint16_t p) { return port_read(p, 2); }
uint32_t indw(uint16_t p) { return port_read(p, 4); }
void outb(uint16_t p, uint8_t v) { port_write(p, v, 1); }
void outw(uint16_t p, uint16_t v) { port_write(p, v, 2); }
void outdw(uint16_t p, uint32_t v) { port_write(p, v, 4); }
struct task *task_allocate(const char *name, bool user, unsigned parent, int cpu) {
    assert(!user && !parent && !cpu && name);
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    task->pid = next_pid++;
    return task;
}
void task_discard(struct task *task) { free(task); }
static void *start_worker(void *argument) {
    struct task *task = argument;
    local_cpu.current = task;
    task->entry(task->argument);
    return NULL;
}
void task_publish(struct task *task) {
    assert(worker_count < 5);
    worker_tasks[worker_count] = task;
    assert(!pthread_create(&workers[worker_count], NULL, start_worker, task));
    worker_count++;
}

static ACPI_SPINLOCK test_spin;
static unsigned spin_counter, global_counter;
static volatile uint32_t facs_lock;
static void *contend_spin(void *argument) {
    (void)argument;
    for (unsigned i = 0; i < 10000; i++) {
        ACPI_CPU_FLAGS flags = AcpiOsAcquireLock(test_spin);
        spin_counter++;
        AcpiOsReleaseLock(test_spin, flags);
    }
    return NULL;
}
static void *contend_global(void *argument) {
    (void)argument;
    for (unsigned i = 0; i < 10000; i++) {
        while (!acpi_os_acquire_global_lock(&facs_lock))
            __asm__ volatile("pause");
        global_counter++;
        acpi_os_release_global_lock(&facs_lock);
    }
    return NULL;
}
static void contend(void *(*function)(void *)) {
    pthread_t threads[6];
    for (unsigned i = 0; i < 6; i++)
        assert(!pthread_create(&threads[i], NULL, function, NULL));
    for (unsigned i = 0; i < 6; i++)
        assert(!pthread_join(threads[i], NULL));
}
static ACPI_SEMAPHORE test_sem;
static unsigned active, peak;
static void *contend_sem(void *argument) {
    (void)argument;
    for (unsigned i = 0; i < 1000; i++) {
        assert(AcpiOsWaitSemaphore(test_sem, 1, ACPI_WAIT_FOREVER) == AE_OK);
        unsigned count = __atomic_add_fetch(&active, 1, __ATOMIC_SEQ_CST);
        assert(count <= 3);
        unsigned old = __atomic_load_n(&peak, __ATOMIC_RELAXED);
        while (old < count && !__atomic_compare_exchange_n(&peak, &old, count, false,
                                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            ;
        __atomic_sub_fetch(&active, 1, __ATOMIC_SEQ_CST);
        assert(AcpiOsSignalSemaphore(test_sem, 1) == AE_OK);
    }
    return NULL;
}
static void *wait_for_signal(void *argument) {
    (void)argument;
    assert(AcpiOsWaitSemaphore(test_sem, 1, ACPI_WAIT_FOREVER) == AE_OK);
    return NULL;
}
static unsigned callbacks, event_ran, notify_ran, slow_ran;
static unsigned fake_gpes[1024];
static void *gpe_contexts[1024];
static void gpe_count_callback(void *context) {
    assert((unsigned *)context >= fake_gpes && (unsigned *)context < fake_gpes + 1024);
    __atomic_add_fetch(&callbacks, 1, __ATOMIC_SEQ_CST);
}
static void count_callback(void *context) {
    assert(context == &callbacks);
    __atomic_add_fetch(&callbacks, 1, __ATOMIC_SEQ_CST);
}
static void event_callback(void *context) {
    assert(context == &fake_gpes[0] && AcpiOsGetThreadId() > 1);
    assert(AcpiOsWaitSemaphore(test_sem, 1, ACPI_WAIT_FOREVER) == AE_OK);
    __atomic_store_n(&event_ran, 1, __ATOMIC_RELEASE);
}
static void notify_callback(void *context) {
    assert(!context && AcpiOsGetThreadId() > 1);
    __atomic_store_n(&notify_ran, 1, __ATOMIC_RELEASE);
    assert(AcpiOsSignalSemaphore(test_sem, 1) == AE_OK);
}
static void slow_callback(void *context) {
    (void)context;
    AcpiOsSleep(15);
    __atomic_store_n(&slow_ran, 1, __ATOMIC_RELEASE);
}
static void firmware_memory_tests(void) {
    unsigned saved_count = firmware_memory_count;
    __typeof__(firmware_memory) saved;
    memcpy(saved, firmware_memory, sizeof(saved));
    firmware_memory_count = 0;
    assert(acpi_os_mark_firmware_memory(0x1003, 2));
    assert(physical_ram(0x1fff, 1) && !physical_ram(0x2000, 1));
    assert(acpi_os_mark_firmware_memory(0x3000, 1) && firmware_memory_count == 2);
    assert(acpi_os_mark_firmware_memory(0x2000, 4096) && firmware_memory_count == 1);
    assert(physical_ram(0x1000, 0x3000) && !physical_ram(0x1000, 0x3001));
    assert(acpi_os_mark_firmware_memory(0x1004, 1) && firmware_memory_count == 1);
    const uint64_t limit = UINT64_C(0x0000800000000000);
    assert(!acpi_os_mark_firmware_memory(limit - 1, 2));
    assert(acpi_os_mark_firmware_memory(limit - 1, 1));
    assert(physical_ram(limit - 4096, 4096));
    struct boot_memory_range ranges[] = {
        {.base = 0x100000, .length = 4096, .type = BOOT_MEMORY_USABLE},
        {.base = 0x101000, .length = 4096, .type = BOOT_MEMORY_ACPI_NVS},
        {.base = 0x102000, .length = 4096, .type = BOOT_MEMORY_RESERVED},
        {.base = 0x103000, .length = 4096, .type = BOOT_MEMORY_FRAMEBUFFER},
    };
    boot_info.memory_map = ranges;
    boot_info.memory_map_count = sizeof(ranges) / sizeof(ranges[0]);
    assert(physical_ram(0x100ff0, 32));
    assert(!physical_ram(0x101ff0, 32));
    assert(!physical_ram(0x103000, 1));
    boot_info.memory_map = NULL;
    boot_info.memory_map_count = 0;
    firmware_memory_count = 0;
    for (unsigned i = 0; i < 256; i++)
        assert(acpi_os_mark_firmware_memory(0x200000 + i * 8192, 4096));
    assert(firmware_memory_count == 256);
    assert(!acpi_os_mark_firmware_memory(0x800000, 4096) && firmware_memory_count == 256);
    assert(physical_ram(0x200000, 4096) && !physical_ram(0x201000, 1));
    assert(acpi_os_mark_firmware_memory(0x201000, 4096) && firmware_memory_count == 255);
    assert(physical_ram(0x200000, 3 * 4096));
    assert(acpi_os_mark_firmware_memory(0x800000, 4096) && firmware_memory_count == 256);
    memcpy(firmware_memory, saved, sizeof(saved));
    firmware_memory_count = saved_count;
}
static unsigned irq_calls, handler_entered, allow_handler_return, allow_irq_eoi, remove_complete;
static void *first_irq_context;
static UINT32 blocked_irq_handler(void *context) {
    assert(context == first_irq_context);
    __atomic_add_fetch(&irq_calls, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&handler_entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&allow_handler_return, __ATOMIC_ACQUIRE))
        sched_sleep(100);
    return ACPI_INTERRUPT_HANDLED;
}
static UINT32 next_irq_handler(void *context) {
    assert(context == &irq_calls);
    __atomic_add_fetch(&irq_calls, 1, __ATOMIC_RELAXED);
    return ACPI_INTERRUPT_HANDLED;
}
static void *deliver_irq(void *argument) {
    (void)argument;
    uint64_t flags = spin_lock_irqsave(&sci_lock);
    unsigned low = 0x10 + 20 * 2;
    assert(!(fake_ioapic_registers[low] & (1u << 16)));
    fake_ioapic_registers[low] |= 1u << 14;
    spin_unlock_irqrestore(&sci_lock, flags);
    void (*handler)(void) = __atomic_load_n(&registered_irq, __ATOMIC_ACQUIRE);
    assert(handler);
    handler();
    /* Model interrupt_dispatch's EOI after the OSL top-half has returned. */
    while (!__atomic_load_n(&allow_irq_eoi, __ATOMIC_ACQUIRE))
        sched_sleep(100);
    flags = spin_lock_irqsave(&sci_lock);
    if ((fake_ioapic_registers[low] & 255) == ACPI_SCI_VECTOR)
        fake_ioapic_registers[low] &= ~(1u << 14);
    spin_unlock_irqrestore(&sci_lock, flags);
    return NULL;
}
static void *remove_irq(void *argument) {
    (void)argument;
    assert(AcpiOsRemoveInterruptHandler(9, blocked_irq_handler) == AE_OK);
    __atomic_store_n(&remove_complete, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void await_flag(unsigned *flag) {
    uint64_t start = tsc_uptime_us();
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        assert(tsc_uptime_us() - start < 2000000);
        sched_sleep(100);
    }
}
/* Reserved finish admission must remain FIFO behind all earlier Notify work,
   and a re-trigger during the first callback's epilogue must not run twice. */
static unsigned notify_blocked, release_notify, pressure_done, finish_queued;
static unsigned generation_started, generation_active, finish_count, requeued;
static unsigned release_generation, unrelated_ran;
static void generation_callback(void *context);
static void notify_blocker(void *context) {
    assert(context == &notify_blocked);
    __atomic_store_n(&notify_blocked, 1, __ATOMIC_RELEASE);
    await_flag(&release_notify);
}
static void pressure_notify(void *context) {
    assert(context == &pressure_done);
    assert(!__atomic_load_n(&finish_count, __ATOMIC_ACQUIRE));
    __atomic_add_fetch(&pressure_done, 1, __ATOMIC_RELAXED);
}
static void finish_callback(void *context) {
    assert(context == &fake_gpes[500]);
    assert(__atomic_load_n(&pressure_done, __ATOMIC_ACQUIRE) == 256);
    unsigned generation = __atomic_add_fetch(&finish_count, 1, __ATOMIC_ACQ_REL);
    if (generation == 1) {
        /* Model finish re-enabling hardware and the same SCI re-triggering. */
        assert(AcpiOsExecute(OSL_GPE_HANDLER, generation_callback, context) == AE_OK);
        __atomic_store_n(&requeued, 1, __ATOMIC_RELEASE);
    } else {
        assert(generation == 2);
    }
}
static void generation_callback(void *context) {
    assert(context == &fake_gpes[500]);
    assert(__atomic_add_fetch(&generation_active, 1, __ATOMIC_ACQ_REL) == 1);
    unsigned generation = __atomic_add_fetch(&generation_started, 1, __ATOMIC_ACQ_REL);
    assert(generation <= 2);
    assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, finish_callback, context) == AE_OK);
    if (generation == 1) {
        __atomic_store_n(&finish_queued, 1, __ATOMIC_RELEASE);
        await_flag(&release_generation);
    }
    assert(__atomic_sub_fetch(&generation_active, 1, __ATOMIC_ACQ_REL) == 0);
}
static void unrelated_callback(void *context) {
    assert(context == &fake_gpes[501]);
    __atomic_store_n(&unrelated_ran, 1, __ATOMIC_RELEASE);
}
static void gpe_lifecycle_tests(void) {
    assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, notify_blocker, &notify_blocked) == AE_OK);
    await_flag(&notify_blocked);
    for (unsigned i = 0; i < 256; i++)
        assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, pressure_notify, &pressure_done) == AE_OK);
    assert(AcpiOsExecute(OSL_GPE_HANDLER, generation_callback, &fake_gpes[500]) == AE_OK);
    await_flag(&finish_queued);
    /* Reserved finish pending + active notify/event callbacks are not idle. */
    assert(!acpi_os_power_idle() && !acpi_os_power_freeze());
    assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, pressure_notify, &pressure_done) == AE_NO_MEMORY);
    __atomic_store_n(&release_notify, 1, __ATOMIC_RELEASE);
    await_flag(&requeued);
    assert(__atomic_load_n(&generation_started, __ATOMIC_ACQUIRE) == 1);
    assert(AcpiOsExecute(OSL_GPE_HANDLER, unrelated_callback, &fake_gpes[501]) == AE_OK);
    await_flag(&unrelated_ran);
    assert(__atomic_load_n(&generation_started, __ATOMIC_ACQUIRE) == 1);
    __atomic_store_n(&release_generation, 1, __ATOMIC_RELEASE);
    AcpiOsWaitEventsComplete();
    assert(generation_started == 2 && !generation_active && finish_count == 2);
    struct acpi_os_diagnostics measured;
    acpi_os_get_diagnostics(&measured);
    assert(measured.notification_rejections == 2 && measured.gpe_rejections == 2);
    assert(measured.event_workers == 4);
}

static unsigned dependency_started, dependency_done, signal_ran;
static void dependent_gpe(void *context) {
    assert(context >= (void *)&fake_gpes[600] && context < (void *)&fake_gpes[603]);
    __atomic_add_fetch(&dependency_started, 1, __ATOMIC_RELEASE);
    assert(AcpiOsWaitSemaphore(test_sem, 1, 1000) == AE_OK);
    __atomic_add_fetch(&dependency_done, 1, __ATOMIC_RELEASE);
}
static void signal_gpe(void *context) {
    assert(context == &fake_gpes[603]);
    assert(AcpiOsSignalSemaphore(test_sem, 3) == AE_OK);
    __atomic_store_n(&signal_ran, 1, __ATOMIC_RELEASE);
}
static unsigned boundary_started, boundary_done, boundary_marker_ran;
static void boundary_waiter(void *context) {
    assert(context >= (void *)&fake_gpes[700] && context < (void *)&fake_gpes[704]);
    __atomic_add_fetch(&boundary_started, 1, __ATOMIC_RELEASE);
    assert(AcpiOsWaitSemaphore(test_sem, 1, ACPI_WAIT_FOREVER) == AE_OK);
    __atomic_add_fetch(&boundary_done, 1, __ATOMIC_RELEASE);
}
static void boundary_marker(void *context) {
    assert(context == &fake_gpes[704]);
    __atomic_store_n(&boundary_marker_ran, 1, __ATOMIC_RELEASE);
}
static void gpe_dependency_tests(void) {
    assert(AcpiOsCreateSemaphore(3, 0, &test_sem) == AE_OK);
    for (unsigned i = 0; i < 3; i++)
        assert(AcpiOsExecute(OSL_GPE_HANDLER, dependent_gpe, &fake_gpes[600 + i]) == AE_OK);
    uint64_t start = tsc_uptime_us();
    while (__atomic_load_n(&dependency_started, __ATOMIC_ACQUIRE) != 3) {
        assert(tsc_uptime_us() - start < 500000);
        sched_sleep(100);
    }
    /* A single event worker deadlocks here. Four workers leave room for the
       GPE that Signals all three blocked AML-like callbacks. Dependencies
       occupying every worker remain an explicitly unsupported firmware case. */
    assert(AcpiOsExecute(OSL_GPE_HANDLER, signal_gpe, &fake_gpes[603]) == AE_OK);
    AcpiOsWaitEventsComplete();
    assert(dependency_done == 3 && signal_ran);
    assert(AcpiOsDeleteSemaphore(test_sem) == AE_OK);

    /* Establish the exact finite-concurrency boundary without deadlocking the
       regression: all four waiters require an external (main-thread) release.
       A fifth GPE cannot run while all event workers are occupied. */
    assert(AcpiOsCreateSemaphore(4, 0, &test_sem) == AE_OK);
    for (unsigned i = 0; i < 4; i++)
        assert(AcpiOsExecute(OSL_GPE_HANDLER, boundary_waiter, &fake_gpes[700 + i]) == AE_OK);
    start = tsc_uptime_us();
    while (__atomic_load_n(&boundary_started, __ATOMIC_ACQUIRE) != 4) {
        assert(tsc_uptime_us() - start < 500000);
        sched_sleep(100);
    }
    assert(AcpiOsExecute(OSL_GPE_HANDLER, boundary_marker, &fake_gpes[704]) == AE_OK);
    /* All four event workers blocked plus a topology-reserved dispatch. */
    assert(!acpi_os_power_idle() && !acpi_os_power_freeze());
    sched_sleep(5000);
    assert(!__atomic_load_n(&boundary_marker_ran, __ATOMIC_ACQUIRE));
    assert(AcpiOsSignalSemaphore(test_sem, 4) == AE_OK);
    AcpiOsWaitEventsComplete();
    assert(boundary_done == 4 && boundary_marker_ran);
    assert(AcpiOsDeleteSemaphore(test_sem) == AE_OK);
}

static void irq_lifetime_tests(void) {
    irq_test_enabled = true;
    memcpy(fake_madt.madt.Header.Signature, ACPI_SIG_MADT, 4);
    fake_madt.madt.Header.Length = sizeof(ACPI_TABLE_MADT) + sizeof(ACPI_MADT_IO_APIC) +
                                   sizeof(ACPI_MADT_INTERRUPT_OVERRIDE);
    fake_madt.io.Header.Type = ACPI_MADT_TYPE_IO_APIC;
    fake_madt.io.Header.Length = sizeof(ACPI_MADT_IO_APIC);
    fake_madt.io.Address = 0xfec00000;
    fake_madt.override.Header.Type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE;
    fake_madt.override.Header.Length = sizeof(ACPI_MADT_INTERRUPT_OVERRIDE);
    fake_madt.override.SourceIrq = 9;
    fake_madt.override.GlobalIrq = 20;
    fake_ioapic_registers[1] = 23u << 16;
    unsigned low = 0x10 + 20 * 2;
    /* This firmware vector differs from SCI, reproducing the old failure. */
    fake_ioapic_registers[low] = 0x61 | (1u << 16);
    fake_ioapic_registers[low + 1] = 0x01000000;
    first_irq_context = &first_irq_context;
    assert(AcpiOsInstallInterruptHandler(9, blocked_irq_handler, first_irq_context) == AE_OK);
    assert((fake_ioapic_registers[low] & 255) == ACPI_SCI_VECTOR);
    assert((fake_ioapic_registers[low] & ((1u << 13) | (1u << 15))) ==
           ((1u << 13) | (1u << 15)));
    assert(AcpiOsInstallInterruptHandler(9, blocked_irq_handler, first_irq_context) == AE_ALREADY_EXISTS);
    assert(AcpiOsRemoveInterruptHandler(8, blocked_irq_handler) == AE_BAD_PARAMETER);
    pthread_t irq, remover;
    assert(!pthread_create(&irq, NULL, deliver_irq, NULL));
    await_flag(&handler_entered);
    assert(!pthread_create(&remover, NULL, remove_irq, NULL));
    uint64_t start = tsc_uptime_us();
    while (__atomic_load_n(&registered_irq, __ATOMIC_ACQUIRE)) {
        assert(tsc_uptime_us() - start < 2000000);
        sched_sleep(100);
    }
    assert(!__atomic_load_n(&remove_complete, __ATOMIC_ACQUIRE));
    assert(AcpiOsInstallInterruptHandler(9, next_irq_handler, &irq_calls) == AE_ALREADY_EXISTS);
    __atomic_store_n(&allow_handler_return, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(remover, NULL));
    assert(__atomic_load_n(&remove_complete, __ATOMIC_ACQUIRE));
    /* Handler returned, but generic EOI is deliberately still pending. */
    assert((fake_ioapic_registers[low] & 255) == ACPI_SCI_VECTOR);
    assert(fake_ioapic_registers[low] & (1u << 16));
    assert(fake_ioapic_registers[low] & (1u << 14));
    assert(AcpiOsInstallInterruptHandler(9, next_irq_handler, &irq_calls) == AE_OK);
    __atomic_store_n(&allow_irq_eoi, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(irq, NULL));
    assert(!(fake_ioapic_registers[low] & (1u << 14)));
    assert(!pthread_create(&irq, NULL, deliver_irq, NULL));
    assert(!pthread_join(irq, NULL));
    assert(irq_calls == 2 && !(fake_ioapic_registers[low] & (1u << 14)));
    assert(AcpiOsRemoveInterruptHandler(9, next_irq_handler) == AE_OK);
    assert((fake_ioapic_registers[low] & 255) == ACPI_SCI_VECTOR);
    assert(fake_ioapic_registers[low] & (1u << 16));
    assert(!registered_irq && !table_references);
    irq_test_enabled = false;
}
int main(void) {
    assert(AcpiOsInitialize() == AE_OK);
    acpi_os_set_root_pointer(0x12345000);
    assert(AcpiOsGetRootPointer() == 0x12345000);
    assert(!acpi_os_mark_firmware_memory(UINT64_MAX, 8));
    assert(!acpi_os_mark_firmware_memory(0, 0));
    assert(acpi_os_mark_firmware_memory(0x12346000, 4096));
    assert(physical_ram(0x12345ff0, 32));
    firmware_memory_tests();
    assert(AcpiOsGetThreadId() == 1);
    assert(AcpiOsCreateLock(NULL) == AE_BAD_PARAMETER);
    assert(AcpiOsCreateLock(&test_spin) == AE_OK);
    contend(contend_spin);
    assert(spin_counter == 60000);
    AcpiOsDeleteLock(test_spin);
    facs_lock = 0x12340000;
    assert(acpi_os_acquire_global_lock(&facs_lock) && facs_lock == 0x12340002);
    assert(!acpi_os_acquire_global_lock(&facs_lock) && facs_lock == 0x12340003);
    assert(acpi_os_release_global_lock(&facs_lock) && facs_lock == 0x12340000);
    contend(contend_global);
    assert(global_counter == 60000 && !(facs_lock & 3) && (facs_lock & ~3u) == 0x12340000);

    assert(AcpiOsCreateSemaphore(0, 0, &test_sem) == AE_BAD_PARAMETER);
    assert(AcpiOsCreateSemaphore(2, 3, &test_sem) == AE_BAD_PARAMETER);
    assert(AcpiOsCreateSemaphore(5, 3, &test_sem) == AE_OK);
    assert(AcpiOsWaitSemaphore(test_sem, 0, 0) == AE_BAD_PARAMETER);
    assert(AcpiOsWaitSemaphore(test_sem, 6, 0) == AE_BAD_PARAMETER);
    assert(AcpiOsWaitSemaphore(test_sem, 2, 0) == AE_OK);
    assert(AcpiOsWaitSemaphore(test_sem, 2, 0) == AE_TIME);
    assert(AcpiOsSignalSemaphore(test_sem, 4) == AE_OK);
    assert(AcpiOsSignalSemaphore(test_sem, 1) == AE_LIMIT);
    assert(AcpiOsWaitSemaphore(test_sem, 5, 0) == AE_OK);
    uint64_t start = tsc_uptime_us();
    assert(AcpiOsWaitSemaphore(test_sem, 1, 15) == AE_TIME);
    assert(tsc_uptime_us() - start >= 15000);
    pthread_t waiter;
    assert(!pthread_create(&waiter, NULL, wait_for_signal, NULL));
    sched_sleep(10000);
    assert(AcpiOsSignalSemaphore(test_sem, 1) == AE_OK);
    assert(!pthread_join(waiter, NULL));
    assert(AcpiOsDeleteSemaphore(test_sem) == AE_OK);
    assert(AcpiOsCreateSemaphore(3, 3, &test_sem) == AE_OK);
    contend(contend_sem);
    assert(!active && peak && peak <= 3);
    assert(AcpiOsWaitSemaphore(test_sem, 3, 0) == AE_OK);
    assert(AcpiOsDeleteSemaphore(test_sem) == AE_OK);

    UINT64 value;
    UINT32 port;
    assert(AcpiOsReadMemory(0, &value, 7) == AE_BAD_PARAMETER);
    assert(AcpiOsWriteMemory(0, 0, 0) == AE_BAD_PARAMETER);
    assert(AcpiOsReadPort(65536, &port, 8) == AE_BAD_PARAMETER);
    assert(AcpiOsWritePort(65535, 0, 16) == AE_BAD_PARAMETER);
    assert(!AcpiOsMapMemory(0, 0));
    assert(!AcpiOsMapMemory(UINT64_MAX, 8));
    ACPI_PCI_ID pci = {.Segment = 0, .Bus = 0, .Device = 1, .Function = 0};
    for (unsigned i = 0; i < 256; i++)
        pci_bytes[i] = i;
    assert(AcpiOsReadPciConfiguration(&pci, 0x40, &value, 64) == AE_OK);
    assert(value == UINT64_C(0x4746454443424140) && cf8_address == 0x12345678);
    assert(AcpiOsWritePciConfiguration(&pci, 4, 0xa55a, 16) == AE_OK);
    assert(port_writes == 1 && last_write_width == 2 && pci_bytes[4] == 0x5a &&
           pci_bytes[5] == 0xa5 && pci_bytes[6] == 6 && pci_bytes[7] == 7);
    assert(AcpiOsWritePciConfiguration(&pci, 0x40, UINT64_C(0x0123456789abcdef), 64) == AE_OK);
    assert(AcpiOsReadPciConfiguration(&pci, 0x40, &value, 64) == AE_OK);
    assert(value == UINT64_C(0x0123456789abcdef) && cf8_address == 0x12345678);
    pci.Segment = 1;
    assert(AcpiOsReadPciConfiguration(&pci, 0, &value, 32) == AE_SUPPORT);
    pci.Segment = 0;
    assert(AcpiOsReadPciConfiguration(&pci, 256, &value, 32) == AE_SUPPORT);
    assert(AcpiOsReadPciConfiguration(&pci, 3, &value, 32) == AE_BAD_PARAMETER);
    irq_lifetime_tests();

    assert(AcpiOsExecute(OSL_GPE_HANDLER, NULL, NULL) == AE_BAD_PARAMETER);
    assert(AcpiOsExecute((ACPI_EXECUTE_TYPE)-1, count_callback, &callbacks) == AE_BAD_PARAMETER);
    assert(AcpiOsExecute(OSL_DEBUGGER_MAIN_THREAD, count_callback, &callbacks) == AE_SUPPORT);
    assert(AcpiOsExecute(OSL_GPE_HANDLER, gpe_count_callback, &fake_gpes[0]) == AE_SUPPORT);
    for (unsigned i = 0; i < 1024; i++)
        gpe_contexts[i] = &fake_gpes[i];
    assert(acpi_os_reserve_gpes(gpe_contexts, 1024));
    assert(!acpi_os_reserve_gpes(gpe_contexts, 1024));
    for (unsigned queue = 0; queue < 2; queue++) {
        ACPI_EXECUTE_TYPE type = queue ? OSL_NOTIFY_HANDLER : OSL_GLOBAL_LOCK_HANDLER;
        for (unsigned i = 0; i < ACPI_WORK_CAPACITY; i++)
            assert(AcpiOsExecute(type, count_callback, &callbacks) == AE_OK);
        assert(AcpiOsExecute(type, count_callback, &callbacks) == AE_NO_MEMORY);
    }
    /* Admission is topology-sized, even when both general rings are full. */
    for (unsigned i = 0; i < 1024; i++)
        assert(AcpiOsExecute(OSL_GPE_HANDLER, gpe_count_callback, &fake_gpes[i]) == AE_OK);
    assert(AcpiOsExecute(OSL_GPE_HANDLER, gpe_count_callback, &fake_gpes[0]) == AE_ALREADY_EXISTS);
    assert(!callbacks);
    AcpiOsWaitEventsComplete();
    assert(callbacks == 1536);
    assert(acpi_os_power_idle());
    /* Both ordinary rings are empty: reserved dispatch alone prevents close. */
    assert(AcpiOsExecute(OSL_GPE_HANDLER, gpe_count_callback, &fake_gpes[999]) == AE_OK);
    assert(!acpi_os_power_idle() && !acpi_os_power_freeze());
    AcpiOsWaitEventsComplete();
    assert(callbacks == 1537 && acpi_os_power_idle());
    struct acpi_os_diagnostics measured;
    acpi_os_get_diagnostics(&measured);
    assert(measured.gpe_capacity == 1024 && measured.event_workers == 0);
    assert(measured.event_rejections == 1 && measured.notification_rejections == 1);
    assert(measured.gpe_rejections == 2);
    assert(measured.notification_high_water == 256 && measured.event_high_water == 256);
    assert(AcpiOsCreateSemaphore(1, 0, &test_sem) == AE_OK);
    __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
    assert(acpi_os_start_workers() && acpi_os_start_workers() && worker_count == 5);
    assert(AcpiOsExecute(OSL_GPE_HANDLER, event_callback, &fake_gpes[0]) == AE_OK);
    assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, notify_callback, NULL) == AE_OK);
    assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, slow_callback, NULL) == AE_OK);
    AcpiOsWaitEventsComplete();
    assert(event_ran && notify_ran && slow_ran);
    assert(AcpiOsDeleteSemaphore(test_sem) == AE_OK);
    gpe_lifecycle_tests();
    gpe_dependency_tests();
    assert(acpi_os_power_idle() && acpi_os_power_freeze() && acpi_os_power_freeze());
    assert(AcpiOsExecute(OSL_GPE_HANDLER, gpe_count_callback, &fake_gpes[999]) == AE_NOT_EXIST);
    assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, count_callback, &callbacks) == AE_NOT_EXIST);
    acpi_os_power_thaw();
    assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, count_callback, &callbacks) == AE_OK);
    AcpiOsWaitEventsComplete();
    assert(callbacks == 1538 && acpi_os_power_idle());
    for (unsigned i = 0; i < 100; i++) {
        assert(AcpiOsExecute(OSL_GPE_HANDLER, gpe_count_callback, &fake_gpes[i]) == AE_OK);
        assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, count_callback, &callbacks) == AE_OK);
    }
    assert(AcpiOsTerminate() == AE_OK);
    assert(callbacks == 1738);
    assert(AcpiOsExecute(OSL_GPE_HANDLER, count_callback, &callbacks) == AE_NOT_EXIST);
    for (unsigned i = 0; i < worker_count; i++) {
        assert(!pthread_join(workers[i], NULL));
        free(worker_tasks[i]);
    }
    assert(AcpiOsInitialize() == AE_SUPPORT);
    /* Kernel reservations have boot lifetime. Host joins make freeing safe. */
    kfree(gpe_slots);
    assert(!live_allocations);
    puts("ACPICA OSL: locks/semaphores, PCI, SCI delayed EOI, 1024-GPE reserved admission, FIFO completion under Notify pressure, retrigger serialization, GPE dependency concurrency, coherent power freeze/thaw and queue shutdown passed");
    return 0;
}
'''


def replace_hardware_body(source, signature, statement):
    """Replace one explicit privileged boundary; retain all OSL policy code."""
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    body = source.index("{", start)
    end = source.index("\n}\n", body)
    return source[:body + 1] + "\n    " + statement + source[end:]


def main():
    with tempfile.TemporaryDirectory(prefix="coast-acpi-osl-") as temporary:
        folder = Path(temporary)
        include = folder / "include"
        include.mkdir()
        (include / "test_kernel.h").write_text(KERNEL)
        for name in ("descriptor_tables/isr.h", "interrupts/lapic.h", "io.h",
                     "mm/pmm.h", "mm/user.h", "mm/vmem_io.h", "panic.h",
                     "printk.h", "sync.h", "tsc.h"):
            header = include / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "test_kernel.h"\n')
        (include / "sched.h").write_text('#include_next <sched.h>\n#include "test_kernel.h"\n')
        for name in ("boot/info.h", "acpi/osl.h", "power.h"):
            header = include / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text((ROOT / "include" / name).read_text())
        source = folder / "test.c"
        osl = (ROOT / "drivers/acpi/osl.c").read_text()
        osl = replace_hardware_body(osl, "static uint64_t control_register(", "(void)number; abort();")
        osl = replace_hardware_body(osl, "static bool cpu_has_pat(", "abort();")
        osl = replace_hardware_body(osl, "static uint64_t msr_read(",
                                    "return test_msr_read(index);")
        osl = replace_hardware_body(osl, "static uint32_t ioapic_read(",
                                    "return test_ioapic_read(mapping, reg);")
        osl = replace_hardware_body(osl, "static void ioapic_write(",
                                    "test_ioapic_write(mapping, reg, value);")
        signature = "void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS where, ACPI_SIZE length)"
        assert osl.count(signature) == 1
        osl = osl.replace(signature, signature.replace("AcpiOsMapMemory", "test_original_map_memory"))
        osl_source = folder / "osl-shimmed.c"
        osl_source.write_text(osl)
        source.write_text(SOURCE.replace("OSL_SOURCE", str(osl_source)))
        binary = folder / "test"
        compiler = shlex.split(os.environ.get("HOSTCC", "cc"))
        subprocess.run(compiler + ["-std=gnu11", "-O2", "-g", "-pthread", "-Wall", "-Wextra",
                                  "-Werror", "-Wno-unused-parameter", "-D__COAST__",
                                  "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                  "-I" + str(include), "-I" + str(ROOT / "lib/acpica/source/include"),
                                  str(source), "-o", str(binary)], check=True)
        environment = os.environ.copy()
        # LeakSanitizer cannot operate under some sandbox ptrace supervisors.
        # OSL allocations are counted explicitly; ASan/UBSan remain enabled.
        environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        subprocess.run([str(binary)], check=True, timeout=30, env=environment)


if __name__ == "__main__":
    main()
