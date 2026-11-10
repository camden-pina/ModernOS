#include <power.h>
/* Opt-in ACPICA integration test, using tools/fixtures/acpica-selftest.asl.
 * The normal boot path does nothing unless the exact acpi.selftest token is
 * present. Only an allocated guest RAM page is written; firmware NVS is read. */
#include <acpica.h>
#include <acpi/osl.h>
#include <boot/info.h>
#include <mm/pmm.h>
#include <printk.h>
#include <sched.h>
#include <string.h>
#include <tsc.h>

static bool enabled, initialized;
static ACPI_HANDLE test_device;
static uintptr_t test_page, nvs_address;
static unsigned failures, notifications;
static bool peer_complete;
static UINT64 peer_result;

static bool requested(void) {
    const char *p = boot_info.cmdline;
    const char key[] = "acpi.selftest";
    while (p && *p) {
        while (*p == ' ' || *p == '\t')
            ++p;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t')
            ++p;
        if ((size_t)(p - start) == sizeof(key) - 1 &&
            !memcmp(start, key, sizeof(key) - 1))
            return true;
    }
    return false;
}

static void failed(const char *stage, ACPI_STATUS status) {
    __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
    pr_err("ACPICA SELFTEST FAIL: %s (%s)\n", stage, AcpiFormatException(status));
}

static ACPI_OBJECT *evaluate(const char *name, ACPI_OBJECT *arguments, UINT32 count) {
    ACPI_OBJECT_LIST args = {count, arguments};
    ACPI_BUFFER result = {ACPI_ALLOCATE_BUFFER, NULL};
    ACPI_STATUS status = AcpiEvaluateObject(test_device, (char *)name,
                                           count ? &args : NULL, &result);
    if (ACPI_FAILURE(status)) {
        failed(name, status);
        AcpiOsFree(result.Pointer);
        return NULL;
    }
    if (!result.Pointer) {
        failed(name, AE_NULL_OBJECT);
        return NULL;
    }
    return result.Pointer;
}

static bool is_integer(const ACPI_OBJECT *object, UINT64 value) {
    return object && object->Type == ACPI_TYPE_INTEGER && object->Integer.Value == value;
}

static bool arithmetic(void) {
    ACPI_OBJECT args[2] = {
        {.Integer = {.Type = ACPI_TYPE_INTEGER, .Value = 6}},
        {.Integer = {.Type = ACPI_TYPE_INTEGER, .Value = 0x13}},
    };
    ACPI_OBJECT *result = evaluate("ARTH", args, 2);
    bool ok = is_integer(result, 112);
    AcpiOsFree(result);
    if (!ok)
        failed("AML arithmetic and mutex", AE_AML_OPERAND_VALUE);
    return ok;
}

static bool package(void) {
    static const UINT8 bytes[] = {0, 0x55, 0xaa, 0xff};
    ACPI_OBJECT *result = evaluate("PACK", NULL, 0);
    bool ok = result && result->Type == ACPI_TYPE_PACKAGE && result->Package.Count == 4;
    if (ok) {
        const ACPI_OBJECT *v = result->Package.Elements;
        ok = v && is_integer(&v[0], 0x12345678) &&
             v[1].Type == ACPI_TYPE_STRING && v[1].String.Length == 6 &&
             v[1].String.Pointer && !memcmp(v[1].String.Pointer, "ACPICA", 6) &&
             v[2].Type == ACPI_TYPE_BUFFER && v[2].Buffer.Length == sizeof(bytes) &&
             v[2].Buffer.Pointer && !memcmp(v[2].Buffer.Pointer, bytes, sizeof(bytes)) &&
             v[3].Type == ACPI_TYPE_PACKAGE && v[3].Package.Count == 2 &&
             v[3].Package.Elements && is_integer(&v[3].Package.Elements[0], 0x42) &&
             is_integer(&v[3].Package.Elements[1], 0xc0dec0de);
    }
    AcpiOsFree(result);
    if (!ok)
        failed("AML nested package/string/buffer", AE_AML_OPERAND_TYPE);
    return ok;
}

static bool memory_field(UINT32 seed) {
    ACPI_OBJECT args[2] = {
        {.Integer = {.Type = ACPI_TYPE_INTEGER, .Value = test_page}},
        {.Integer = {.Type = ACPI_TYPE_INTEGER, .Value = seed}},
    };
    ACPI_OBJECT *result = evaluate("MEMT", args, 2);
    volatile UINT32 *memory = AcpiOsMapMemory(test_page, 8);
    bool ok = result && result->Type == ACPI_TYPE_PACKAGE && result->Package.Count == 2 &&
              result->Package.Elements &&
              is_integer(&result->Package.Elements[0], seed) &&
              is_integer(&result->Package.Elements[1], seed ^ 0xa5a55a5aU) &&
              memory && memory[0] == seed && memory[1] == (seed ^ 0xa5a55a5aU);
    if (memory)
        AcpiOsUnmapMemory((void *)memory, 8);
    AcpiOsFree(result);
    if (!ok)
        failed("SystemMemory owned-page field", AE_AML_OPERAND_VALUE);
    return ok;
}

static bool nvs_field(void) {
    if (!nvs_address) {
        failed("firmware NVS range missing", AE_NOT_FOUND);
        return false;
    }
    volatile UINT8 *memory = AcpiOsMapMemory(nvs_address, 1);
    if (!memory) {
        failed("firmware NVS map", AE_BAD_ADDRESS);
        return false;
    }
    UINT8 expected = *memory;
    ACPI_OBJECT argument = {.Integer = {.Type = ACPI_TYPE_INTEGER, .Value = nvs_address}};
    ACPI_OBJECT *result = evaluate("NRED", &argument, 1);
    bool ok = is_integer(result, expected) && *memory == expected;
    AcpiOsUnmapMemory((void *)memory, 1);
    AcpiOsFree(result);
    if (!ok)
        failed("firmware NVS read-only field", AE_AML_OPERAND_VALUE);
    return ok;
}

static bool first_runtime_mmio_field(void) {
    /* The injected QEMU-only SSDT declares this read-only region statically,
     * but no boot method touches its Field. Runtime first-use must reuse the
     * UC mapping prepared before SMP, rather than create/retype a global map. */
    struct acpi_runtime_status preparation;
    struct acpi_os_diagnostics before, after;
    acpi_os_get_runtime_status(&preparation);
    acpi_os_get_diagnostics(&before);
    ACPI_OBJECT *result = evaluate("LMMR", NULL, 0);
    acpi_os_get_diagnostics(&after);
    /* The unassigned bus-hole fill value is not part of this contract. A
     * successful bounded DWord read, with no new mapping rejection, is. */
    bool ok = preparation.prepared && preparation.memory_regions_prepared &&
              result && result->Type == ACPI_TYPE_INTEGER &&
              result->Integer.Value <= UINT32_MAX &&
              before.mapping_rejections == after.mapping_rejections;
    AcpiOsFree(result);
    if (!ok)
        failed("runtime-first literal MMIO preflight Field", AE_AML_OPERAND_VALUE);
    return ok;
}

static bool gpe_registration(void) {
    ACPI_EVENT_STATUS flags = 0;
    ACPI_STATUS status = AcpiGetGpeStatus(NULL, 0x0f, &flags);
    const ACPI_EVENT_STATUS expected = ACPI_EVENT_FLAG_HAS_HANDLER |
        ACPI_EVENT_FLAG_ENABLED | ACPI_EVENT_FLAG_ENABLE_SET;
    if (ACPI_FAILURE(status) || (flags & expected) != expected) {
        failed("synthetic GPE method registration/enablement",
               ACPI_FAILURE(status) ? status : AE_ERROR);
        return false;
    }
    return true;
}

static void notified(ACPI_HANDLE device, UINT32 value, void *context) {
    (void)context;
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    /* A real AML Notify must be deferred to the notification task. */
    bool worker = sched_ready() && this_cpu()->current &&
                  !strcmp(this_cpu()->current->name, "acpi-notify");
    if (device != test_device || value != 0x80 || !worker || !(flags & 0x200))
        failed("Notify callback context", AE_ERROR);
    __atomic_add_fetch(&notifications, 1, __ATOMIC_RELEASE);
}

static bool trigger(void) {
    ACPI_STATUS status = AcpiEvaluateObject(test_device, "TRIG", NULL, NULL);
    if (ACPI_FAILURE(status))
        failed("AML Notify", status);
    return ACPI_SUCCESS(status);
}

void acpi_selftest_init(void) {
    enabled = requested();
    if (!enabled)
        return;
    pr_info("ACPICA SELFTEST: enabled (synthetic SSDT required)\n");
    ACPI_STATUS status = AcpiGetHandle(NULL, "\\_SB.MTST", &test_device);
    if (ACPI_FAILURE(status)) {
        failed("synthetic SSDT missing", status);
        return;
    }
    if (gpe_registration())
        pr_info("ACPICA SELFTEST PASS: GPE method registered and enabled\n");
    if (arithmetic() && package())
        pr_info("ACPICA SELFTEST PASS: boot AML arithmetic/package/mutex\n");

    test_page = pmm_alloc_pages(1);
    if (!test_page) {
        failed("owned test page allocation", AE_NO_MEMORY);
        return;
    }
    if (memory_field(0x1234abcd))
        pr_info("ACPICA SELFTEST PASS: boot SystemMemory owned-page field\n");

    for (uint64_t i = 0; i < boot_info.memory_map_count; ++i) {
        const struct boot_memory_range *range = &boot_info.memory_map[i];
        if (range->type == BOOT_MEMORY_ACPI_NVS && range->base && range->length) {
            nvs_address = range->base;
            break;
        }
    }
    if (nvs_field())
        pr_info("ACPICA SELFTEST PASS: boot firmware NVS read-only field\n");
    status = AcpiInstallNotifyHandler(test_device, ACPI_DEVICE_NOTIFY, notified, NULL);
    if (ACPI_FAILURE(status)) {
        failed("Notify handler install", status);
        return;
    }
    if (trigger() && !__atomic_load_n(&notifications, __ATOMIC_ACQUIRE))
        pr_info("ACPICA SELFTEST PASS: boot Notify queued before scheduler\n");
    else
        failed("Notify ran before scheduler", AE_ERROR);
    initialized = true;
}

static bool await_notifications(unsigned expected) {
    uint64_t start = tsc_uptime_us();
    while (__atomic_load_n(&notifications, __ATOMIC_ACQUIRE) < expected) {
        if (tsc_uptime_us() - start > 2000000) {
            failed("deferred Notify timeout", AE_TIME);
            return false;
        }
        sched_sleep(1000);
    }
    if (__atomic_load_n(&notifications, __ATOMIC_ACQUIRE) != expected) {
        failed("unexpected Notify count", AE_ERROR);
        return false;
    }
    return true;
}

static void mutex_peer_body(void *argument) {
    (void)argument;
    ACPI_OBJECT *result = evaluate("SLPT", NULL, 0);
    peer_result = result && result->Type == ACPI_TYPE_INTEGER ?
                  result->Integer.Value : 0;
    AcpiOsFree(result);
    __atomic_store_n(&peer_complete, true, __ATOMIC_RELEASE);
}

static void mutex_peer(void *argument) {
    power_transaction_begin();
    mutex_peer_body(argument);
    power_transaction_end();
}

static bool mutex_contention(void) {
    /* Both threads enter a non-serialized AML method that holds MTX0 over a
     * schedulable Sleep. This forces real interpreter mutex contention. */
    if (task_kernel("acpi-lock-peer", mutex_peer, NULL, 0) < 0) {
        failed("AML mutex peer allocation", AE_NO_MEMORY);
        return false;
    }
    ACPI_OBJECT *result = evaluate("SLPT", NULL, 0);
    UINT64 own_result = result && result->Type == ACPI_TYPE_INTEGER ?
                        result->Integer.Value : 0;
    AcpiOsFree(result);
    uint64_t start = tsc_uptime_us();
    while (!__atomic_load_n(&peer_complete, __ATOMIC_ACQUIRE)) {
        if (tsc_uptime_us() - start > 2000000) {
            failed("AML mutex contention timeout", AE_TIME);
            return false;
        }
        sched_sleep(1000);
    }
    if (!((own_result == 4 && peer_result == 5) ||
          (own_result == 5 && peer_result == 4))) {
        failed("AML mutex protected counter", AE_AML_OPERAND_VALUE);
        return false;
    }
    return true;
}

static void runtime_body(void *argument) {
    (void)argument;
    if (!sched_ready() || !this_cpu()->current || this_cpu()->current->idle) {
        failed("runtime scheduler context", AE_ERROR);
        return;
    }
    if (await_notifications(1))
        pr_info("ACPICA SELFTEST PASS: deferred Notify worker after sched_start\n");
    if (first_runtime_mmio_field())
        pr_info("ACPICA SELFTEST PASS: runtime-first literal MMIO preflight Field\n");
    if (arithmetic() && package() && memory_field(0x87654321) && nvs_field())
        pr_info("ACPICA SELFTEST PASS: runtime AML/mutex/SystemMemory/NVS\n");
    for (unsigned i = 1; i <= 3; ++i) {
        uint64_t start = tsc_uptime_us();
        ACPI_OBJECT *result = evaluate("SLPT", NULL, 0);
        uint64_t elapsed = tsc_uptime_us() - start;
        bool ok = is_integer(result, i) && elapsed >= 5050;
        AcpiOsFree(result);
        if (!ok)
            failed("AML Sleep/Stall/mutex", AE_AML_OPERAND_VALUE);
    }
    if (!__atomic_load_n(&failures, __ATOMIC_RELAXED))
        pr_info("ACPICA SELFTEST PASS: runtime Sleep/Stall/mutex repeated\n");
    if (mutex_contention())
        pr_info("ACPICA SELFTEST PASS: concurrent AML mutex contention\n");
    for (unsigned i = 2; i <= 4; ++i)
        if (!trigger() || !await_notifications(i))
            break;
    if (!__atomic_load_n(&failures, __ATOMIC_RELAXED)) {
        pr_info("ACPICA SELFTEST PASS: repeated deferred Notify (4 callbacks)\n");
        pr_info("ACPICA SELFTEST PASS: complete\n");
    } else {
        pr_err("ACPICA SELFTEST FAIL: complete (%u failures)\n",
               __atomic_load_n(&failures, __ATOMIC_RELAXED));
    }
    /* Keep the owned page for this boot: ACPICA can retain region mappings. */
}

static void runtime(void *argument) {
    power_transaction_begin();
    runtime_body(argument);
    power_transaction_end();
}

bool acpi_selftest_start(void) {
    if (!enabled || !initialized)
        return true;
    return task_kernel("acpi-selftest", runtime, NULL, 0) >= 0;
}
