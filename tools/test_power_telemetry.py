#!/usr/bin/env python3
"""ASan/UBSan tests of the production telemetry consumer and object parsers.

Includes the actual telemetry.c against vendored public ACPICA headers. Firmware
evaluation, namespace walking, scheduler and IRQ/spin boundaries are host mocks;
this is not an AML-interpreter, guest, physical battery or EC-handler test.
No ports, OS image, firmware, persistent cache, or guest builds are run.
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
#include <stdio.h>
struct task { unsigned cpu, power_role; void (*entry)(void *); void *argument; };
struct cpu_local { unsigned id; struct task *current; };
struct cpu_local *this_cpu(void);
bool sched_ready(void);
void sched_sleep(uint64_t);
struct task *task_allocate(const char *, bool, unsigned, int);
void task_publish(struct task *);
uint64_t tsc_uptime_us(void);
typedef struct { unsigned value; } spinlock_t;
static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->value, 1, __ATOMIC_ACQUIRE)) {}
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    __atomic_store_n(&lock->value, 0, __ATOMIC_RELEASE);
}
#endif
'''

SOURCE = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <acpica.h>
#include "TELEMETRY_SOURCE"

static _Thread_local struct cpu_local cpu;
static bool ready = true, allocation_fails;
static unsigned allocations, publications, sleeps, evaluations, installs;
static uint64_t now = 1000000;
static struct task allocated;
struct cpu_local *this_cpu(void) { return &cpu; }
bool sched_ready(void) { return ready; }
void sched_sleep(uint64_t n) { (void)n; sleeps++; }
uint64_t tsc_uptime_us(void) { return now; }
struct task *task_allocate(const char *name, bool user, unsigned parent, int target) {
    assert(!strcmp(name, "acpi-telemetry") && !user && !parent && !target);
    allocations++;
    return allocation_fails ? NULL : &allocated;
}
void task_publish(struct task *task) {
    assert(task == &allocated && task->power_role == POWER_ROLE_TELEMETRY && task->entry);
    publications++;
}
const char *AcpiFormatException(ACPI_STATUS status) {
    switch (status) {
    case AE_OK: return "AE_OK";
    case AE_NOT_FOUND: return "AE_NOT_FOUND";
    case AE_NOT_EXIST: return "AE_NOT_EXIST";
    case AE_BAD_DATA: return "AE_BAD_DATA";
    case AE_NO_HANDLER: return "AE_NO_HANDLER";
    case AE_BUFFER_OVERFLOW: return "AE_BUFFER_OVERFLOW";
    case AE_ALREADY_EXISTS: return "AE_ALREADY_EXISTS";
    case AE_NOT_CONFIGURED: return "AE_NOT_CONFIGURED";
    default: return "AE_ERROR";
    }
}

static struct telemetry_view scalar(union telemetry_result *b, uint64_t n) {
    memset(b, 0, sizeof(*b));
    b->alignment.Integer.Type = ACPI_TYPE_INTEGER;
    b->alignment.Integer.Value = n;
    return (struct telemetry_view){b->bytes, sizeof(ACPI_OBJECT)};
}
static ACPI_OBJECT *make_package(union telemetry_result *b, unsigned count) {
    assert(count < TELEMETRY_RETURN_BYTES / sizeof(ACPI_OBJECT) - 1);
    memset(b, 0, sizeof(*b));
    b->alignment.Package.Type = ACPI_TYPE_PACKAGE;
    b->alignment.Package.Count = count;
    b->alignment.Package.Elements = (ACPI_OBJECT *)(b->bytes + sizeof(ACPI_OBJECT));
    return b->alignment.Package.Elements;
}
static void number(ACPI_OBJECT *o, uint64_t n) {
    o->Integer.Type = ACPI_TYPE_INTEGER;
    o->Integer.Value = n;
}
static size_t put_string(union telemetry_result *b, ACPI_OBJECT *o, size_t at, const char *s) {
    size_t n = strlen(s);
    assert(at + n + 1 <= sizeof(b->bytes));
    o->String.Type = ACPI_TYPE_STRING;
    o->String.Length = n;
    o->String.Pointer = (char *)b->bytes + at;
    memcpy(o->String.Pointer, s, n + 1);
    return at + n + 1;
}
static struct telemetry_view information(union telemetry_result *b, bool bix, unsigned revision,
                                          unsigned unit, uint32_t full) {
    unsigned count = bix ? revision == 1 ? 21 : 20 : 13;
    unsigned ints = bix ? 16 : 9, u = bix ? 1 : 0;
    ACPI_OBJECT *p = make_package(b, count);
    for (unsigned i = 0; i < ints; i++) number(&p[i], 0);
    if (bix) number(&p[0], revision);
    number(&p[u], unit);
    number(&p[u+1], 6000);
    number(&p[u+2], full);
    number(&p[u+3], 1);
    number(&p[u+4], 12000);
    number(&p[u+5], 400);
    number(&p[u+6], 200);
    if (bix) {
        number(&p[9], 99000);
        number(&p[10], 1000);
        number(&p[11], 100);
        number(&p[12], 2000);
        number(&p[13], 1000);
    }
    size_t at = sizeof(ACPI_OBJECT) * (count + 1);
    at = put_string(b, &p[ints], at, "Fixture Battery");
    at = put_string(b, &p[ints+1], at, "serial");
    at = put_string(b, &p[ints+2], at, "Li-ion");
    at = put_string(b, &p[ints+3], at, "Fixture OEM");
    if (count == 21) number(&p[20], 2);
    return (struct telemetry_view){b->bytes, at};
}
static struct telemetry_view battery_status(union telemetry_result *b, uint32_t state,
                                            uint32_t rate, uint32_t remaining, uint32_t voltage) {
    ACPI_OBJECT *p = make_package(b, 4);
    number(&p[0], state); number(&p[1], rate); number(&p[2], remaining); number(&p[3], voltage);
    return (struct telemetry_view){b->bytes, sizeof(ACPI_OBJECT) * 5};
}

#define MODEL_NODES 1030
struct model {
    enum telemetry_kind kind;
    unsigned cid, revision, unit, psr;
    uint32_t sta, full, remaining, rate, voltage, state;
    ACPI_STATUS sta_error, bix_error, bif_error, bst_error, psr_error, notify_error, name_error;
    unsigned fail_return, calls[7];
    ACPI_NOTIFY_HANDLER callback;
    void *context;
};
static struct model models[MODEL_NODES];
static unsigned nodes;
static ACPI_STATUS walk_error;
static pthread_mutex_t block_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t block_condition = PTHREAD_COND_INITIALIZER;
static bool block_bst, entered, released;
static unsigned freeze_on_call;

ACPI_STATUS AcpiWalkNamespace(ACPI_OBJECT_TYPE type, ACPI_HANDLE root, UINT32 depth,
    ACPI_WALK_CALLBACK descending, ACPI_WALK_CALLBACK ascending, void *context, void **result) {
    assert(type == ACPI_TYPE_DEVICE && root == ACPI_ROOT_OBJECT && depth == UINT32_MAX);
    assert(descending && !ascending && !result && !cpu.id);
    if (walk_error) return walk_error;
    for (unsigned i = 0; i < nodes; i++) {
        ACPI_STATUS status = descending(&models[i], i, context, result);
        if (status == AE_CTRL_TERMINATE) return AE_OK;
        if (ACPI_FAILURE(status)) return status;
    }
    return AE_OK;
}
ACPI_STATUS AcpiGetName(ACPI_HANDLE handle, UINT32 type, ACPI_BUFFER *buffer) {
    struct model *m = handle;
    assert(type == ACPI_FULL_PATHNAME && buffer->Length == TELEMETRY_PATH_BYTES && !cpu.id);
    if (m->name_error) return m->name_error;
    int n = snprintf(buffer->Pointer, buffer->Length, "\\_SB.DEV%u", (unsigned)(m - models));
    buffer->Length = n + 1;
    return AE_OK;
}
ACPI_STATUS AcpiInstallNotifyHandler(ACPI_HANDLE handle, UINT32 type, ACPI_NOTIFY_HANDLER callback,
                                    void *context) {
    struct model *m = handle;
    assert(type == ACPI_ALL_NOTIFY && callback && context && !cpu.id);
    installs++;
    if (m->notify_error) return m->notify_error;
    assert(!m->callback);
    m->callback = callback; m->context = context;
    return AE_OK;
}
ACPI_STATUS AcpiEvaluateObject(ACPI_HANDLE handle, ACPI_STRING name, ACPI_OBJECT_LIST *args,
                              ACPI_BUFFER *buffer) {
    struct model *m = handle;
    assert(!args && !cpu.id && buffer->Pointer && buffer->Length == TELEMETRY_RETURN_BYTES);
    evaluations++;
    if (freeze_on_call == evaluations) {
        acpi_telemetry_freeze(77);
        assert(!acpi_telemetry_idle(77));
    }
    union telemetry_result *b = buffer->Pointer;
    struct telemetry_view view;
    ACPI_STATUS status = AE_OK;
    unsigned method;
    if (!strcmp(name, "_HID") || !strcmp(name, "_CID")) {
        bool cid = name[1] == 'C';
        method = cid ? 1 : 0;
        if (cid != !!m->cid || !m->kind) status = AE_NOT_FOUND;
        view = scalar(b, 0);
        if (m->cid == 2 && cid) {
            ACPI_OBJECT *p = make_package(b, 2);
            number(&p[0], 0x12345678); number(&p[1], 0x0a0cd041);
            view.length = sizeof(ACPI_OBJECT) * 3;
        } else if (m->kind == TELEMETRY_BATTERY) {
            view = scalar(b, 0x0a0cd041);
        } else {
            size_t at = put_string(b, &b->alignment, sizeof(ACPI_OBJECT), "ACPI0003");
            view.length = at;
        }
    } else if (!strcmp(name, "_STA")) {
        method = 2; status = m->sta_error; view = scalar(b, m->sta);
    } else if (!strcmp(name, "_PSR")) {
        method = 3; status = m->psr_error; view = scalar(b, m->psr);
    } else if (!strcmp(name, "_BIX")) {
        method = 4; status = m->bix_error;
        view = information(b, true, m->revision, m->unit, m->full);
    } else if (!strcmp(name, "_BIF")) {
        method = 5; status = m->bif_error;
        view = information(b, false, 0, m->unit, m->full);
    } else {
        assert(!strcmp(name, "_BST")); method = 6; status = m->bst_error;
        pthread_mutex_lock(&block_mutex);
        if (block_bst) {
            entered = true;
            pthread_cond_broadcast(&block_condition);
            while (!released) pthread_cond_wait(&block_condition, &block_mutex);
        }
        pthread_mutex_unlock(&block_mutex);
        view = battery_status(b, m->state, m->rate, m->remaining, m->voltage);
    }
    m->calls[method]++;
    buffer->Length = view.length;
    if (m->fail_return == 1) buffer->Length = 0;
    if (m->fail_return == 2) buffer->Length = TELEMETRY_RETURN_BYTES + 1;
    if (m->fail_return == 3) buffer->Pointer = NULL;
    if (m->fail_return == 4) return AE_BUFFER_OVERFLOW;
    return status;
}

static void reset(void) {
    assert(!busy && !telemetry_lock.value);
    memset(devices, 0, sizeof(devices)); memset(models, 0, sizeof(models));
    device_count = visited = overflow = discovery_errors = 0;
    discovery_status = discovery_last_error = 0;
    discovered = busy = scan_truncated = started = false;
    frozen_epoch = last_epoch = 0; walk_error = 0;
    allocations = publications = sleeps = evaluations = installs = 0;
    freeze_on_call = 0; block_bst = entered = released = false;
    ready = true; allocation_fails = false; cpu.id = 0; cpu.current = &allocated; nodes = 0; now = 1000000;
    memset(&allocated, 0, sizeof(allocated));
}
static struct model *add(enum telemetry_kind kind) {
    struct model *m = &models[nodes++];
    m->kind = kind; m->sta = kind == TELEMETRY_AC ? 0xf : 0x1f;
    m->psr = 1; m->revision = 1; m->full = 5000; m->remaining = 2500;
    m->rate = 1000; m->voltage = 12000; m->state = 1;
    return m;
}

static void parser_tests(void) {
    union telemetry_result b;
    struct telemetry_view v;
    struct telemetry_sample s = {0};
    uint32_t n;
    v = scalar(&b, UINT32_MAX); assert(parse_integer(&v, &n) == AE_OK && n == UINT32_MAX);
    b.alignment.Integer.Value = UINT64_MAX; assert(ACPI_FAILURE(parse_integer(&v, &n)));
    v = scalar(&b, 1);
    for (size_t length = 0; length < sizeof(ACPI_OBJECT); length++) {
        v.length = length; assert(ACPI_FAILURE(parse_integer(&v, &n)));
    }
    for (unsigned bix = 0; bix < 2; bix++) for (unsigned revision = 0; revision < 2; revision++) {
        v = information(&b, bix, revision, revision, 5000);
        assert(parse_information(&v, bix, &s) == AE_OK && s.full == 5000 && s.unit == revision);
        assert(!strcmp(s.model, "Fixture Battery"));
        size_t length = v.length;
        for (v.length = 0; v.length < length; v.length++)
            assert(ACPI_FAILURE(parse_information(&v, bix, &s)));
        v.length = length;
        unsigned count = b.alignment.Package.Count;
        ACPI_OBJECT *p = b.alignment.Package.Elements;
        for (unsigned i = 0; i < count; i++) {
            ACPI_OBJECT old = p[i];
            p[i].Type = ACPI_TYPE_BUFFER;
            assert(ACPI_FAILURE(parse_information(&v, bix, &s))); p[i] = old;
        }
        for (unsigned i = 0; i < (bix ? 16u : 9u); i++) {
            uint64_t old = p[i].Integer.Value;
            p[i].Integer.Value = UINT64_MAX;
            assert(ACPI_FAILURE(parse_information(&v, bix, &s))); p[i].Integer.Value = old;
        }
        for (unsigned i = bix ? 16 : 9; i < (bix ? 20u : 13u); i++) {
            ACPI_OBJECT old = p[i];
            char first = old.String.Pointer[0];
            p[i].String.Pointer = (char *)(uintptr_t)1;
            assert(ACPI_FAILURE(parse_information(&v, bix, &s))); p[i] = old;
            p[i].String.Length = UINT32_MAX;
            assert(ACPI_FAILURE(parse_information(&v, bix, &s))); p[i] = old;
            p[i].String.Pointer[p[i].String.Length] = 'x';
            assert(ACPI_FAILURE(parse_information(&v, bix, &s)));
            p[i].String.Pointer[p[i].String.Length] = 0;
            p[i].String.Pointer[0] = '\n';
            assert(ACPI_FAILURE(parse_information(&v, bix, &s)));
            p[i].String.Pointer[0] = first;
        }
        b.alignment.Package.Count = count + 1;
        assert(ACPI_FAILURE(parse_information(&v, bix, &s)));
    }
    v = information(&b, true, 2, 0, 5000); assert(ACPI_FAILURE(parse_information(&v, true, &s)));
    v = information(&b, true, 0, 2, 5000); assert(ACPI_FAILURE(parse_information(&v, true, &s)));
    v = information(&b, false, 0, 0, 0x80000000); assert(ACPI_FAILURE(parse_information(&v, false, &s)));
    v = information(&b, false, 0, 1, UINT32_MAX); assert(parse_information(&v, false, &s) == AE_OK);
    const struct { unsigned field; uint32_t value; } invalid[] = {
        {0,2}, {1,2}, {2,0x80000000}, {3,0x80000000}, {4,2}, {5,0x80000000},
        {6,0x80000000}, {7,401}, {9,100001}, {11,1001}, {13,2001}, {20,3},
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        v = information(&b, true, 1, 0, 5000);
        number(&b.alignment.Package.Elements[invalid[i].field], invalid[i].value);
        assert(ACPI_FAILURE(parse_information(&v, true, &s)));
    }
    v = battery_status(&b, 9, UINT32_MAX, UINT32_MAX, UINT32_MAX);
    assert(parse_status(&v, &s) == AE_OK && s.state == 9);
    for (unsigned bit = 4; bit < 32; bit++) {
        number(&b.alignment.Package.Elements[0], 1u << bit);
        assert(ACPI_FAILURE(parse_status(&v, &s)));
    }
    number(&b.alignment.Package.Elements[0], 3); assert(ACPI_FAILURE(parse_status(&v, &s)));
    for (unsigned i = 1; i < 4; i++) {
        v = battery_status(&b, 1, 1000, 2500, 12000);
        number(&b.alignment.Package.Elements[i], 0x80000000);
        assert(ACPI_FAILURE(parse_status(&v, &s)));
    }
    v = battery_status(&b, 1, 1000, 2500, 12000);
    ACPI_OBJECT *good = b.alignment.Package.Elements;
    const uintptr_t pointers[] = {0, 1, UINTPTR_MAX, (uintptr_t)b.bytes,
        (uintptr_t)b.bytes + 1, (uintptr_t)b.bytes + sizeof(b) - 1, (uintptr_t)b.bytes + sizeof(b)};
    for (unsigned i = 0; i < sizeof(pointers) / sizeof(*pointers); i++) {
        b.alignment.Package.Elements = (ACPI_OBJECT *)pointers[i];
        assert(ACPI_FAILURE(parse_status(&v, &s)));
    }
    b.alignment.Package.Elements = good;
    for (unsigned count = 0; count < 1000; count++) {
        b.alignment.Package.Count = count;
        if (count != 4) assert(ACPI_FAILURE(parse_status(&v, &s)));
    }
    s = (struct telemetry_sample){.info_status=AE_OK,.bst_status=AE_OK,.full=5000,.remaining=2500};
    percentage(&s); assert(s.percent_known && s.percent == 50);
    s.unit = 1; percentage(&s); assert(s.percent_known && s.percent == 50);
    s.full = UINT32_MAX; percentage(&s); assert(!s.percent_known);
    s.full = 0; percentage(&s); assert(!s.percent_known);
    s.full = 5000; s.remaining = 5001; percentage(&s); assert(!s.percent_known);
    s.remaining = UINT32_MAX; percentage(&s); assert(!s.percent_known);
    s.remaining = 0; percentage(&s); assert(s.percent_known && !s.percent);
    s.remaining = s.full = INT32_MAX; percentage(&s); assert(s.percent_known && s.percent == 100);
    s.info_status = AE_NO_HANDLER; percentage(&s); assert(!s.percent_known);
    /* Deterministic malformed external-object corpus. Dereferencing an invalid
       firmware pointer must never precede the range/alignment/type checks. */
    uint32_t random = 0x5f373e01;
    for (unsigned iteration = 0; iteration < 10000; iteration++) {
        for (unsigned i = 0; i < sizeof(b.bytes); i++) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            b.bytes[i] = (unsigned char)random;
        }
        b.alignment.Type = iteration & 1 ? ACPI_TYPE_PACKAGE : ACPI_TYPE_INTEGER;
        v = (struct telemetry_view){b.bytes, iteration % (sizeof(b.bytes) + 1)};
        if (b.alignment.Type == ACPI_TYPE_PACKAGE) {
            b.alignment.Package.Count = iteration % 24;
            b.alignment.Package.Elements = (ACPI_OBJECT *)(b.bytes + (iteration % sizeof(b.bytes)));
        }
        (void)parse_information(&v, true, &s);
        (void)parse_status(&v, &s);
        (void)parse_integer(&v, &n);
    }
}

static void service_tests(void) {
    reset(); ready = false; assert(acpi_telemetry_start());
    assert(allocations == 1 && publications == 1 && !evaluations);
    reset(); ready = true;
    cpu.id = 1; assert(!acpi_telemetry_start()); cpu.id = 0;
    allocation_fails = true; assert(!acpi_telemetry_start()); allocation_fails = false;
    assert(acpi_telemetry_start() && acpi_telemetry_start());
    assert(allocations == 2 && publications == 1 && !evaluations);
    struct model *ac = add(TELEMETRY_AC), *bat = add(TELEMETRY_BATTERY);
    add(TELEMETRY_BATTERY)->sta = 0xf; /* Battery bay present, battery absent. */
    add(TELEMETRY_BATTERY)->sta_error = AE_NO_HANDLER;
    add(TELEMETRY_BATTERY)->bix_error = AE_NOT_FOUND;
    add(TELEMETRY_BATTERY)->revision = 3; /* Invalid _BIX, valid fallback. */
    add(TELEMETRY_BATTERY)->cid = 2;
    add(TELEMETRY_AC)->psr = 2;
    ac->sta_error = AE_NOT_FOUND;
    telemetry_step();
    assert(discovered && device_count == 8 && installs == 8);
    assert(devices[0].sample.complete && devices[0].sample.online);
    assert(devices[1].sample.complete && devices[1].sample.percent == 50);
    assert(devices[1].sample.from_bix && !bat->calls[5]);
    assert(!devices[2].sample.present && devices[2].sample.complete && !models[2].calls[4] && !models[2].calls[6]);
    assert(devices[3].sample.sta_status == AE_NO_HANDLER && !models[3].calls[4]);
    assert(devices[4].sample.complete && !devices[4].sample.from_bix && models[4].calls[5]);
    assert(devices[5].sample.complete && devices[5].sample.bix_status == AE_BAD_DATA);
    assert(devices[6].kind == TELEMETRY_BATTERY && devices[6].sample.complete);
    assert(devices[7].sample.psr_status == AE_BAD_DATA && !devices[7].sample.complete);
    unsigned before = evaluations;
    for (unsigned i = 0; i < 100; i++) bat->callback(bat, 0x80, bat->context);
    assert(evaluations == before && devices[1].notifications == 100 && devices[1].coalesced == 99);
    telemetry_step(); assert(evaluations == before);
    now += TELEMETRY_COALESCE_US; telemetry_step();
    assert(evaluations == before + 3 && !devices[1].dirty);
    char output[16384]; cpu.id = 1; before = evaluations;
    size_t used = acpi_telemetry_info(output, sizeof(output));
    assert(used == strlen(output) && evaluations == before && !sleeps);
    assert(strstr(output, "percent=50") && strstr(output, "AE_NO_HANDLER") && strstr(output, "source=_BIF"));
    assert(strstr(output, "unit=mWh/mW") && strstr(output, "present=no") && strstr(output, "online=yes"));
    assert(!acpi_telemetry_info(NULL, 10) && !acpi_telemetry_info(output, 0));
    for (size_t cap = 1; cap < 256; cap++) {
        unsigned char bytes[258]; memset(bytes, 0xa5, sizeof(bytes));
        size_t n = acpi_telemetry_info((char *)bytes + 1, cap);
        assert(n < cap && bytes[0] == 0xa5 && bytes[cap+1] == 0xa5 && !bytes[n+1]);
        if (cap > 31) assert(strstr((char *)bytes+1, "telemetry_output_truncated=1"));
    }
    cpu.id = 0; now += TELEMETRY_STALE_US + 1;
    acpi_telemetry_info(output, sizeof(output)); assert(strstr(output, "stale=1"));
    acpi_telemetry_freeze(1); assert(acpi_telemetry_idle(1));
    before = evaluations; telemetry_step(); assert(evaluations == before);
    acpi_telemetry_thaw(2); assert(acpi_telemetry_idle(1));
    acpi_telemetry_freeze(2); assert(!acpi_telemetry_idle(1) && acpi_telemetry_idle(2));
    acpi_telemetry_thaw(1); assert(acpi_telemetry_idle(2));
    acpi_telemetry_thaw(2); assert(!acpi_telemetry_idle(2));
    acpi_telemetry_freeze(2); assert(!acpi_telemetry_idle(2)); /* Stale replay. */
    telemetry_step(); assert(evaluations > before);
}

static void failure_and_limit_tests(void) {
    char output[4096];
    reset(); nodes = TELEMETRY_SCAN_LIMIT + 1; telemetry_step();
    assert(visited == TELEMETRY_SCAN_LIMIT && scan_truncated && !device_count);
    reset(); for (unsigned i = 0; i < 20; i++) add(TELEMETRY_BATTERY);
    telemetry_step(); assert(device_count == TELEMETRY_DEVICES && overflow == 8 && installs == TELEMETRY_DEVICES);
    acpi_telemetry_info(output, sizeof(output)); assert(strstr(output, "overflow=8"));
    reset(); walk_error = AE_NO_NAMESPACE; telemetry_step();
    acpi_telemetry_info(output, sizeof(output)); assert(strstr(output, "discovery=failed"));
    for (unsigned fault = 1; fault <= 4; fault++) {
        reset(); add(TELEMETRY_BATTERY)->fail_return = fault; telemetry_step();
        assert(!device_count && discovery_errors == (fault == 4 ? 2u : 1u));
    }
    reset(); struct model *m = add(TELEMETRY_BATTERY);
    m->notify_error = AE_ALREADY_EXISTS; m->name_error = AE_BUFFER_OVERFLOW;
    m->bix_error = m->bif_error = m->bst_error = AE_NO_HANDLER;
    telemetry_step(); assert(!devices[0].sample.complete && !devices[0].sample.percent_known);
    assert(devices[0].notify_status == AE_ALREADY_EXISTS && !strcmp(devices[0].path, "device-0"));
    acpi_telemetry_info(output, sizeof(output));
    assert(strstr(output, "unit=unknown") && strstr(output, "percent=unknown") && strstr(output, "rate=unknown"));
    now += TELEMETRY_POLL_US; unsigned before = evaluations; telemetry_step();
    assert(evaluations > before); /* Notification failure retains polling. */
    reset(); m = add(TELEMETRY_BATTERY); m->full = m->remaining = m->rate = m->voltage = UINT32_MAX;
    telemetry_step(); assert(devices[0].sample.complete && !devices[0].sample.percent_known);
    acpi_telemetry_info(output, sizeof(output)); assert(strstr(output, "full=unknown remaining=unknown rate=unknown voltage_mV=unknown percent=unknown"));
    reset(); m = add(TELEMETRY_BATTERY); m->unit = 1; m->revision = 0;
    telemetry_step(); assert(devices[0].sample.complete && devices[0].sample.percent == 50);
    acpi_telemetry_info(output, sizeof(output)); assert(strstr(output, "unit=mAh/mA"));
    for (unsigned at = 1; at <= 4; at++) {
        reset(); add(TELEMETRY_BATTERY); freeze_on_call = at; telemetry_step();
        assert(acpi_telemetry_idle(77) && evaluations == at);
        acpi_telemetry_thaw(77); now += TELEMETRY_POLL_US; telemetry_step();
        assert(discovered && device_count == 1 && devices[0].sample.complete && installs == 1);
    }
}

static void *run_step(void *arg) { (void)arg; telemetry_step(); return NULL; }
static void in_flight_test(void) {
    reset(); add(TELEMETRY_BATTERY); block_bst = true;
    pthread_t thread; assert(!pthread_create(&thread, NULL, run_step, NULL));
    pthread_mutex_lock(&block_mutex);
    while (!entered) pthread_cond_wait(&block_condition, &block_mutex);
    pthread_mutex_unlock(&block_mutex);
    acpi_telemetry_freeze(5);
    for (unsigned i = 0; i < 100; i++) {
        assert(!acpi_telemetry_idle(5));
        char out[1024]; acpi_telemetry_info(out, sizeof(out)); assert(strstr(out, "active=1"));
        telemetry_step(); /* Closed admission does not start another evaluation. */
    }
    acpi_telemetry_thaw(4); assert(!acpi_telemetry_idle(5));
    pthread_mutex_lock(&block_mutex); released = true;
    pthread_cond_broadcast(&block_condition); pthread_mutex_unlock(&block_mutex);
    assert(!pthread_join(thread, NULL)); assert(acpi_telemetry_idle(5));
    assert(devices[0].sample.complete); /* Already active result may finish. */
    acpi_telemetry_thaw(5); assert(!acpi_telemetry_idle(5));
}

int main(void) {
    parser_tests(); service_tests(); failure_and_limit_tests(); in_flight_test();
    puts("PASS actual ACPI telemetry C: fixed-buffer pointer/type/package/revision/unit/sentinel bounds, "
         "BIX/BIF fallback, AC/absent/error batteries, percentages, notification coalescing, "
         "polling, scan/device overflow, AP snapshot-only reads, stale/truncated status, "
         "epoch freeze/thaw and blocked in-flight evaluation (ASan/UBSan; mocked firmware, no hardware claim)");
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="coast-power-telemetry-") as temporary:
        folder = Path(temporary)
        include = folder / "include"
        include.mkdir()
        (include / "test_kernel.h").write_text(KERNEL)
        for name in ("sched.h", "sync.h", "tsc.h"):
            (include / name).write_text('#include "test_kernel.h"\n')
        (include / "acpica.h").write_text('#include <acpi.h>\n')
        (include / "power.h").write_text((ROOT / "include/power.h").read_text())
        (include / "acpi").mkdir()
        (include / "acpi/telemetry.h").write_text((ROOT / "include/acpi/telemetry.h").read_text())
        source = folder / "test.c"
        source.write_text(SOURCE.replace("TELEMETRY_SOURCE", str(ROOT / "drivers/acpi/telemetry.c")))
        binary = folder / "test"
        compiler = shlex.split(os.environ.get("HOSTCC", "cc"))
        subprocess.run(compiler + ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                  "-D__COAST__", "-fsanitize=address,undefined", "-pthread",
                                  "-fno-omit-frame-pointer", "-I" + str(include),
                                  "-I" + str(ROOT / "lib/acpica/source/include"),
                                  str(source), "-o", str(binary)], check=True)
        environment = os.environ.copy()
        environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
        subprocess.run([str(binary)], check=True, timeout=30, env=environment)


if __name__ == "__main__":
    main()
