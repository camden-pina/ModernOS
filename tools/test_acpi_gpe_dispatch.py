#!/usr/bin/env python3
"""Host regression for real ACPICA GPE dispatch and the real Coast OSL.

Extracts four complete, unmodified functions from the checked-in upstream
``evgpe.c`` and compiles them with the actual OSL and ACPICA internal types.
Only privileged/kernel boundaries, hardware GPE registers, AML evaluation and
error reporting are simulated. This verifies software admission, ordering and
GPE lifetime, NOT hardware, interrupt delivery, or AML interpreter integration.

All four event workers and the notify worker are parked, both ordinary rings
are filled, and 1024 distinct ACPI_GPE_EVENT_INFO objects are dispatched. The
notify worker remains parked until every admitted method and finish enqueue
has completed, so every finish is submitted while the ordinary notify ring is
full. All earlier notifications must run before any GPE is re-enabled. Half
of the GPEs are edge-triggered, half level-triggered. Two temporary OSL
mutations prove the test detects lost initial admission and upstream's inline
completion fallback. Production sources are never modified.
"""
from pathlib import Path
import os
import re
import resource
import shlex
import subprocess
import tempfile

from test_acpi_osl import KERNEL, SOURCE, replace_hardware_body

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "lib/acpica/source/components/events/evgpe.c"
FUNCTIONS = (
    "AcpiEvAsynchExecuteGpeMethod",
    "AcpiEvAsynchEnableGpe",
    "AcpiEvFinishGpe",
    "AcpiEvGpeDispatch",
)

HARNESS = r'''
#include "accommon.h"
#include "acevents.h"
#include "acnamesp.h"
#define _COMPONENT ACPI_EVENTS
ACPI_MODULE_NAME("gpe_host")
ACPI_SPINLOCK AcpiGbl_GpeLock;
static void ACPI_SYSTEM_XFACE AcpiEvAsynchExecuteGpeMethod(void *);
static void ACPI_SYSTEM_XFACE AcpiEvAsynchEnableGpe(void *);

#define GPE_COUNT 1024
#define RING_COUNT 256
#define CHECK(condition, reason) do { \
    if (!(condition)) { fprintf(stderr, "GPE regression: %s\n", reason); abort(); } \
} while (0)
static ACPI_GPE_EVENT_INFO events[GPE_COUNT];
static ACPI_GPE_REGISTER_INFO registers[GPE_COUNT / 8];
static ACPI_NAMESPACE_NODE methods[GPE_COUNT];
static uint8_t hardware_enable[GPE_COUNT / 8], hardware_status[GPE_COUNT / 8];
static unsigned method_calls[GPE_COUNT], clear_calls[GPE_COUNT], enable_calls[GPE_COUNT];
static unsigned event_started, notify_started, release_event, release_notify;
static unsigned event_done, notify_done, method_done, finish_done, upstream_errors;
static unsigned notify_context[RING_COUNT];

static unsigned event_index(ACPI_GPE_EVENT_INFO *event) {
    ptrdiff_t index = event - events;
    assert(index >= 0 && index < GPE_COUNT);
    assert(event->RegisterInfo == &registers[index / 8]);
    return (unsigned)index;
}
static void await_count(unsigned *counter, unsigned wanted) {
    uint64_t start = tsc_uptime_us();
    while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) != wanted) {
        CHECK(tsc_uptime_us() - start < 5000000, "host worker timed out");
        sched_sleep(100);
    }
}
static void event_blocker(void *context) {
    assert(!context);
    __atomic_add_fetch(&event_started, 1, __ATOMIC_RELEASE);
    await_count(&release_event, 1);
}
static void notify_blocker(void *context) {
    assert(!context);
    __atomic_store_n(&notify_started, 1, __ATOMIC_RELEASE);
    await_count(&release_notify, 1);
}
static void ordinary_event(void *context) {
    assert(!context);
    __atomic_add_fetch(&event_done, 1, __ATOMIC_RELAXED);
}
static void ordinary_notify(void *context) {
    unsigned index = *(unsigned *)context;
    CHECK(!__atomic_load_n(&finish_done, __ATOMIC_ACQUIRE),
          "GPE finish overtook previously pending Notify handlers");
    CHECK(__atomic_load_n(&notify_done, __ATOMIC_RELAXED) == index,
          "ordinary Notify FIFO order changed");
    __atomic_add_fetch(&notify_done, 1, __ATOMIC_RELEASE);
}

/* Model byte-wide status/enable registers and conditional EnableMask only.
   The upstream dispatch/finish functions own DisableForDispatch throughout. */
ACPI_STATUS AcpiHwLowSetGpe(ACPI_GPE_EVENT_INFO *event, UINT32 action) {
    unsigned index = event_index(event), bit = 1u << (index % 8);
    if (action == ACPI_GPE_DISABLE) {
        assert(hardware_enable[index / 8] & bit);
        assert(!event->DisableForDispatch && !method_calls[index]);
        hardware_enable[index / 8] &= ~bit;
    } else {
        assert(action == ACPI_GPE_CONDITIONAL_ENABLE);
        CHECK(__atomic_load_n(&notify_done, __ATOMIC_ACQUIRE) == RING_COUNT,
              "GPE finished before pending Notify handlers (upstream inline fallback)");
        CHECK(this_cpu()->current == worker_tasks[4],
              "GPE finish ran outside the ordered notify worker");
        assert(event->DisableForDispatch && method_calls[index] == 1);
        assert(!(hardware_status[index / 8] & bit));
        assert(!enable_calls[index]++);
        if (event->RegisterInfo->EnableMask & bit)
            hardware_enable[index / 8] |= bit;
        __atomic_add_fetch(&finish_done, 1, __ATOMIC_RELEASE);
    }
    return AE_OK;
}
ACPI_STATUS AcpiHwClearGpe(ACPI_GPE_EVENT_INFO *event) {
    unsigned index = event_index(event), bit = 1u << (index % 8);
    assert(!(hardware_enable[index / 8] & bit));
    assert(hardware_status[index / 8] & bit);
    assert(!clear_calls[index]++);
    if ((event->Flags & ACPI_GPE_XRUPT_TYPE_MASK) == ACPI_GPE_LEVEL_TRIGGERED)
        assert(method_calls[index] == 1 && event->DisableForDispatch);
    else
        assert(!method_calls[index] && !event->DisableForDispatch);
    hardware_status[index / 8] &= ~bit;
    return AE_OK;
}
/* Intentional evaluator boundary: real ACPICA evaluation-info setup/allocation
   and upstream asynchronous callbacks execute; no AML bytecode is interpreted. */
ACPI_STATUS AcpiNsEvaluate(ACPI_EVALUATE_INFO *info) {
    ptrdiff_t index = info->PrefixNode - methods;
    assert(index >= 0 && index < GPE_COUNT);
    assert(info->Flags == ACPI_IGNORE_RETURN_VALUE);
    assert(events[index].DisableForDispatch);
    assert(!(hardware_enable[index / 8] & (1u << (index % 8))));
    bool event_worker = false;
    for (unsigned i = 0; i < 4; i++)
        event_worker |= this_cpu()->current == worker_tasks[i];
    CHECK(event_worker, "GPE method ran outside the asynchronous event workers");
    assert(!method_calls[index]++);
    __atomic_add_fetch(&method_done, 1, __ATOMIC_RELEASE);
    return AE_OK;
}
ACPI_STATUS AcpiEvQueueNotifyRequest(ACPI_NAMESPACE_NODE *node, UINT32 value) {
    (void)node; (void)value;
    abort(); /* This regression selects the real METHOD dispatch path. */
}
const char *AcpiUtGetNodeName(void *object) { (void)object; return "_L00"; }
void AcpiError(const char *module, UINT32 line, const char *format, ...) {
    (void)module; (void)line; (void)format;
    __atomic_add_fetch(&upstream_errors, 1, __ATOMIC_RELAXED);
}
void AcpiException(const char *module, UINT32 line, ACPI_STATUS status,
                   const char *format, ...) {
    (void)module; (void)line; (void)status; (void)format;
    __atomic_add_fetch(&upstream_errors, 1, __ATOMIC_RELAXED);
}

UPSTREAM_FUNCTIONS

/* Poll only event-lane state under the real OSL lock. Waiting for method_done
   alone is insufficient: the method's finish may not yet have been queued. */
static void await_events_idle(void) {
    uint64_t start = tsc_uptime_us();
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&work_lock);
        bool idle = !work_pending(0);
        spin_unlock_irqrestore(&work_lock, flags);
        if (idle)
            return;
        CHECK(tsc_uptime_us() - start < 5000000, "event lane failed to drain");
        sched_sleep(100);
    }
}
int main(void) {
    void *contexts[GPE_COUNT];
    assert(AcpiOsInitialize() == AE_OK);
    assert(AcpiOsCreateLock(&AcpiGbl_GpeLock) == AE_OK);
    for (unsigned i = 0; i < GPE_COUNT; i++) {
        contexts[i] = &events[i];
        events[i].Dispatch.MethodNode = &methods[i];
        events[i].RegisterInfo = &registers[i / 8];
        events[i].Flags = ACPI_GPE_DISPATCH_METHOD |
            (i % 2 ? ACPI_GPE_EDGE_TRIGGERED : ACPI_GPE_LEVEL_TRIGGERED);
        /* Eight independent 128-GPE blocks provide >256 real contexts. */
        events[i].GpeNumber = i % 128;
        registers[i / 8].BaseGpeNumber = (i % 128) & ~7u;
        registers[i / 8].EnableMask = 255;
        registers[i / 8].EnableForRun = 255;
        hardware_enable[i / 8] = hardware_status[i / 8] = 255;
    }
    assert(acpi_os_reserve_gpes(contexts, GPE_COUNT));
    __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
    assert(acpi_os_start_workers() && worker_count == 5);
    for (unsigned i = 0; i < 4; i++)
        assert(AcpiOsExecute(OSL_GLOBAL_LOCK_HANDLER, event_blocker, NULL) == AE_OK);
    assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, notify_blocker, NULL) == AE_OK);
    await_count(&event_started, 4);
    await_count(&notify_started, 1);
    for (unsigned i = 0; i < RING_COUNT; i++) {
        notify_context[i] = i;
        assert(AcpiOsExecute(OSL_GLOBAL_LOCK_HANDLER, ordinary_event, NULL) == AE_OK);
        assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, ordinary_notify, &notify_context[i]) == AE_OK);
    }
    /* Check pressure really is present, including each ring's refusal limit. */
    assert(AcpiOsExecute(OSL_GLOBAL_LOCK_HANDLER, ordinary_event, NULL) == AE_NO_MEMORY);
    assert(AcpiOsExecute(OSL_NOTIFY_HANDLER, ordinary_notify, NULL) == AE_NO_MEMORY);
    for (unsigned i = 0; i < GPE_COUNT; i++) {
        ACPI_CPU_FLAGS flags = AcpiOsAcquireLock(AcpiGbl_GpeLock);
        UINT32 result = AcpiEvGpeDispatch(NULL, &events[i], i % 128);
        AcpiOsReleaseLock(AcpiGbl_GpeLock, flags);
        assert(result == ACPI_INTERRUPT_HANDLED);
        assert(events[i].DisableForDispatch);
        assert(!(hardware_enable[i / 8] & (1u << (i % 8))));
    }
    assert(!method_done && !finish_done);
    __atomic_store_n(&release_event, 1, __ATOMIC_RELEASE);
    await_events_idle();
    /* The ordinary notify ring was full during every upstream finish enqueue. */
    assert(!notify_done && !finish_done);
    assert(work_queues[1].count == RING_COUNT);
    __atomic_store_n(&release_notify, 1, __ATOMIC_RELEASE);
    AcpiOsWaitEventsComplete();
    for (unsigned i = 0; i < GPE_COUNT; i++) {
        CHECK(!events[i].DisableForDispatch &&
              (hardware_enable[i / 8] & (1u << (i % 8))),
              "GPE remained disabled after complete queue drain");
        assert(method_calls[i] == 1 && clear_calls[i] == 1 && enable_calls[i] == 1);
    }
    struct acpi_os_diagnostics measured;
    acpi_os_get_diagnostics(&measured);
    assert(measured.gpe_capacity == GPE_COUNT && measured.event_workers == 4);
    assert(!measured.gpe_rejections && !upstream_errors);
    assert(measured.event_rejections == 1 && measured.notification_rejections == 1);
    assert(measured.event_high_water == RING_COUNT && measured.notification_high_water == RING_COUNT);
    assert(event_done == RING_COUNT && notify_done == RING_COUNT);
    assert(method_done == GPE_COUNT && finish_done == GPE_COUNT);
    assert(AcpiOsTerminate() == AE_OK);
    for (unsigned i = 0; i < worker_count; i++) {
        assert(!pthread_join(workers[i], NULL));
        free(worker_tasks[i]);
    }
    AcpiOsDeleteLock(AcpiGbl_GpeLock);
    /* Production reservations have boot lifetime; host joins permit cleanup. */
    kfree(gpe_slots);
    assert(!live_allocations);
    puts("ACPICA GPE dispatch: 1024 real contexts, upstream disable/method/FIFO finish/reenable, full ordinary rings, four event workers + notify worker passed");
    return 0;
}
'''


def extract_function(source, name):
    """Copy exactly one upstream definition, including its complete body.

    ACPICA's outer closing brace is in column zero; all nested braces are
    indented. Require that convention and a unique definition, rather than
    replacing call sites or copying a handwritten approximation of callbacks.
    """
    pattern = rf"(?m)^(?:static void ACPI_SYSTEM_XFACE|ACPI_STATUS|UINT32)\n{re.escape(name)} \([^;{{}}]*\)\n\{{\n"
    matches = list(re.finditer(pattern, source))
    assert len(matches) == 1, f"expected one definition of {name}"
    start = matches[0].start()
    opening = source.index("\n{\n", matches[0].start())
    closing = source.index("\n}\n", opening + 3)
    function = source[start:closing + 3]
    assert function.count("\n{\n") == 1 and function.count("\n}\n") == 1, name
    line = source.count("\n", 0, start) + 1
    return f'#line {line} "{UPSTREAM.relative_to(ROOT)}"\n{function}'


def prepare_osl():
    osl = (ROOT / "drivers/acpi/osl.c").read_text()
    for signature, body in (
        ("static uint64_t control_register(", "(void)number; abort();"),
        ("static bool cpu_has_pat(", "abort();"),
        ("static uint64_t msr_read(", "return test_msr_read(index);"),
        ("static uint32_t ioapic_read(", "return test_ioapic_read(mapping, reg);"),
        ("static void ioapic_write(", "test_ioapic_write(mapping, reg, value);"),
    ):
        osl = replace_hardware_body(osl, signature, body)
    signature = "void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS where, ACPI_SIZE length)"
    assert osl.count(signature) == 1
    return osl.replace(signature, signature.replace("AcpiOsMapMemory", "test_original_map_memory"))


def main():
    # Negative controls intentionally abort; do not leave host core dumps.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    upstream = UPSTREAM.read_text()
    bodies = "\n".join(extract_function(upstream, name) for name in FUNCTIONS)
    assert SOURCE.count("static ACPI_SPINLOCK test_spin;") == 1
    prefix = SOURCE.split("static ACPI_SPINLOCK test_spin;", 1)[0]
    harness = prefix + HARNESS.replace("UPSTREAM_FUNCTIONS", bodies)
    compiler = shlex.split(os.environ.get("HOSTCC", "cc"))
    environment = os.environ.copy()
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    # ASan/UBSan remain active; the harness also explicitly counts allocations.
    environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1")
    original = prepare_osl()
    admission = "    else if (gpe) {"
    assert original.count(admission) == 1
    variants = (
        ("reserved", original, None),
        ("old-initial-capacity", original.replace(admission,
            "    else if (gpe && !index && queue->count == ACPI_WORK_CAPACITY)\n"
            "        status = AE_NO_MEMORY;\n" + admission),
            "GPE remained disabled after complete queue drain"),
        ("old-completion-capacity", original.replace(admission,
            "    else if (gpe && index && queue->count == ACPI_WORK_CAPACITY)\n"
            "        status = AE_NO_MEMORY;\n" + admission),
            "GPE finished before pending Notify handlers (upstream inline fallback)"),
    )
    with tempfile.TemporaryDirectory(prefix="coast-gpe-dispatch-") as temporary:
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
        for name, osl, failure in variants:
            osl_path = folder / f"{name}-osl.c"
            osl_path.write_text(osl)
            source = folder / f"{name}.c"
            source.write_text(harness.replace("OSL_SOURCE", str(osl_path)))
            binary = folder / name
            subprocess.run(compiler + ["-std=gnu11", "-O2", "-g", "-pthread", "-Wall", "-Wextra",
                "-Werror", "-Wno-unused-parameter", "-D__COAST__", "-DACPI_USE_SYSTEM_CLIBRARY",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + str(include), "-I" + str(ROOT / "lib/acpica/source/include"),
                str(source), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], timeout=30, env=environment,
                                    text=True, capture_output=True)
            if failure is None:
                if result.returncode:
                    raise RuntimeError(f"{name} failed ({result.returncode}):\n{result.stdout}{result.stderr}")
                print(result.stdout, end="", flush=True)
            else:
                assert result.returncode != 0 and failure in result.stderr, (
                    f"negative control {name} did not detect expected lifecycle failure:\n"
                    f"{result.stdout}{result.stderr}")
                print(f"Negative control {name}: caught {failure}", flush=True)


if __name__ == "__main__":
    main()
