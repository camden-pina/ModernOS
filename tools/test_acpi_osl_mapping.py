#!/usr/bin/env python3
"""Check the real ACPICA OSL mapping policy and PCI ECAM paths on the host.

Only CR3/CR4, CPUID PAT availability and RDMSR are replaced. Page-table walks,
RAM/MMIO classification, runtime mapping decisions and ECAM accesses are real
OSL code. Kernel VM calls are counted mocks backed by synthetic physical RAM;
this does not test the kernel VM implementation or hardware TLB/cache behavior.
The real volatile-access helpers are traced to check ECAM DWORD splitting.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "test_kernel.h"
static uint64_t test_control_register(unsigned number);
static bool test_cpu_has_pat(void);
static uint64_t test_msr_read(uint32_t index);
static uint64_t read_volatile(const volatile void *pointer, unsigned width);
static void write_volatile(volatile void *pointer, uint64_t value, unsigned width);
#include "OSL_SOURCE"

#define BACKING_SIZE (64u * 1024 * 1024)
#define TABLE_BYTES (1024u * 1024)
#define GIGA_SIZE UINT64_C(0x40000000)
static uint8_t *backing;
static uint8_t table_snapshot[TABLE_BYTES];
struct boot_info boot_info;
static struct boot_memory_range memory_ranges[8];
static bool ready, has_pat, fail_map, fail_mmio;
static uint64_t cr3, cr4, pat_value, next_table;
static unsigned map_calls, mmio_calls, port_calls, msr_calls;
static uint64_t last_mmio;
static size_t last_mmio_length;
static struct { uint64_t physical; size_t length; } mmio_requests[16];
static unsigned table_gets, table_puts, table_references;
static ACPI_STATUS table_status;
static bool poison_table_on_put;
static struct {
    ACPI_TABLE_MCFG mcfg;
    ACPI_MCFG_ALLOCATION allocation[3];
} fake_mcfg;
_Static_assert(offsetof(__typeof__(fake_mcfg), allocation) == sizeof(ACPI_TABLE_MCFG),
               "MCFG fixture must use the firmware byte layout");
static struct {
    uintptr_t address;
    uint64_t value;
    unsigned width;
    bool write;
} accesses[8];
static unsigned access_count;

static uint64_t test_control_register(unsigned number) {
    assert(number == 3 || number == 4);
    return number == 3 ? cr3 : cr4;
}
static bool test_cpu_has_pat(void) { return has_pat; }
static uint64_t test_msr_read(uint32_t index) {
    assert(index == 0x277 && has_pat);
    msr_calls++;
    return pat_value;
}
static uint64_t read_volatile(const volatile void *pointer, unsigned width) {
    assert(access_count < sizeof(accesses) / sizeof(accesses[0]));
    unsigned i = access_count++;
    accesses[i].address = (uintptr_t)pointer;
    accesses[i].width = width;
    accesses[i].write = false;
    uint64_t value = test_real_read_volatile(pointer, width);
    accesses[i].value = value;
    return value;
}
static void write_volatile(volatile void *pointer, uint64_t value, unsigned width) {
    assert(access_count < sizeof(accesses) / sizeof(accesses[0]));
    unsigned i = access_count++;
    accesses[i].address = (uintptr_t)pointer;
    accesses[i].width = width;
    accesses[i].value = value;
    accesses[i].write = true;
    test_real_write_volatile(pointer, value, width);
}

void *phys_ptr(uint64_t address) {
    assert(address < BACKING_SIZE);
    return backing + address;
}
static uint64_t allocate_table(void) {
    uint64_t address = next_table;
    next_table += PAGE_SIZE;
    assert(next_table <= TABLE_BYTES);
    memset(phys_ptr(address), 0, PAGE_SIZE);
    return address;
}
static uint64_t *entry_at(uint64_t va, unsigned level, bool create) {
    uint64_t *table = phys_ptr(cr3 & ACPI_PAGE_ADDRESS);
    for (unsigned current = 3;; current--) {
        uint64_t *entry = &table[(va >> (12 + current * 9)) & 511];
        if (current == level)
            return entry;
        if (!(*entry & 1)) {
            if (!create)
                return NULL;
            *entry = allocate_table() | 3;
        }
        assert(!(*entry & 128));
        table = phys_ptr(*entry & ACPI_PAGE_ADDRESS);
    }
}
static void map_leaf(uint64_t va, uint64_t pa, unsigned level, unsigned pat, bool write) {
    assert(level <= 2 && pat < 8);
    uint64_t mask = (UINT64_C(1) << (12 + level * 9)) - 1;
    assert(!((va | pa) & mask));
    uint64_t flags = 1 | (write ? 2 : 0) | ((pat & 3) << 3);
    if (level)
        flags |= 128 | ((uint64_t)(pat & 4) << 10);
    else
        flags |= (pat & 4) << 5;
    *entry_at(va, level, true) = pa | flags;
}
static void map_direct(uint64_t pa, unsigned level, unsigned pat, bool write) {
    map_leaf((uintptr_t)phys_ptr(pa), pa, level, pat, write);
}
static void reset_fixture(void) {
    assert(!table_references);
    ready = false;
    has_pat = true;
    fail_map = fail_mmio = false;
    memset(backing, 0, TABLE_BYTES);
    next_table = PAGE_SIZE;
    cr3 = allocate_table() | 0x18; /* CR3 cache flags must not become address bits. */
    cr4 = 0;
    pat_value = UINT64_C(0x0007040600070406); /* architectural reset PAT */
    memset(&boot_info, 0, sizeof(boot_info));
    memset(memory_ranges, 0, sizeof(memory_ranges));
    boot_info.hhdm_offset = (uintptr_t)backing;
    boot_info.memory_map = memory_ranges;
    firmware_memory_count = 0;
    map_calls = mmio_calls = port_calls = msr_calls = 0;
    last_mmio = last_mmio_length = 0;
    access_count = 0;
    table_gets = table_puts = 0;
    table_status = AE_NOT_FOUND;
    poison_table_on_put = false;
    memset(&fake_mcfg, 0, sizeof(fake_mcfg));
}
static void set_ram(uint64_t base, uint64_t length, unsigned type) {
    assert(boot_info.memory_map_count < 8);
    memory_ranges[boot_info.memory_map_count++] = (struct boot_memory_range){base, length, type};
}
static void begin_runtime(void) {
    ready = true;
    map_calls = mmio_calls = 0;
    memcpy(table_snapshot, backing, TABLE_BYTES);
}
static void unchanged_tables(void) {
    assert(!map_calls && !mmio_calls);
    assert(!memcmp(table_snapshot, backing, TABLE_BYTES));
}

uint64_t kernel_cr3(void) { return cr3 & ACPI_PAGE_ADDRESS; }
bool vmem_map_page_in_root(uint64_t root, uint64_t va, uint64_t pa) {
    assert(!ready && root == kernel_cr3() && va == (uintptr_t)phys_ptr(pa));
    map_calls++;
    if (fail_map)
        return false;
    uint64_t *entry = entry_at(va, 0, true);
    uint64_t attributes = (*entry & 1) && (*entry & ACPI_PAGE_ADDRESS) == pa ?
                          *entry & ~ACPI_PAGE_ADDRESS : 0;
    *entry = pa | attributes | 3;
    return true;
}
void *vmem_map_mmio(uint64_t pa, size_t bytes) {
    assert(!ready && !table_references);
    assert(mmio_calls < sizeof(mmio_requests) / sizeof(mmio_requests[0]));
    mmio_requests[mmio_calls].physical = pa;
    mmio_requests[mmio_calls].length = bytes;
    mmio_calls++;
    last_mmio = pa;
    last_mmio_length = bytes;
    if (fail_mmio)
        return NULL;
    /* Emulate successful VM installation with UC leaves. The real VM's
       splitting/TLB/cache machinery remains outside this host test. */
    for (uint64_t page = pa & ~UINT64_C(4095); page < pa + bytes; page += PAGE_SIZE)
        map_direct(page, 0, 3, true);
    return phys_ptr(pa);
}
bool vmem_translate(uint64_t va, uint64_t *pa) {
    bool write;
    unsigned pat;
    return page_attributes(va, pa, &write, &pat);
}
struct cpu_local *this_cpu(void) {
    static struct task task = {.pid = 1};
    static struct cpu_local cpu = {.current = &task};
    return &cpu;
}
bool sched_ready(void) { return ready; }
void sched_sleep(uint64_t us) { (void)us; abort(); }
uint64_t tsc_uptime_us(void) { static uint64_t clock; return ++clock; }
void *kmalloc(size_t bytes) { return malloc(bytes); }
void *kmallocz(size_t bytes) { return calloc(1, bytes); }
void kfree(void *pointer) { free(pointer); }
struct task *task_allocate(const char *name, bool user, unsigned parent, int cpu) {
    (void)name; (void)user; (void)parent; (void)cpu; abort();
}
void task_discard(struct task *task) { (void)task; abort(); }
void task_publish(struct task *task) { (void)task; abort(); }
unsigned apic_read(int index) { (void)index; abort(); }
void register_interrupt_handler(unsigned n, void (*handler)(void)) {
    (void)n; (void)handler; abort();
}
void panic(const char *format, ...) { (void)format; abort(); }
int printk(const char *format, ...) { (void)format; return 0; }
int vprintk(const char *format, va_list args) { (void)format; (void)args; return 0; }
uint8_t inb(uint16_t port) { (void)port; port_calls++; return 0; }
uint16_t inw(uint16_t port) { (void)port; port_calls++; return 0; }
uint32_t indw(uint16_t port) { (void)port; port_calls++; return 0; }
void outb(uint16_t port, uint8_t value) { (void)port; (void)value; port_calls++; }
void outw(uint16_t port, uint16_t value) { (void)port; (void)value; port_calls++; }
void outdw(uint16_t port, uint32_t value) { (void)port; (void)value; port_calls++; }
ACPI_STATUS AcpiGetTable(ACPI_STRING signature, UINT32 instance, ACPI_TABLE_HEADER **out) {
    assert(!strcmp(signature, ACPI_SIG_MCFG) && instance == 1 && out);
    assert(!table_references);
    table_gets++;
    if (ACPI_FAILURE(table_status))
        return table_status;
    *out = &fake_mcfg.mcfg.Header;
    table_references++;
    return AE_OK;
}
void AcpiPutTable(ACPI_TABLE_HEADER *header) {
    assert(header == &fake_mcfg.mcfg.Header && table_references == 1);
    table_puts++;
    table_references--;
    if (poison_table_on_put)
        memset(fake_mcfg.allocation, 0xff, sizeof(fake_mcfg.allocation));
}

static uint64_t mapping_rejections(void) {
    struct acpi_os_diagnostics state;
    acpi_os_get_diagnostics(&state);
    return state.mapping_rejections;
}
static void *checked_map(ACPI_PHYSICAL_ADDRESS where, ACPI_SIZE length) {
    uint64_t before = mapping_rejections();
    void *result = AcpiOsMapMemory(where, length);
    assert(mapping_rejections() == before + (result == NULL));
    return result;
}

static void attributes(uint64_t va, uint64_t expected, bool write, unsigned pat) {
    uint64_t physical = UINT64_MAX;
    bool writable = !write;
    unsigned index = 99;
    assert(page_attributes(va, &physical, &writable, &index));
    assert(physical == expected && writable == write && index == pat);
}
static void page_walk_tests(void) {
    const uint64_t pa = 0x400000;
    for (unsigned level = 0; level <= 2; level++) {
        for (unsigned pat = 0; pat < 8; pat++) {
            reset_fixture();
            uint64_t base = level == 2 ? 0 : pa;
            map_direct(base, level, pat, true);
            attributes((uintptr_t)phys_ptr(base + 0x5a3), base + 0x5a3, true, pat);
            if (level)
                attributes((uintptr_t)phys_ptr(base + 0x1f1234), base + 0x1f1234, true, pat);
        }
    }
    for (unsigned readonly_level = 0; readonly_level <= 3; readonly_level++) {
        reset_fixture();
        map_direct(pa, 0, 6, true);
        *entry_at((uintptr_t)phys_ptr(pa), readonly_level, false) &= ~UINT64_C(2);
        attributes((uintptr_t)phys_ptr(pa + 77), pa + 77, false, 6);
        assert(AcpiOsReadable(phys_ptr(pa + 77), 17));
        assert(!AcpiOsWritable(phys_ptr(pa + 77), 17));
    }
    for (unsigned missing_level = 0; missing_level <= 3; missing_level++) {
        reset_fixture();
        map_direct(pa, 0, 0, true);
        *entry_at((uintptr_t)phys_ptr(pa), missing_level, false) &= ~UINT64_C(1);
        uint64_t physical;
        bool writable;
        unsigned index;
        assert(!page_attributes((uintptr_t)phys_ptr(pa), &physical, &writable, &index));
    }
    reset_fixture();
    map_leaf(UINT64_C(0xffff800012345000), pa, 0, 5, true);
    attributes(UINT64_C(0xffff800012345678), pa + 0x678, true, 5);
    uint64_t physical;
    bool writable;
    unsigned pat;
    assert(!page_attributes(UINT64_C(0x0000800000000000), &physical, &writable, &pat));
    assert(!page_attributes(UINT64_C(0xffff7fffffffffff), &physical, &writable, &pat));
    map_direct(pa, 0, 0, true);
    cr4 = UINT64_C(1) << 12;
    assert(!page_attributes((uintptr_t)phys_ptr(pa), &physical, &writable, &pat));
    cr4 = 0;
    *entry_at((uintptr_t)phys_ptr(pa), 3, false) |= 128;
    assert(!page_attributes((uintptr_t)phys_ptr(pa), &physical, &writable, &pat));

    reset_fixture();
    map_direct(pa, 0, 0, true);
    assert(AcpiOsReadable(phys_ptr(pa + 4090), 6));
    assert(!AcpiOsReadable(phys_ptr(pa + 4090), 7));
    map_direct(pa + PAGE_SIZE, 0, 0, false);
    assert(AcpiOsReadable(phys_ptr(pa + 4090), 20));
    assert(!AcpiOsWritable(phys_ptr(pa + 4090), 20));
    assert(!AcpiOsReadable(NULL, 1) && !AcpiOsReadable(phys_ptr(pa), 0));
    assert(!AcpiOsWritable((void *)(uintptr_t)(UINT64_MAX - 1), 4));
    ACPI_PHYSICAL_ADDRESS output = 0;
    assert(AcpiOsGetPhysicalAddress(phys_ptr(pa + 31), &output) == AE_OK && output == pa + 31);
    assert(AcpiOsGetPhysicalAddress(phys_ptr(pa + 2 * PAGE_SIZE), &output) == AE_BAD_ADDRESS);
    assert(AcpiOsGetPhysicalAddress(NULL, &output) == AE_BAD_PARAMETER);
    assert(AcpiOsGetPhysicalAddress(phys_ptr(pa), NULL) == AE_BAD_PARAMETER);
}

static void boot_mapping_tests(void) {
    const uint64_t pa = 0x800000;
    const unsigned ram_types[] = {BOOT_MEMORY_USABLE, BOOT_MEMORY_ACPI_RECLAIMABLE,
        BOOT_MEMORY_ACPI_NVS, BOOT_MEMORY_RECLAIMABLE, BOOT_MEMORY_KERNEL};
    for (unsigned i = 0; i < sizeof(ram_types) / sizeof(ram_types[0]); i++) {
        reset_fixture();
        set_ram(pa, PAGE_SIZE, ram_types[i]);
        map_direct(pa, 0, 6, true);
        memcpy(table_snapshot, backing, TABLE_BYTES);
        assert(checked_map(pa + 17, 99) == phys_ptr(pa + 17));
        assert(!map_calls && !mmio_calls && !memcmp(table_snapshot, backing, TABLE_BYTES));
        attributes((uintptr_t)phys_ptr(pa), pa, true, 6);
    }
    const unsigned mmio_types[] = {BOOT_MEMORY_RESERVED, BOOT_MEMORY_BAD, BOOT_MEMORY_FRAMEBUFFER};
    for (unsigned i = 0; i < sizeof(mmio_types) / sizeof(mmio_types[0]); i++) {
        reset_fixture();
        set_ram(pa, PAGE_SIZE, mmio_types[i]);
        assert(checked_map(pa + 3, 17) == phys_ptr(pa + 3));
        assert(mmio_calls == 1 && !map_calls && last_mmio == pa && last_mmio_length == PAGE_SIZE);
    }
    reset_fixture();
    set_ram(pa, PAGE_SIZE, BOOT_MEMORY_ACPI_NVS);
    set_ram(pa + PAGE_SIZE, PAGE_SIZE, BOOT_MEMORY_USABLE);
    assert(checked_map(pa + 4090, 20) == phys_ptr(pa + 4090));
    assert(map_calls == 2 && !mmio_calls);
    attributes((uintptr_t)phys_ptr(pa), pa, true, 0);
    attributes((uintptr_t)phys_ptr(pa + PAGE_SIZE), pa + PAGE_SIZE, true, 0);

    /* Adjacent sub-page RAM ranges may jointly describe one complete page. */
    reset_fixture();
    set_ram(pa, PAGE_SIZE / 2, BOOT_MEMORY_ACPI_NVS);
    set_ram(pa + PAGE_SIZE / 2, PAGE_SIZE / 2, BOOT_MEMORY_USABLE);
    map_direct(pa, 0, 6, true);
    assert(checked_map(pa + 17, 23) == phys_ptr(pa + 17));
    assert(!map_calls && !mmio_calls);

    for (unsigned level = 1; level <= 2; level++) {
        reset_fixture();
        set_ram(pa, 2 * PAGE_SIZE, BOOT_MEMORY_ACPI_NVS);
        map_direct(level == 2 ? 0 : pa, level, 5, true);
        memcpy(table_snapshot, backing, TABLE_BYTES);
        assert(checked_map(pa + 4090, 20) == phys_ptr(pa + 4090));
        assert(!map_calls && !mmio_calls && !memcmp(table_snapshot, backing, TABLE_BYTES));
    }

    reset_fixture();
    set_ram(pa, PAGE_SIZE, BOOT_MEMORY_RESERVED);
    assert(acpi_os_mark_firmware_memory(pa + 77, 20));
    map_direct(pa, 0, 5, true);
    assert(checked_map(pa + 12, 32) == phys_ptr(pa + 12));
    assert(!mmio_calls && !map_calls);
    attributes((uintptr_t)phys_ptr(pa), pa, true, 5);
    assert(acpi_os_mark_firmware_memory(pa + PAGE_SIZE, 1));
    assert(firmware_memory_count == 1);
    assert(!acpi_os_mark_firmware_memory(pa, 0));
    assert(!acpi_os_mark_firmware_memory(UINT64_C(0x800000000000), 1));
    assert(!acpi_os_mark_firmware_memory(UINT64_C(0x7fffffffffff), 2));

    reset_fixture();
    set_ram(pa, PAGE_SIZE, BOOT_MEMORY_ACPI_NVS);
    map_direct(pa, 0, 5, false);
    assert(checked_map(pa, 32) == phys_ptr(pa));
    assert(map_calls == 1 && !mmio_calls);
    attributes((uintptr_t)phys_ptr(pa), pa, true, 5);
    reset_fixture();
    set_ram(pa, PAGE_SIZE, BOOT_MEMORY_USABLE);
    fail_map = true;
    assert(!checked_map(pa, 32) && map_calls == 1 && !mmio_calls);
    reset_fixture();
    set_ram(pa, PAGE_SIZE, BOOT_MEMORY_USABLE);
    map_leaf((uintptr_t)phys_ptr(pa), pa + PAGE_SIZE, 0, 0, true);
    assert(!checked_map(pa, 32) && !map_calls && !mmio_calls);
    reset_fixture();
    fail_mmio = true;
    assert(!checked_map(pa, 32) && mmio_calls == 1 && !map_calls);

    reset_fixture();
    assert(!checked_map(pa, 0));
    assert(!checked_map(pa, ACPI_MAP_LIMIT + 1));
    assert(!checked_map(UINT64_C(0x800000000000), 1));
    assert(!checked_map(UINT64_C(0x7fffffffffff), 2));
    assert(!checked_map(UINT64_MAX, 8));
    boot_info.hhdm_offset = 0;
    assert(!checked_map(pa, 1));
    boot_info.hhdm_offset = UINT64_MAX - pa;
    assert(!checked_map(pa, 2));
    assert(!map_calls && !mmio_calls);
}

static void runtime_mapping_tests(void) {
    const uint64_t pa = 0x800000;
    /* No boot OSL mapping call: the first runtime touch borrows an existing UC leaf. */
    for (unsigned level = 0; level <= 2; level++) {
        for (unsigned pat = 0; pat < 8; pat++) {
            reset_fixture();
            pat_value = UINT64_C(0x0606000606060606); /* only index 5 is UC */
            map_direct(level == 2 ? 0 : pa, level, pat, true);
            begin_runtime();
            assert(checked_map(pa + 33, 77) == (pat == 5 ? phys_ptr(pa + 33) : NULL));
            unchanged_tables();
        }
    }
    for (unsigned pat = 0; pat < 8; pat++) {
        reset_fixture();
        has_pat = false;
        map_direct(pa, 0, pat, true);
        begin_runtime();
        assert(checked_map(pa, 32) == ((pat & 3) == 3 ? phys_ptr(pa) : NULL));
        assert(!msr_calls);
        unchanged_tables();
    }
    for (unsigned level = 0; level <= 2; level++) {
        reset_fixture();
        set_ram(pa, 2 * PAGE_SIZE, BOOT_MEMORY_ACPI_NVS);
        map_direct(level == 2 ? 0 : pa, level, 0, true);
        if (!level)
            map_direct(pa + PAGE_SIZE, 0, 6, true);
        begin_runtime();
        assert(checked_map(pa + 4090, 20) == phys_ptr(pa + 4090));
        AcpiOsUnmapMemory(phys_ptr(pa), PAGE_SIZE);
        unchanged_tables();
    }
    for (unsigned kind = 0; kind < 6; kind++) {
        reset_fixture();
        if (kind != 0)
            map_direct(pa, 0, kind == 3 ? 0 : 3, kind != 1);
        if (kind == 2)
            *entry_at((uintptr_t)phys_ptr(pa), 2, false) &= ~UINT64_C(2);
        if (kind == 4)
            *entry_at((uintptr_t)phys_ptr(pa), 0, false) += PAGE_SIZE;
        if (kind == 5)
            cr4 = UINT64_C(1) << 12;
        begin_runtime();
        assert(!checked_map(pa, 32));
        unchanged_tables();
    }
    reset_fixture();
    map_direct(pa, 0, 3, true);
    begin_runtime();
    assert(!checked_map(pa + 4090, 20));
    unchanged_tables();
    reset_fixture();
    set_ram(pa, PAGE_SIZE, BOOT_MEMORY_ACPI_NVS);
    begin_runtime();
    assert(!checked_map(pa, 32)); /* RAM cannot create a runtime mapping either. */
    unchanged_tables();

    reset_fixture();
    map_direct(pa, 0, 3, true);
    begin_runtime();
    for (unsigned width = 8; width <= 64; width *= 2) {
        access_count = 0;
        uint64_t expected = UINT64_C(0xfedcba9876543210) >> (64 - width);
        UINT64 value = 0;
        assert(AcpiOsWriteMemory(pa + 0x100, expected, width) == AE_OK);
        assert(AcpiOsReadMemory(pa + 0x100, &value, width) == AE_OK && value == expected);
    }
    UINT64 value = 0x1234;
    assert(AcpiOsReadMemory(pa + PAGE_SIZE, &value, 32) == AE_BAD_ADDRESS && value == 0x1234);
    assert(AcpiOsWriteMemory(pa + PAGE_SIZE, 1, 32) == AE_BAD_ADDRESS);
    assert(AcpiOsReadMemory(pa, NULL, 32) == AE_BAD_PARAMETER);
    assert(AcpiOsReadMemory(pa, &value, 24) == AE_BAD_PARAMETER);
    assert(AcpiOsWriteMemory(pa, 1, 24) == AE_BAD_PARAMETER);
    unchanged_tables();
}

static void mixed_mapping_tests(void) {
    const uint64_t pa = 0x800000;
    reset_fixture();
    set_ram(pa, PAGE_SIZE, BOOT_MEMORY_ACPI_NVS);
    set_ram(pa + 3 * PAGE_SIZE, PAGE_SIZE, BOOT_MEMORY_USABLE);
    map_direct(pa, 0, 5, true);
    map_direct(pa + 3 * PAGE_SIZE, 0, 6, true);
    uint64_t first_leaf = *entry_at((uintptr_t)phys_ptr(pa), 0, false);
    uint64_t second_leaf = *entry_at((uintptr_t)phys_ptr(pa + 3 * PAGE_SIZE), 0, false);
    assert(checked_map(pa + 17, 6 * PAGE_SIZE - 34) == phys_ptr(pa + 17));
    assert(!map_calls && mmio_calls == 2);
    assert(mmio_requests[0].physical == pa + PAGE_SIZE && mmio_requests[0].length == 2 * PAGE_SIZE);
    assert(mmio_requests[1].physical == pa + 4 * PAGE_SIZE && mmio_requests[1].length == 2 * PAGE_SIZE);
    assert(*entry_at((uintptr_t)phys_ptr(pa), 0, false) == first_leaf);
    assert(*entry_at((uintptr_t)phys_ptr(pa + 3 * PAGE_SIZE), 0, false) == second_leaf);

    /* A mixed span borrows WB RAM and UC MMIO, with a per-page decision. */
    for (unsigned i = 0; i < 6; i++)
        if (i != 0 && i != 3)
            map_direct(pa + i * PAGE_SIZE, 0, 3, true);
    begin_runtime();
    assert(checked_map(pa + 17, 6 * PAGE_SIZE - 34) == phys_ptr(pa + 17));
    unchanged_tables();

    /* Even a request wholly inside the RAM fragment must reject an ambiguous page. */
    for (unsigned partial = 0; partial < 3; partial++) {
        for (unsigned runtime = 0; runtime < 2; runtime++) {
            reset_fixture();
            if (partial == 0)
                set_ram(pa + 1, PAGE_SIZE - 1, BOOT_MEMORY_ACPI_NVS);
            else if (partial == 1)
                set_ram(pa, PAGE_SIZE - 1, BOOT_MEMORY_USABLE);
            else {
                set_ram(pa, PAGE_SIZE / 2, BOOT_MEMORY_USABLE);
                set_ram(pa + PAGE_SIZE / 2 + 1, PAGE_SIZE / 2 - 1, BOOT_MEMORY_ACPI_NVS);
            }
            map_direct(pa, 0, runtime ? 3 : 0, true);
            if (runtime)
                begin_runtime();
            assert(!checked_map(pa + 100, 1));
            assert(!map_calls && !mmio_calls);
            if (runtime)
                unchanged_tables();
        }
    }
}

static void unaligned_memory_tests(void) {
    const uint64_t pa = 0x800000;
    const unsigned offsets[] = {1, PAGE_SIZE - 1};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        for (unsigned width = 8; width <= 64; width *= 2) {
            reset_fixture();
            /* Mapping preparation performs no memory access. The first real
               touch is a runtime write/read, sometimes crossing a page. */
            assert(checked_map(pa, 2 * PAGE_SIZE) == phys_ptr(pa));
            begin_runtime();
            uint64_t expected = UINT64_C(0xfedcba9876543210) >> (64 - width);
            UINT64 value = 0;
            assert(AcpiOsWriteMemory(pa + offsets[i], expected, width) == AE_OK);
            assert(AcpiOsReadMemory(pa + offsets[i], &value, width) == AE_OK);
            assert(value == expected && access_count == 2);
            assert(accesses[0].width == width && accesses[0].write);
            assert(accesses[1].width == width && !accesses[1].write);
            unchanged_tables();
        }
    }
}

static void setup_mcfg(void) {
    table_status = AE_OK;
    fake_mcfg.mcfg.Header.Length = sizeof(ACPI_TABLE_MCFG) + 2 * sizeof(ACPI_MCFG_ALLOCATION);
    memcpy(fake_mcfg.mcfg.Header.Signature, ACPI_SIG_MCFG, 4);
    fake_mcfg.allocation[0] = (ACPI_MCFG_ALLOCATION){
        .Address = 0x100000, .PciSegment = 9, .StartBusNumber = 0x20, .EndBusNumber = 0x21};
    fake_mcfg.allocation[1] = (ACPI_MCFG_ALLOCATION){
        .Address = 0x100000, .PciSegment = 7, .StartBusNumber = 0x20, .EndBusNumber = 0x21};
}
static void released(unsigned gets, unsigned puts) {
    assert(table_gets == gets && table_puts == puts && !table_references);
}
static void check_accesses(uint64_t function, unsigned reg, unsigned width, bool write, uint64_t value) {
    unsigned count = width == 64 ? 2 : 1;
    assert(access_count == count);
    for (unsigned i = 0; i < count; i++) {
        assert(accesses[i].address == (uintptr_t)phys_ptr(function + reg + i * 4));
        assert(accesses[i].width == (width == 64 ? 32 : width));
        assert(accesses[i].write == write);
        if (write && width == 64)
            assert((uint32_t)accesses[i].value == (uint32_t)(value >> (32 * i)));
    }
    assert(!port_calls);
}
static void ecam_tests(void) {
    ACPI_PCI_ID pci = {.Segment = 7, .Bus = 0x20, .Device = 3, .Function = 2};
    const uint64_t function = UINT64_C(0x100000) + (UINT64_C(0x20) << 20) + (3 << 15) + (2 << 12);
    const uint64_t rebased = UINT64_C(0x100000) + (3 << 15) + (2 << 12);
    reset_fixture();
    setup_mcfg();
    memset(phys_ptr(function), 0xa5, PAGE_SIZE);
    memset(phys_ptr(rebased), 0x3c, PAGE_SIZE);
    UINT64 value = 0;
    for (unsigned width = 8; width <= 64; width *= 2) {
        unsigned reg = 0x40 + width;
        uint64_t expected = UINT64_C(0xfedcba9876543210) >> (64 - width);
        uint8_t *bytes = phys_ptr(function);
        access_count = 0;
        assert(AcpiOsWritePciConfiguration(&pci, reg, expected, width) == AE_OK);
        check_accesses(function, reg, width, true, expected);
        assert(bytes[reg - 1] == 0xa5 && bytes[reg + width / 8] == 0xa5);
        access_count = 0;
        assert(AcpiOsReadPciConfiguration(&pci, reg, &value, width) == AE_OK && value == expected);
        check_accesses(function, reg, width, false, expected);
        assert(last_mmio == function && last_mmio_length == PAGE_SIZE);
    }
    released(8, 8);
    for (unsigned i = 0; i < PAGE_SIZE; i++)
        assert(((uint8_t *)phys_ptr(rebased))[i] == 0x3c);

    /* Table memory may be reclaimed immediately after PutTable; base is copied first. */
    setup_mcfg();
    poison_table_on_put = true;
    access_count = 0;
    assert(AcpiOsReadPciConfiguration(&pci, 0x80, &value, 64) == AE_OK);
    assert(last_mmio == function);
    released(9, 9);

    reset_fixture();
    setup_mcfg();
    map_direct(function, 0, 3, true);
    begin_runtime();
    assert(AcpiOsReadPciConfiguration(&pci, 0x80, &value, 64) == AE_OK);
    assert(value == UINT64_C(0xfedcba9876543210));
    check_accesses(function, 0x80, 64, false, value);
    released(1, 1);
    unchanged_tables();
    *entry_at((uintptr_t)phys_ptr(function), 0, false) &= ~UINT64_C(1);
    begin_runtime();
    value = 0x1234;
    access_count = 0;
    assert(AcpiOsReadPciConfiguration(&pci, 0x80, &value, 64) == AE_BAD_ADDRESS);
    assert(value == 0x1234 && !access_count);
    released(2, 2);
    unchanged_tables();

    reset_fixture();
    setup_mcfg();
    fail_mmio = true;
    value = 0x1234;
    assert(AcpiOsReadPciConfiguration(&pci, 0, &value, 32) == AE_BAD_ADDRESS && value == 0x1234);
    assert(mmio_calls == 1 && !access_count && !port_calls);
    released(1, 1);
    assert(AcpiOsWritePciConfiguration(&pci, 0, 1, 32) == AE_BAD_ADDRESS);
    released(2, 2);

    for (unsigned malformed = 0; malformed < 2; malformed++) {
        reset_fixture();
        setup_mcfg();
        fake_mcfg.mcfg.Header.Length = malformed ? sizeof(ACPI_TABLE_MCFG) + 1 : sizeof(ACPI_TABLE_MCFG) - 1;
        assert(AcpiOsReadPciConfiguration(&pci, 0, &value, 32) == AE_BAD_HEADER);
        released(1, 1);
        assert(!mmio_calls && !port_calls && !access_count);
    }
    for (unsigned missing = 0; missing < 5; missing++) {
        reset_fixture();
        setup_mcfg();
        if (missing == 0)
            fake_mcfg.allocation[1].PciSegment++;
        else if (missing == 1)
            fake_mcfg.allocation[1].StartBusNumber++;
        else if (missing == 2)
            fake_mcfg.allocation[1].EndBusNumber = 0x1f;
        else if (missing == 3)
            fake_mcfg.allocation[1].Address = 0;
        else
            fake_mcfg.allocation[1].Address++;
        assert(AcpiOsReadPciConfiguration(&pci, 0, &value, 32) == AE_SUPPORT);
        released(1, 1);
        assert(!mmio_calls && !port_calls && !access_count);
    }
    reset_fixture();
    setup_mcfg();
    fake_mcfg.allocation[1].Address = UINT64_C(0xfffffffffff00000);
    assert(AcpiOsReadPciConfiguration(&pci, 0, &value, 32) == AE_BAD_ADDRESS);
    released(1, 1);
    assert(!mmio_calls && !port_calls);
    reset_fixture();
    table_status = AE_NO_MEMORY;
    assert(AcpiOsReadPciConfiguration(&pci, 0, &value, 32) == AE_NO_MEMORY);
    released(1, 0);
    table_status = AE_NOT_FOUND;
    assert(AcpiOsReadPciConfiguration(&pci, 0, &value, 32) == AE_SUPPORT);
    released(2, 0);
    assert(!mmio_calls && !port_calls);

    reset_fixture();
    setup_mcfg();
    assert(AcpiOsReadPciConfiguration(NULL, 0, &value, 32) == AE_BAD_PARAMETER);
    assert(AcpiOsReadPciConfiguration(&pci, 0, NULL, 32) == AE_BAD_PARAMETER);
    assert(AcpiOsReadPciConfiguration(&pci, 0, &value, 24) == AE_BAD_PARAMETER);
    assert(AcpiOsReadPciConfiguration(&pci, 3, &value, 32) == AE_BAD_PARAMETER);
    assert(AcpiOsReadPciConfiguration(&pci, 4096, &value, 8) == AE_BAD_PARAMETER);
    assert(AcpiOsReadPciConfiguration(&pci, 4092, &value, 64) == AE_BAD_PARAMETER);
    pci.Bus = 256;
    assert(AcpiOsReadPciConfiguration(&pci, 0, &value, 32) == AE_BAD_PARAMETER);
    pci.Bus = 0x20; pci.Device = 32;
    assert(AcpiOsReadPciConfiguration(&pci, 0, &value, 32) == AE_BAD_PARAMETER);
    pci.Device = 3; pci.Function = 8;
    assert(AcpiOsReadPciConfiguration(&pci, 0, &value, 32) == AE_BAD_PARAMETER);
    released(0, 0);
    assert(!mmio_calls && !port_calls && !access_count);
}

int main(void) {
    /* Align the HHDM backing to 1 GiB so real huge leaves preserve PA offsets.
       mmap reserves sparse virtual space; only fixture pages consume RAM. */
    size_t reserved = BACKING_SIZE + GIGA_SIZE;
    void *allocation = mmap(NULL, reserved, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(allocation != MAP_FAILED);
    uintptr_t aligned = ((uintptr_t)allocation + GIGA_SIZE - 1) & ~(GIGA_SIZE - 1);
    size_t prefix = aligned - (uintptr_t)allocation;
    size_t suffix = reserved - prefix - BACKING_SIZE;
    if (prefix)
        assert(!munmap(allocation, prefix));
    if (suffix)
        assert(!munmap((void *)(aligned + BACKING_SIZE), suffix));
    backing = (void *)aligned;
    acpi_os_get_diagnostics(NULL);
    page_walk_tests();
    boot_mapping_tests();
    runtime_mapping_tests();
    mixed_mapping_tests();
    unaligned_memory_tests();
    ecam_tests();
    assert(!table_references);
    assert(!munmap(backing, BACKING_SIZE));
    puts("ACPICA OSL mapping: real 4K/2M/1G walks, PAT/cache policy, boot/runtime permissions, immutable runtime borrowing and ECAM success/error paths passed");
    return 0;
}
'''


def replace_hardware_body(source, signature, statement):
    """Replace one explicit privileged boundary, never OSL policy functions."""
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    body = source.index("{", start)
    end = source.index("\n}\n", body)
    return source[:body + 1] + "\n    " + statement + source[end:]


def main():
    with tempfile.TemporaryDirectory(prefix="coast-acpi-osl-mapping-") as temporary:
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
        osl = (ROOT / "drivers/acpi/osl.c").read_text()
        for signature, statement in (
                ("static uint64_t control_register(", "return test_control_register(number);"),
                ("static bool cpu_has_pat(", "return test_cpu_has_pat();"),
                ("static uint64_t msr_read(", "return test_msr_read(index);")):
            osl = replace_hardware_body(osl, signature, statement)
        # Observe helper call widths while keeping their original memory accesses.
        for signature in ("static uint64_t read_volatile(", "static void write_volatile("):
            assert osl.count(signature) == 1, signature
            name = "read_volatile" if "read_volatile" in signature else "write_volatile"
            osl = osl.replace(signature, signature.replace(name, "test_real_" + name))
        osl_source = folder / "osl-shimmed.c"
        osl_source.write_text(osl)
        source = folder / "test.c"
        source.write_text(SOURCE.replace("OSL_SOURCE", str(osl_source)))
        binary = folder / "test"
        compiler = shlex.split(os.environ.get("HOSTCC", "cc"))
        subprocess.run(compiler + ["-std=gnu11", "-O2", "-g", "-Wall", "-Wextra",
                                  "-Werror", "-Wno-unused-parameter", "-D__COAST__",
                                  "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                  "-I" + str(include), "-I" + str(ROOT / "lib/acpica/source/include"),
                                  str(source), "-o", str(binary)], check=True)
        environment = os.environ.copy()
        # LeakSanitizer is incompatible with some sandbox ptrace supervisors.
        # Fixture backing is explicitly unmapped; ASan/UBSan stay enabled.
        environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1")
        subprocess.run([str(binary)], check=True, timeout=30, env=environment)


if __name__ == "__main__":
    main()
