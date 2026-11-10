/* Read-only control-method battery/AC telemetry, not a power policy driver.
 * Namespace IDs are discovered without AcpiGetDevices' implicit _STA filter,
 * so an absent battery bay or a failing _STA remains visible. Every external
 * object is copied into and validated inside a fixed 4 KiB return buffer.
 * Public methods described in ACPI 6.5 chapter 10:
 * https://uefi.org/specs/ACPI/6.5/10_Power_Source_and_Power_Meter_Devices.html
 */
#include <acpica.h>
#include <acpi/telemetry.h>
#include <power.h>
#include <sched.h>
#include <sync.h>
#include <string.h>
#include <tsc.h>
#include <stdarg.h>

#define TELEMETRY_DEVICES 12u
#define TELEMETRY_SCAN_LIMIT 1024u
#define TELEMETRY_CIDS 16u
#define TELEMETRY_RETURN_BYTES 4096u
#define TELEMETRY_STRING_BYTES 64u
#define TELEMETRY_PATH_BYTES 128u
#define TELEMETRY_POLL_US 10000000ull
#define TELEMETRY_STALE_US 30000000ull
#define TELEMETRY_COALESCE_US 1000000ull
#define TELEMETRY_UNKNOWN UINT32_MAX

enum telemetry_kind { TELEMETRY_OTHER, TELEMETRY_AC, TELEMETRY_BATTERY };
/* Internal, never returned by ACPICA. A frozen pass is abandoned, not posted
 * as a firmware failure and certainly not described as canceled AML. */
#define TELEMETRY_FROZEN ((ACPI_STATUS)0xffff0001u)

union telemetry_result {
    ACPI_OBJECT alignment;
    unsigned char bytes[TELEMETRY_RETURN_BYTES];
};
struct telemetry_view { const unsigned char *base; size_t length; };
struct telemetry_sample {
    uint64_t attempted_us, successful_us;
    ACPI_STATUS sta_status, info_status, bix_status, bst_status, psr_status;
    uint32_t sta, unit, design, full, design_voltage, state, rate, remaining, voltage;
    unsigned percent;
    bool attempted, complete, present, online, percent_known, from_bix, sta_default;
    char model[TELEMETRY_STRING_BYTES];
};
struct telemetry_device {
    ACPI_HANDLE handle;
    enum telemetry_kind kind;
    char path[TELEMETRY_PATH_BYTES];
    ACPI_STATUS path_status, notify_status;
    bool dirty;
    uint64_t notifications, coalesced;
    uint32_t last_notify;
    struct telemetry_sample sample;
};
static spinlock_t telemetry_lock;
static struct telemetry_device devices[TELEMETRY_DEVICES];
static unsigned device_count, visited, overflow, discovery_errors;
static ACPI_STATUS discovery_status, discovery_last_error;
static bool started, discovered, busy, scan_truncated;
static uint64_t frozen_epoch, last_epoch;

static bool frozen(void) {
    uint64_t f = spin_lock_irqsave(&telemetry_lock);
    bool value = frozen_epoch != 0;
    spin_unlock_irqrestore(&telemetry_lock, f);
    return value;
}

static bool within(const struct telemetry_view *v, const void *p, size_t n) {
    uintptr_t base = (uintptr_t)v->base, at = (uintptr_t)p;
    return p && at >= base && at - base <= v->length && n <= v->length - (at - base);
}

static const ACPI_OBJECT *root_object(const struct telemetry_view *v) {
    return within(v, v->base, sizeof(ACPI_OBJECT)) &&
           !((uintptr_t)v->base % _Alignof(ACPI_OBJECT)) ?
           (const ACPI_OBJECT *)v->base : NULL;
}

static const ACPI_OBJECT *package(const struct telemetry_view *v, unsigned n) {
    const ACPI_OBJECT *o = root_object(v);
    if (!o || o->Type != ACPI_TYPE_PACKAGE || o->Package.Count != n ||
        !n || n > TELEMETRY_RETURN_BYTES / sizeof(ACPI_OBJECT))
        return NULL;
    const ACPI_OBJECT *p = o->Package.Elements;
    if (!within(v, p, n * sizeof(*p)) ||
        (uintptr_t)p % _Alignof(ACPI_OBJECT) ||
        (uintptr_t)p - (uintptr_t)v->base < sizeof(*o))
        return NULL;
    return p;
}

static bool integer32(const ACPI_OBJECT *o, uint32_t *value) {
    if (o->Type != ACPI_TYPE_INTEGER || o->Integer.Value > UINT32_MAX)
        return false;
    *value = (uint32_t)o->Integer.Value;
    return true;
}

/* ASCIIZ length excludes the terminator. Reject embedded NUL, controls and
 * overlong strings instead of exposing unsanitized firmware text to the UI. */
static bool string_value(const struct telemetry_view *v, const ACPI_OBJECT *o,
                         char *out, size_t cap) {
    if (o->Type != ACPI_TYPE_STRING || o->String.Length >= cap ||
        !within(v, o->String.Pointer, (size_t)o->String.Length + 1))
        return false;
    for (unsigned i = 0; i < o->String.Length; i++) {
        unsigned char c = o->String.Pointer[i];
        if (c < 0x20 || c > 0x7e)
            return false;
    }
    if (o->String.Pointer[o->String.Length])
        return false;
    if (out)
        memcpy(out, o->String.Pointer, (size_t)o->String.Length + 1);
    return true;
}

static bool quantity(uint32_t value) {
    return value <= INT32_MAX || value == TELEMETRY_UNKNOWN;
}

static ACPI_STATUS parse_integer(const struct telemetry_view *v, uint32_t *value) {
    const ACPI_OBJECT *o = root_object(v);
    return o && integer32(o, value) ? AE_OK : AE_BAD_DATA;
}

static ACPI_STATUS parse_information(const struct telemetry_view *v, bool bix,
                                     struct telemetry_sample *s) {
    const ACPI_OBJECT *o = root_object(v);
    unsigned count = bix ? 20 : 13, ints = bix ? 16 : 9;
    if (!o || o->Type != ACPI_TYPE_PACKAGE)
        return AE_BAD_DATA;
    if (bix && o->Package.Count == 21)
        count = 21;
    const ACPI_OBJECT *p = package(v, count);
    uint32_t a[21] = {0};
    if (!p)
        return AE_BAD_DATA;
    for (unsigned i = 0; i < ints; i++)
        if (!integer32(&p[i], &a[i]))
            return AE_BAD_DATA;
    for (unsigned i = ints; i < ints + 4; i++)
        if (!string_value(v, &p[i], NULL, TELEMETRY_STRING_BYTES))
            return AE_BAD_DATA;
    if (bix && ((a[0] != 0 && a[0] != 1) ||
                count != (a[0] ? 21u : 20u)))
        return AE_BAD_DATA;
    if (count == 21 && (!integer32(&p[20], &a[20]) || a[20] > 2))
        return AE_BAD_DATA;
    unsigned u = bix ? 1 : 0;
    if (a[u] > 1 || a[u + 3] > 1 ||
        !quantity(a[u + 1]) || !quantity(a[u + 2]) || !quantity(a[u + 4]) ||
        a[u + 5] > INT32_MAX || a[u + 6] > INT32_MAX || a[u + 6] > a[u + 5])
        return AE_BAD_DATA;
    if (bix && (a[9] > 100000 ||
                (a[10] != TELEMETRY_UNKNOWN && a[11] != TELEMETRY_UNKNOWN && a[11] > a[10]) ||
                (a[12] != TELEMETRY_UNKNOWN && a[13] != TELEMETRY_UNKNOWN && a[13] > a[12])))
        return AE_BAD_DATA;
    s->unit = a[u];
    s->design = a[u + 1];
    s->full = a[u + 2];
    s->design_voltage = a[u + 4];
    s->from_bix = bix;
    string_value(v, &p[ints], s->model, sizeof(s->model));
    return AE_OK;
}

static ACPI_STATUS parse_status(const struct telemetry_view *v,
                                struct telemetry_sample *s) {
    const ACPI_OBJECT *p = package(v, 4);
    uint32_t a[4];
    if (!p)
        return AE_BAD_DATA;
    for (unsigned i = 0; i < 4; i++)
        if (!integer32(&p[i], &a[i]))
            return AE_BAD_DATA;
    /* ACPI 6.5 also defines bit 3, charge limiting. Charging and discharging
     * are mutually exclusive. Unknown is permitted only for quantities. */
    if ((a[0] & ~15u) || (a[0] & 3u) == 3u ||
        !quantity(a[1]) || !quantity(a[2]) || !quantity(a[3]))
        return AE_BAD_DATA;
    s->state = a[0];
    s->rate = a[1];
    s->remaining = a[2];
    s->voltage = a[3];
    return AE_OK;
}

static void percentage(struct telemetry_sample *s) {
    s->percent_known = ACPI_SUCCESS(s->info_status) && ACPI_SUCCESS(s->bst_status) &&
        s->unit <= 1 && s->full != TELEMETRY_UNKNOWN && s->full &&
        s->remaining != TELEMETRY_UNKNOWN && s->remaining <= s->full;
    if (s->percent_known)
        s->percent = (unsigned)((uint64_t)s->remaining * 100 / s->full);
}

static ACPI_STATUS evaluate(ACPI_HANDLE handle, const char *name,
                            union telemetry_result *storage, struct telemetry_view *v) {
    /* busy is held for the entire worker pass. The same spinlock orders this
     * final admission check against freeze; BSP0 serializes the tiny interval
     * between unlocking and entering ACPICA. Freeze cannot run there unless
     * this task is preempted; in that case busy still forbids idle. */
    if (frozen())
        return TELEMETRY_FROZEN;
    ACPI_BUFFER b = {sizeof(storage->bytes), storage->bytes};
    ACPI_STATUS status = AcpiEvaluateObject(handle, (char *)name, NULL, &b);
    if (ACPI_FAILURE(status))
        return status;
    if (b.Pointer != storage->bytes || b.Length > sizeof(storage->bytes) ||
        b.Length < sizeof(ACPI_OBJECT))
        return AE_BAD_DATA;
    *v = (struct telemetry_view){storage->bytes, b.Length};
    return AE_OK;
}

static ACPI_STATUS read_integer(ACPI_HANDLE h, const char *name, uint32_t *value) {
    union telemetry_result b;
    struct telemetry_view v;
    ACPI_STATUS status = evaluate(h, name, &b, &v);
    return ACPI_SUCCESS(status) ? parse_integer(&v, value) : status;
}

static enum telemetry_kind id_kind(const struct telemetry_view *v,
                                   const ACPI_OBJECT *o, bool *valid) {
    if (o->Type == ACPI_TYPE_INTEGER) {
        uint32_t n;
        *valid = integer32(o, &n);
        return *valid && n == 0x0a0cd041u ? TELEMETRY_BATTERY : TELEMETRY_OTHER;
    }
    char id[TELEMETRY_STRING_BYTES];
    *valid = string_value(v, o, id, sizeof(id));
    if (!*valid)
        return TELEMETRY_OTHER;
    return !strcmp(id, "ACPI0003") ? TELEMETRY_AC :
           !strcmp(id, "PNP0C0A") ? TELEMETRY_BATTERY : TELEMETRY_OTHER;
}

static ACPI_STATUS read_id(ACPI_HANDLE h, bool cid, enum telemetry_kind *kind) {
    union telemetry_result b;
    struct telemetry_view v;
    ACPI_STATUS status = evaluate(h, cid ? "_CID" : "_HID", &b, &v);
    if (ACPI_FAILURE(status))
        return status;
    const ACPI_OBJECT *o = root_object(&v), *p = o;
    unsigned count = 1;
    if (cid && o->Type == ACPI_TYPE_PACKAGE) {
        count = o->Package.Count;
        if (count > TELEMETRY_CIDS || !(p = package(&v, count)))
            return AE_BAD_DATA;
    }
    *kind = TELEMETRY_OTHER;
    for (unsigned i = 0; i < count; i++) {
        bool valid;
        enum telemetry_kind k = id_kind(&v, &p[i], &valid);
        if (!valid || (k && *kind && k != *kind))
            return AE_BAD_DATA;
        if (k)
            *kind = k;
    }
    return AE_OK;
}

static void notify(ACPI_HANDLE handle, UINT32 value, void *context) {
    struct telemetry_device *d = context;
    (void)handle;
    uint64_t f = spin_lock_irqsave(&telemetry_lock);
    if (d->notifications != UINT64_MAX)
        d->notifications++;
    if (d->dirty && d->coalesced != UINT64_MAX)
        d->coalesced++;
    d->dirty = true;
    d->last_notify = value;
    spin_unlock_irqrestore(&telemetry_lock, f);
}

struct discovery_pass { unsigned visited, overflow, errors; bool interrupted, truncated; ACPI_STATUS last; };
static ACPI_STATUS discover_device(ACPI_HANDLE h, UINT32 depth, void *context, void **result) {
    struct discovery_pass *pass = context;
    (void)depth;
    (void)result;
    if (frozen()) {
        pass->interrupted = true;
        return AE_CTRL_TERMINATE;
    }
    if (pass->visited == TELEMETRY_SCAN_LIMIT) {
        pass->truncated = true;
        return AE_CTRL_TERMINATE;
    }
    pass->visited++;
    enum telemetry_kind kind = TELEMETRY_OTHER;
    ACPI_STATUS status = read_id(h, false, &kind);
    if (status == TELEMETRY_FROZEN)
        goto interrupted;
    if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
        pass->errors++;
        pass->last = status;
    }
    if (!kind) {
        status = read_id(h, true, &kind);
        if (status == TELEMETRY_FROZEN)
            goto interrupted;
        if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
            pass->errors++;
            pass->last = status;
        }
        if (ACPI_FAILURE(status))
            kind = TELEMETRY_OTHER;
    }
    if (!kind)
        return AE_OK;
    if (frozen())
        goto interrupted;
    uint64_t f = spin_lock_irqsave(&telemetry_lock);
    for (unsigned i = 0; i < device_count; i++) {
        if (devices[i].handle == h) {
            spin_unlock_irqrestore(&telemetry_lock, f);
            return AE_OK;
        }
    }
    if (device_count == TELEMETRY_DEVICES) {
        pass->overflow++;
        spin_unlock_irqrestore(&telemetry_lock, f);
        return AE_OK;
    }
    unsigned at = device_count++;
    struct telemetry_device *d = &devices[at];
    d->handle = h;
    d->kind = kind;
    d->dirty = true;
    d->path_status = d->notify_status = AE_NOT_EXIST;
    /* Handle and slot never move or die during this boot. */
    spin_unlock_irqrestore(&telemetry_lock, f);
    char path[TELEMETRY_PATH_BYTES] = {0};
    ACPI_BUFFER name = {sizeof(path), path};
    ACPI_STATUS path_status = AcpiGetName(h, ACPI_FULL_PATHNAME, &name);
    if (ACPI_SUCCESS(path_status) &&
        (name.Pointer != path || !name.Length || name.Length > sizeof(path) ||
         !memchr(path, 0, name.Length)))
        path_status = AE_BAD_DATA;
    if (ACPI_SUCCESS(path_status))
        for (size_t i = 0; path[i]; i++)
            if ((unsigned char)path[i] < 0x20 || (unsigned char)path[i] > 0x7e) {
                path_status = AE_BAD_DATA;
                break;
            }
    ACPI_STATUS notify_status = AcpiInstallNotifyHandler(h, ACPI_ALL_NOTIFY, notify, d);
    f = spin_lock_irqsave(&telemetry_lock);
    if (ACPI_SUCCESS(path_status))
        memcpy(d->path, path, sizeof(path));
    else
        snprintf(d->path, sizeof(d->path), "device-%u", at);
    d->path_status = path_status;
    d->notify_status = notify_status;
    spin_unlock_irqrestore(&telemetry_lock, f);
    return AE_OK;
interrupted:
    pass->interrupted = true;
    return AE_CTRL_TERMINATE;
}

static bool refresh(ACPI_HANDLE h, enum telemetry_kind kind,
                     struct telemetry_sample *s) {
    memset(s, 0, sizeof(*s));
    s->attempted = true;
    s->attempted_us = tsc_uptime_us();
    s->sta_status = s->info_status = s->bix_status = s->bst_status = s->psr_status = AE_NOT_EXIST;
    s->unit = s->design = s->full = s->design_voltage = TELEMETRY_UNKNOWN;
    s->rate = s->remaining = s->voltage = TELEMETRY_UNKNOWN;
    s->sta_status = read_integer(h, "_STA", &s->sta);
    /* AC adapters may omit _STA; batteries require bit 4 for presence. */
    if (kind == TELEMETRY_AC && s->sta_status == AE_NOT_FOUND) {
        s->sta = 0x0f;
        s->sta_status = AE_OK;
        s->sta_default = true;
    }
    if (s->sta_status == TELEMETRY_FROZEN)
        return false;
    if (ACPI_SUCCESS(s->sta_status) && (s->sta & ~31u))
        s->sta_status = AE_BAD_DATA;
    if (ACPI_FAILURE(s->sta_status))
        return true;
    s->present = (s->sta & 1) != 0 && (kind != TELEMETRY_BATTERY || (s->sta & 16) != 0);
    if (!s->present) {
        s->complete = true;
        return true;
    }
    /* Device is present but disabled/nonfunctional: do not touch its methods. */
    if ((s->sta & 0x0b) != 0x0b) {
        s->sta_status = AE_NOT_CONFIGURED;
        return true;
    }
    if (kind == TELEMETRY_AC) {
        uint32_t online;
        s->psr_status = read_integer(h, "_PSR", &online);
        if (s->psr_status == TELEMETRY_FROZEN)
            return false;
        if (ACPI_SUCCESS(s->psr_status) && online > 1)
            s->psr_status = AE_BAD_DATA;
        s->online = ACPI_SUCCESS(s->psr_status) && online == 1;
        s->complete = ACPI_SUCCESS(s->psr_status);
        return true;
    }
    union telemetry_result b;
    struct telemetry_view v;
    s->bix_status = evaluate(h, "_BIX", &b, &v);
    if (s->bix_status == TELEMETRY_FROZEN)
        return false;
    if (ACPI_SUCCESS(s->bix_status))
        s->bix_status = parse_information(&v, true, s);
    s->info_status = s->bix_status;
    if (ACPI_FAILURE(s->info_status)) {
        s->info_status = evaluate(h, "_BIF", &b, &v);
        if (s->info_status == TELEMETRY_FROZEN)
            return false;
        if (ACPI_SUCCESS(s->info_status))
            s->info_status = parse_information(&v, false, s);
    }
    s->bst_status = evaluate(h, "_BST", &b, &v);
    if (s->bst_status == TELEMETRY_FROZEN)
        return false;
    if (ACPI_SUCCESS(s->bst_status))
        s->bst_status = parse_status(&v, s);
    percentage(s);
    s->complete = ACPI_SUCCESS(s->info_status) && ACPI_SUCCESS(s->bst_status);
    return true;
}

/* One admitted BSP transaction. Exposed to the host test by including this C
 * file, not through a kernel test-only ABI or a second parser implementation. */
static void telemetry_step(void) {
    uint64_t f = spin_lock_irqsave(&telemetry_lock);
    if (frozen_epoch || busy) {
        spin_unlock_irqrestore(&telemetry_lock, f);
        return;
    }
    busy = true;
    bool need_discovery = !discovered;
    spin_unlock_irqrestore(&telemetry_lock, f);
    if (need_discovery) {
        struct discovery_pass pass = {0};
        ACPI_STATUS status = AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
            UINT32_MAX, discover_device, NULL, &pass, NULL);
        f = spin_lock_irqsave(&telemetry_lock);
        visited = pass.visited;
        overflow = pass.overflow;
        scan_truncated = pass.truncated;
        discovery_errors = pass.errors;
        discovery_last_error = pass.last;
        discovery_status = status;
        discovered = !pass.interrupted;
        spin_unlock_irqrestore(&telemetry_lock, f);
    }
    for (unsigned i = 0; i < device_count; i++) {
        uint64_t now = tsc_uptime_us();
        f = spin_lock_irqsave(&telemetry_lock);
        struct telemetry_device *d = &devices[i];
        bool due = !d->sample.attempted ||
            now - d->sample.attempted_us >= TELEMETRY_POLL_US ||
            (d->dirty && now - d->sample.attempted_us >= TELEMETRY_COALESCE_US);
        if (frozen_epoch) {
            spin_unlock_irqrestore(&telemetry_lock, f);
            break;
        }
        ACPI_HANDLE h = d->handle;
        enum telemetry_kind kind = d->kind;
        if (due)
            d->dirty = false;
        spin_unlock_irqrestore(&telemetry_lock, f);
        if (!due)
            continue;
        struct telemetry_sample s;
        if (!refresh(h, kind, &s)) {
            f = spin_lock_irqsave(&telemetry_lock);
            d->dirty = true;
            spin_unlock_irqrestore(&telemetry_lock, f);
            break;
        }
        f = spin_lock_irqsave(&telemetry_lock);
        s.successful_us = s.complete ? tsc_uptime_us() : d->sample.successful_us;
        d->sample = s;
        spin_unlock_irqrestore(&telemetry_lock, f);
    }
    f = spin_lock_irqsave(&telemetry_lock);
    busy = false;
    spin_unlock_irqrestore(&telemetry_lock, f);
}

static void telemetry_worker(void *arg) {
    (void)arg;
    for (;;) {
        telemetry_step();
        sched_sleep(250000);
    }
}

bool acpi_telemetry_start(void) {
    if (started)
        return true;
    /* Called after sched_init installed BSP GS/current, before sched_start.
       Creation is safe here; only the scheduled worker may evaluate AML. */
    if (this_cpu()->id || !this_cpu()->current)
        return false;
    struct task *t = task_allocate("acpi-telemetry", false, 0, 0);
    if (!t)
        return false;
    t->power_role = POWER_ROLE_TELEMETRY;
    t->entry = telemetry_worker;
    t->argument = NULL;
    started = true;
    task_publish(t);
    return true;
}

void acpi_telemetry_freeze(uint64_t epoch) {
    uint64_t f = spin_lock_irqsave(&telemetry_lock);
    if (epoch && epoch > last_epoch) {
        last_epoch = frozen_epoch = epoch;
    }
    spin_unlock_irqrestore(&telemetry_lock, f);
}

bool acpi_telemetry_idle(uint64_t epoch) {
    uint64_t f = spin_lock_irqsave(&telemetry_lock);
    bool idle = epoch && epoch == frozen_epoch && !busy;
    spin_unlock_irqrestore(&telemetry_lock, f);
    return idle;
}

void acpi_telemetry_thaw(uint64_t epoch) {
    uint64_t f = spin_lock_irqsave(&telemetry_lock);
    if (epoch && epoch == frozen_epoch) {
        frozen_epoch = 0;
        for (unsigned i = 0; i < device_count; i++)
            devices[i].dirty = true;
    }
    spin_unlock_irqrestore(&telemetry_lock, f);
}

struct text_output { char *out; size_t cap, used; bool cut; };
static void append(struct text_output *w, const char *fmt, ...) {
    if (w->used == w->cap - 1) {
        w->cut = true;
        return;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(w->out + w->used, w->cap - w->used, fmt, args);
    va_end(args);
    if (n < 0) {
        w->cut = true;
        return;
    }
    size_t available = w->cap - w->used;
    if ((size_t)n >= available) {
        w->used = w->cap - 1;
        w->cut = true;
    } else {
        w->used += (size_t)n;
    }
}

static void value_text(char *out, size_t cap, uint32_t value, bool valid) {
    if (!valid || value == TELEMETRY_UNKNOWN)
        snprintf(out, cap, "unknown");
    else
        snprintf(out, cap, "%u", value);
}

size_t acpi_telemetry_info(char *out, size_t cap) {
    if (!out || !cap)
        return 0;
    struct telemetry_device snapshot[TELEMETRY_DEVICES];
    uint64_t f = spin_lock_irqsave(&telemetry_lock);
    unsigned count = device_count, seen = visited, extra = overflow, errors = discovery_errors;
    bool running = started, done = discovered, active = busy, truncated = scan_truncated;
    uint64_t epoch = frozen_epoch;
    ACPI_STATUS status = discovery_status, last_error = discovery_last_error;
    memcpy(snapshot, devices, count * sizeof(snapshot[0]));
    spin_unlock_irqrestore(&telemetry_lock, f);
    struct text_output w = {out, cap, 0, false};
    out[0] = 0;
    append(&w, "telemetry=%s discovery=%s devices=%u limit=%u scanned=%u scan_limit=%u\n"
        "telemetry_frozen_epoch=%llu active=%u overflow=%u scan_truncated=%u discovery_errors=%u "
        "discovery_status=%s last_discovery_error=%s\n",
        running ? "running" : "unavailable", done ? ACPI_FAILURE(status) ? "failed" : "complete" : "pending", count,
        TELEMETRY_DEVICES, seen, TELEMETRY_SCAN_LIMIT, (unsigned long long)epoch, active,
        extra, truncated, errors, AcpiFormatException(status), AcpiFormatException(last_error));
    if (done && ACPI_SUCCESS(status) && !count)
        append(&w, "No supported ACPI0003/PNP0C0A devices found in the bounded scan.\n");
    uint64_t now = tsc_uptime_us();
    for (unsigned i = 0; i < count; i++) {
        const struct telemetry_device *d = &snapshot[i];
        const struct telemetry_sample *s = &d->sample;
        bool stale = d->dirty || !s->attempted || !s->complete || now - s->attempted_us > TELEMETRY_STALE_US;
        append(&w, "%s[%u] %s present=%s stale=%u refresh_pending=%u attempted=%u last_refresh_us=%llu last_success_us=%llu "
            "_STA=%s _STA_default=%u path_status=%s notify_status=%s notifications=%llu coalesced=%llu last_notify=%x\n",
            d->kind == TELEMETRY_AC ? "AC" : "Battery", i, d->path,
            !s->attempted || ACPI_FAILURE(s->sta_status) ? "unknown" : s->present ? "yes" : "no",
            stale, d->dirty, s->attempted, (unsigned long long)s->attempted_us,
            (unsigned long long)s->successful_us,
            s->attempted ? AcpiFormatException(s->sta_status) : "not sampled", s->sta_default,
            AcpiFormatException(d->path_status), AcpiFormatException(d->notify_status),
            (unsigned long long)d->notifications, (unsigned long long)d->coalesced, d->last_notify);
        if (d->kind == TELEMETRY_AC) {
            append(&w, "  online=%s _PSR=%s\n", !s->attempted || !s->present ||
                ACPI_FAILURE(s->psr_status) ? "unknown" : s->online ? "yes" : "no",
                s->attempted ? AcpiFormatException(s->psr_status) : "not sampled");
            continue;
        }
        bool info = s->attempted && s->present && ACPI_SUCCESS(s->info_status);
        bool bst = s->attempted && s->present && ACPI_SUCCESS(s->bst_status);
        char full[16], remaining[16], rate[16], voltage[16], percent[16], design[16];
        value_text(full, sizeof(full), s->full, info);
        value_text(design, sizeof(design), s->design, info);
        value_text(remaining, sizeof(remaining), s->remaining, bst);
        value_text(rate, sizeof(rate), s->rate, bst);
        value_text(voltage, sizeof(voltage), s->voltage, bst);
        value_text(percent, sizeof(percent), s->percent, s->percent_known);
        append(&w, "  source=%s _BIX=%s information=%s _BST=%s model=%s\n"
            "  unit=%s design=%s full=%s remaining=%s rate=%s voltage_mV=%s percent=%s state=%s critical=%s charge_limiting=%s\n",
            info ? s->from_bix ? "_BIX" : "_BIF" : "unavailable",
            s->attempted ? AcpiFormatException(s->bix_status) : "not sampled",
            s->attempted ? AcpiFormatException(s->info_status) : "not sampled",
            s->attempted ? AcpiFormatException(s->bst_status) : "not sampled", info ? s->model : "unknown",
            info ? s->unit ? "mAh/mA" : "mWh/mW" : "unknown", design, full, remaining, rate,
            voltage, percent, !bst ? "unknown" : s->state & 1 ? "discharging" : s->state & 2 ? "charging" :
            s->state & 8 ? "charge-limiting" : "idle", !bst ? "unknown" : s->state & 4 ? "yes" : "no",
            !bst ? "unknown" : s->state & 8 ? "yes" : "no");
    }
    append(&w, "Telemetry only: no runtime estimate, charging control, EC/SMBus backend, or hot-added namespace support.\n");
    static const char marker[] = "\ntelemetry_output_truncated=1\n";
    if (w.cut && cap > sizeof(marker)) {
        size_t at = cap - sizeof(marker);
        memcpy(out + at, marker, sizeof(marker));
        w.used = cap - 1;
    }
    return w.used;
}
