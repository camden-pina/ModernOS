#!/usr/bin/env python3
"""Check SCI availability throughout the actual bootstrap AML call sequence.

Compiles the real acpi_start/kernel_main functions and upstream FACS global-lock
acquisition/SCI handler with host-safe hardware and scheduler boundaries. The
firmware release and SCI delivery are a deterministic model, not guest hardware
or an end-to-end AML test. Negative controls remove STI or the pre-SMP CLI.
"""
from pathlib import Path
import os
import re
import resource
import shlex
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    body = source.index('{', start)
    # Internal ACPICA files can declare a static callback before defining it.
    while source.find(';', start, body) != -1:
        start = source.index(signature, start + len(signature))
        body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


HARNESS = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ACPI_NO_ERROR_MESSAGES
#define ACPI_USE_SYSTEM_CLIBRARY
#include <acpi.h>
#include <accommon.h>
#include <acinterp.h>
#include <boot/info.h>

static bool irq_enabled, sci_installed, timer_masked, scheduler_initialized;
static bool gpes_reserved, runtime_prepared, power_initialized;
static bool telemetry_started, coordinator_started;
static unsigned handshakes, irq_enables, irq_disables;
static jmp_buf boot_finished;
struct boot_info boot_info;
int64_t kernel_boot_epoch;
static void *fadt;
static ACPI_TABLE_FACS fake_facs __attribute__((aligned(8)));
static ACPI_OPERAND_OBJECT fake_global_mutex;
ACPI_TABLE_FACS *AcpiGbl_FACS = &fake_facs;
ACPI_OPERAND_OBJECT *AcpiGbl_GlobalLockMutex = &fake_global_mutex;
ACPI_SEMAPHORE AcpiGbl_GlobalLockSemaphore;
ACPI_SPINLOCK AcpiGbl_GlobalLockPendingLock;
UINT16 AcpiGbl_GlobalLockHandle;
BOOLEAN AcpiGbl_GlobalLockAcquired, AcpiGbl_GlobalLockPresent, AcpiGbl_GlobalLockPending;
static unsigned semaphore_units;

static int test_printk(const char *format, ...) { (void)format; return 0; }
#define kprintf test_printk
#define pr_info test_printk
#define pr_warn test_printk
#define pr_notice test_printk
static void panic(const char *format, ...) __attribute__((noreturn));
static void panic(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args);
    abort();
}

static void test_sti(void) {
    assert(sci_installed && timer_masked && !scheduler_initialized);
    irq_enabled = true;
    ++irq_enables;
}
static void test_cli(void) { irq_enabled = false; ++irq_disables; }
ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK lock) { (void)lock; return 0; }
void AcpiOsReleaseLock(ACPI_SPINLOCK lock, ACPI_CPU_FLAGS flags) {
    (void)lock; (void)flags;
}
ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE sem, UINT32 units) {
    assert(sem == AcpiGbl_GlobalLockSemaphore && units == 1 && !semaphore_units);
    semaphore_units = 1;
    return AE_OK;
}
ACPI_STATUS AcpiExSystemWaitMutex(ACPI_MUTEX mutex, UINT16 timeout) {
    assert(mutex == fake_global_mutex.Mutex.OsMutex && timeout == ACPI_WAIT_FOREVER);
    return AE_OK;
}
static UINT32 AcpiEvGlobalLockHandler(void *context);

/* ACTUAL_GLOBAL_LOCK_CODE */

ACPI_STATUS AcpiExSystemWaitSemaphore(ACPI_SEMAPHORE sem, UINT16 timeout) {
    assert(sem == AcpiGbl_GlobalLockSemaphore && timeout == ACPI_WAIT_FOREVER);
    assert(AcpiGbl_GlobalLockPending && (fake_facs.GlobalLock & 3) == 3);
    /* Model firmware giving up ownership and asserting the global-lock SCI. */
    volatile uint32_t *lock = (void *)((uint8_t *)&fake_facs + 16);
    assert(acpi_os_release_global_lock(lock));
    assert(irq_enabled && sci_installed);
    assert(AcpiEvGlobalLockHandler(NULL) == ACPI_INTERRUPT_HANDLED);
    assert(semaphore_units == 1 && !AcpiGbl_GlobalLockPending);
    semaphore_units = 0;
    return AE_OK;
}
static void boot_aml(void) {
    assert(irq_enabled && sci_installed && timer_masked && !scheduler_initialized);
    assert(gpes_reserved);
    fake_facs.GlobalLock = 2; /* Firmware owns the hardware global lock. */
    AcpiGbl_GlobalLockPresent = TRUE;
    AcpiGbl_GlobalLockAcquired = FALSE;
    assert(AcpiEvAcquireGlobalLock(ACPI_WAIT_FOREVER) == AE_OK);
    assert(AcpiGbl_GlobalLockAcquired && (fake_facs.GlobalLock & 3) == 2);
    assert(!acpi_os_release_global_lock((void *)((uint8_t *)&fake_facs + 16)));
    AcpiGbl_GlobalLockAcquired = FALSE;
    ++handshakes;
}
ACPI_STATUS AcpiLoadTables(void) { assert(!irq_enabled); return AE_OK; }
ACPI_STATUS AcpiEnableSubsystem(UINT32 flags) {
    assert(flags == ACPI_FULL_INITIALIZATION && !irq_enabled && timer_masked);
    sci_installed = true;
    return AE_OK;
}
ACPI_STATUS AcpiEvaluateObject(ACPI_HANDLE object, ACPI_STRING path,
        ACPI_OBJECT_LIST *args, ACPI_BUFFER *out) {
    assert(!object && !strcmp(path, "\\_PIC") && args->Count == 1 && !out);
    boot_aml();
    return AE_OK;
}
ACPI_STATUS AcpiInitializeObjects(UINT32 flags) {
    assert(flags == ACPI_FULL_INITIALIZATION);
    boot_aml();
    return AE_OK;
}
static bool acpi_os_prepare_gpes(void) {
    assert(sci_installed && !irq_enabled && !handshakes && !gpes_reserved);
    gpes_reserved = true;
    return true;
}
static bool acpi_os_prepare_runtime(void) {
    assert(irq_enabled && gpes_reserved && handshakes == 2 && !runtime_prepared);
    runtime_prepared = true;
    return true;
}
ACPI_STATUS AcpiUpdateAllGpes(void) {
    assert(irq_enabled && runtime_prepared); return AE_OK;
}
ACPI_STATUS AcpiWalkNamespace(ACPI_OBJECT_TYPE type, ACPI_HANDLE root, UINT32 depth,
        ACPI_WALK_CALLBACK down, ACPI_WALK_CALLBACK up, void *context, void **out) {
    assert(type == ACPI_TYPE_DEVICE && root == ACPI_ROOT_OBJECT && !up && !out);
    assert(depth == ACPI_UINT32_MAX);
    return down(NULL, 0, context, out);
}
ACPI_STATUS AcpiGetSleepTypeData(UINT8 state, UINT8 *a, UINT8 *b) {
    assert(state == ACPI_STATE_S5); *a = *b = 0; return AE_OK;
}
ACPI_STATUS AcpiInstallFixedEventHandler(UINT32 event, ACPI_EVENT_HANDLER handler,
        void *context) {
    assert(event == ACPI_EVENT_POWER_BUTTON && handler && !context && irq_enabled);
    return AE_OK;
}
const char *AcpiFormatException(ACPI_STATUS status) { (void)status; return "mock status"; }
static void acpi_selftest_init(void) { assert(irq_enabled); }
static void power_button_event(void) { assert(irq_enabled); }
static void acpi_power_init(void) {
    assert(irq_enabled && runtime_prepared && !scheduler_initialized);
    power_initialized = true;
}

/* ACTUAL_ACPI_START_CODE */

static void gdt_init(void) {}
static void idt_init(void) {}
static void user_cpu_init(void) {}
static void tsc_init(void) {}
static uint64_t tsc_uptime_us(void) { return 0; }
static void printk_set_uptime_fn(uint64_t (*fn)(void)) { (void)fn; }
static void pmm_init(struct boot_memory_range *map, uint64_t count) { (void)map; (void)count; }
static void vmem_init(uintptr_t address, size_t length) { (void)address; (void)length; }
static void fbcon_init(struct boot_framebuffer *fb) { (void)fb; }
static bool acpi_init(void *rsdp) { assert(rsdp && !irq_enabled); fadt = rsdp; return true; }
static void hpet_init(void) {}
static void apic_init(void) { assert(!irq_enabled); timer_masked = true; }
static void pci_init(void) { assert(timer_masked && !irq_enabled); }
static bool kernel_usb_input_start(void) {
    assert(irq_enabled == (boot_info.rsdp != NULL)); return true;
}
static void vfs_init(struct boot_module *modules, uint64_t count) { (void)modules; (void)count; }
static void vfs_discover(void) {}
static void audio_init(void) { if (boot_info.rsdp) boot_aml(); }
static void sched_init(struct boot_smp *smp) {
    (void)smp;
    assert(!irq_enabled && timer_masked);
    scheduler_initialized = true;
}
static int process_spawn(const char *path, char **argv, int ppid, const char *cwd,
        int cpu, bool init) {
    (void)path; (void)argv; (void)ppid; (void)cwd; (void)cpu;
    assert(init && scheduler_initialized && !irq_enabled); return 1;
}
static bool acpi_os_start_workers(void) { assert(!irq_enabled); return true; }
static bool acpi_selftest_start(void) { assert(!irq_enabled); return true; }
/* These boundaries create scheduled tasks after BSP-local initialization.
 * They must not require sched_ready(): sched_start has not run yet. The actual
 * telemetry-start implementation/order is separately compiled by its harness. */
static bool acpi_telemetry_start(void) {
    assert(scheduler_initialized && power_initialized && !irq_enabled);
    telemetry_started = true; return true;
}
static bool power_start(bool acpi) {
    assert(scheduler_initialized && !irq_enabled);
    assert(acpi == (boot_info.rsdp != NULL) && telemetry_started == acpi);
    coordinator_started = true; return true;
}
static void network_start(void) {}
static void input_task(void *arg) { (void)arg; }
static int task_kernel(const char *name, void (*fn)(void *), void *arg, int cpu) {
    (void)name; (void)fn; (void)arg; (void)cpu; return 2;
}
static void sched_start(void) __attribute__((noreturn));
static void sched_start(void) { assert(!irq_enabled && coordinator_started); longjmp(boot_finished, 1); }

/* ACTUAL_KERNEL_MAIN_CODE */

static void run_boot(volatile unsigned acpi) {
    irq_enabled = sci_installed = timer_masked = scheduler_initialized = false;
    gpes_reserved = runtime_prepared = power_initialized = false;
    telemetry_started = coordinator_started = false;
    handshakes = irq_enables = irq_disables = 0;
    boot_info.rsdp = acpi ? &fake_facs : NULL;
    if (!setjmp(boot_finished)) kernel_main();
    assert(scheduler_initialized && !irq_enabled && irq_disables == 1);
    assert(irq_enables == acpi && handshakes == acpi * 3);
    assert(gpes_reserved == acpi && runtime_prepared == acpi);
    assert(power_initialized == acpi && telemetry_started == acpi && coordinator_started);
}
int main(void) {
    run_boot(1);
    run_boot(0);
    puts("PASS bootstrap SCI: _PIC, object initialization and audio global-lock handshakes; CLI before SMP; ACPI-absent path");
    return 0;
}
'''


def main():
    acpi = (ROOT / 'drivers/acpi/acpi.c').read_text()
    init = (ROOT / 'init/main.c').read_text()
    global_lock = (ROOT / 'lib/acpica/source/components/events/evglock.c').read_text()
    osl = (ROOT / 'drivers/acpi/osl.c').read_text()
    lapic = (ROOT / 'arch/x86/kernel/interrupts/lapic.c').read_text()
    scheduler = (ROOT / 'kernel/sched/sched.c').read_text()
    # The real bootstrap timer stays masked, and premature timer interrupts
    # return before touching uninitialized CPU-local/scheduler state.
    assert 'apic_write(0x320, 0x10000);' in function(lapic, 'void apic_init_cpu(void)')
    assert 'apic_timer_start(' not in function(lapic, 'void apic_init(void)')
    interrupt = function(scheduler, 'register_t *sched_interrupt(')
    assert re.search(r'\{\s*if \(!sched_ready\(\)\)\s*return frame;', interrupt)
    bootstrap = function(init, 'void kernel_main(void)')
    assert 'apic_timer_start(' not in bootstrap
    startup = ''.join(function(acpi, signature) for signature in (
        'static UINT32 power_button(', 'static ACPI_STATUS count_device(', 'bool acpi_start(void)'))
    lock_code = '\n'.join(function(source, signature) for source, signature in (
        (osl, 'bool acpi_os_acquire_global_lock('),
        (osl, 'bool acpi_os_release_global_lock('),
        (global_lock, 'static UINT32\nAcpiEvGlobalLockHandler ('),
        (global_lock, 'ACPI_STATUS\nAcpiEvAcquireGlobalLock (')))
    source = HARNESS.replace('/* ACTUAL_GLOBAL_LOCK_CODE */', lock_code)
    source = source.replace('/* ACTUAL_ACPI_START_CODE */', startup)
    source = source.replace('/* ACTUAL_KERNEL_MAIN_CODE */', bootstrap)
    source = source.replace('__asm__ volatile("sti" ::: "memory");', 'test_sti();')
    source = source.replace('__asm__ volatile("cli" ::: "memory");', 'test_cli();')
    source = re.sub(r'__asm__ volatile\("pushfq; popq %0" : "=r"\((\w+)\)\);',
                    lambda match: match[1] + ' = irq_enabled ? 0x200 : 0;', source)
    compiler = shlex.split(os.environ.get('HOSTCC', 'cc'))
    with tempfile.TemporaryDirectory(prefix='coast-acpi-boot-') as temporary:
        folder = Path(temporary)
        include = folder / 'boot'
        include.mkdir()
        (include / 'info.h').write_text((ROOT / 'include/boot/info.h').read_text())
        environment = os.environ.copy()
        environment.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
        for label, body, failure in (
                ('current', source, None),
                ('missing-sti', source.replace('    test_sti();', ''),
                 'SCI interrupts disabled before boot AML'),
                ('missing-cli', source.replace('    test_cli();', ''), 'sched_init')):
            file = folder / (label + '.c')
            binary = folder / label
            file.write_text(body)
            subprocess.run(compiler + ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                '-Werror', '-Wno-unused-parameter', '-Wno-unused-function', '-D__COAST__',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(folder), '-I' + str(ROOT / 'lib/acpica/source/include'),
                str(file), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5,
                cwd=folder, env=environment,
                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
            if failure is None:
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode == -signal.SIGABRT and failure in result.stderr, (
                    label + ': wrong negative-control failure\n' + result.stderr)
                print('PASS negative control detected: ' + label)


if __name__ == '__main__':
    main()
