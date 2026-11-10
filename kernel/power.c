/* Coordinated, reversible software quiescence followed by terminal commit.
 * Admission is deliberately resource-aware: a preempted owner and an existing
 * vfork child remain runnable. Console/futex/socket waits need not complete.
 */
#include <power.h>
#include <sched.h>
#include <sync.h>
#include <panic.h>
#include <string.h>
#include <tsc.h>
#include <interrupts/lapic.h>
#include <acpi/power.h>
#include <acpi/osl.h>
#include <acpi/telemetry.h>
#include <drivers/block.h>
#include <drivers/audio.h>
#include <net/network.h>
#include <usb_input.h>
#include <boot/info.h>

#define POWER_IPI 0xe1
#define POWER_TIMEOUT_US UINT64_C(5000000)
#define POWER_PENDING_US UINT64_C(1000000)
enum phase { PHASE_IDLE, PHASE_RENDEZVOUS, PHASE_RELEASE, PHASE_COMMIT };
enum state { STATE_IDLE, STATE_PENDING, STATE_FREEZING, STATE_PREPARING,
             STATE_RENDEZVOUS, STATE_FLUSHING, STATE_COMMITTED, STATE_FAILED, STATE_RECOVERING };
static const char *const state_names[] = {"idle", "pending", "freezing", "preparing",
    "rendezvous", "flushing", "committed", "failed", "recovering"};
static spinlock_t control_lock;
static uint64_t command, epoch, request_id, pending_since, button_events;
static unsigned state, action, request_pid;
static bool started, firmware_present, frozen, armed, canceled, prepared, gpes_disabled;
enum { TEST_NONE, TEST_DRY_RUN, TEST_OMIT_ACK, TEST_FLUSH_FAIL, TEST_DRAIN_BUSY };
static unsigned test_mode; /* exact opt-in boot fixture, never syscall-controlled */
static int last_error;
static uint32_t firmware_status;
static char failure_stage[48];

static uint64_t make_command(uint64_t e, unsigned phase) { return (e << 3) | phase; }
static unsigned command_phase(uint64_t c) { return c & 7; }
static uint64_t command_epoch(uint64_t c) { return c >> 3; }

bool power_frozen(void) { return __atomic_load_n(&frozen, __ATOMIC_ACQUIRE); }

bool power_task_may_run(const struct task *t) {
    if (!power_frozen() || t->idle || t->power_role)
        return true;
    /* Counts belong to each task AND its CPU, so preempted owners drain.
       A shared-vfork child must finish exec/exit to release its parent. */
    return __atomic_load_n(&t->held_mutexes, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&t->no_park_depth, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&t->vfork_parent, __ATOMIC_ACQUIRE);
}

void power_mutex_acquired(void) {
    if (!sched_ready())
        return;
    struct cpu_local *c = this_cpu();
    if (c->held_mutexes == UINT32_MAX || c->current->held_mutexes == UINT32_MAX)
        panic("power: mutex ownership overflow");
    __atomic_add_fetch(&c->current->held_mutexes, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&c->held_mutexes, 1, __ATOMIC_RELEASE);
}

void power_mutex_released(void) {
    if (!sched_ready())
        return;
    struct cpu_local *c = this_cpu();
    if (!c->held_mutexes || !c->current->held_mutexes)
        panic("power: native mutex ownership underflow");
    __atomic_sub_fetch(&c->current->held_mutexes, 1, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&c->held_mutexes, 1, __ATOMIC_RELEASE);
}

void power_ap_checkpoint(void) {
    if (!sched_ready())
        return;
    struct cpu_local *c = this_cpu();
    if (!c->id || c->handoff || c->irq_depth)
        return;
    uint64_t observed = __atomic_load_n(&command, __ATOMIC_ACQUIRE);
    uint64_t e = command_epoch(observed);
    unsigned phase = command_phase(observed);
    if (!e || phase == PHASE_IDLE)
        return;
    __atomic_store_n(&c->park_seen_epoch, e, __ATOMIC_RELEASE);
    if (phase == PHASE_RELEASE) {
        __atomic_store_n(&c->resume_ack_epoch, e, __ATOMIC_RELEASE);
        return;
    }
    if (phase == PHASE_RENDEZVOUS && test_mode == TEST_OMIT_ACK &&
        c->id == sched_cpu_count() - 1)
        return;
    if (phase != PHASE_RENDEZVOUS ||
        __atomic_load_n(&c->held_mutexes, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&c->no_park_depth, __ATOMIC_ACQUIRE))
        return;
    /* The caller has IF=0 and has already sent EOI when entered from an IPI
       or timer. No scheduling, allocation, logging, CR3/GS/FPU changes here.
       The complete interrupt/kernel stack stays live until RELEASE. */
    __atomic_store_n(&c->park_ack_epoch, e, __ATOMIC_RELEASE);
    for (;;) {
        observed = __atomic_load_n(&command, __ATOMIC_ACQUIRE);
        if (command_epoch(observed) != e || command_phase(observed) == PHASE_RELEASE) {
            __atomic_store_n(&c->resume_ack_epoch, e, __ATOMIC_RELEASE);
            return;
        }
        if (command_phase(observed) == PHASE_COMMIT) {
            __atomic_store_n(&c->commit_ack_epoch, e, __ATOMIC_RELEASE);
            for (;;)
                __asm__ volatile("cli; hlt" ::: "memory");
        }
        __asm__ volatile("pause" ::: "memory");
    }
}

void power_checkpoint(void) {
    if (!sched_ready())
        return;
    struct cpu_local *c = this_cpu();
    /* IRQ code never sleeps for admission; native IRQ users are spin-only. */
    if (c->irq_depth || c->handoff)
        return;
    uint64_t flags = irq_save();
    /* An ordinary caller may already own an IRQ-masking spinlock. Native
       mutex/no-park counts do not cover it; only an IF=1 entry is parkable. */
    if (!(flags & 0x200))
        return;
    power_ap_checkpoint();
    irq_restore(flags);
    while (!power_task_may_run(c->current))
        sched_sleep(1000);
}

void power_transaction_begin(void) {
    if (!sched_ready())
        return;
    power_checkpoint();
    uint64_t flags = irq_save();
    struct cpu_local *c = this_cpu();
    if (c->no_park_depth == UINT32_MAX || c->current->no_park_depth == UINT32_MAX)
        panic("power: transaction depth overflow");
    __atomic_add_fetch(&c->current->no_park_depth, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&c->no_park_depth, 1, __ATOMIC_RELEASE);
    irq_restore(flags);
}

void power_transaction_end(void) {
    if (!sched_ready())
        return;
    uint64_t flags = irq_save();
    struct cpu_local *c = this_cpu();
    if (!c->no_park_depth || !c->current->no_park_depth)
        panic("power: transaction depth underflow");
    __atomic_sub_fetch(&c->current->no_park_depth, 1, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&c->no_park_depth, 1, __ATOMIC_RELEASE);
    if (flags & 0x200)
        power_ap_checkpoint();
    irq_restore(flags);
    if (flags & 0x200)
        power_checkpoint();
}

void power_task_exit(void) {
    if (!sched_ready())
        return;
    struct cpu_local *c = this_cpu();
    struct task *t = c->current;
    if (t->held_mutexes || c->no_park_depth < t->no_park_depth)
        panic("power: exiting resource owner");
    __atomic_sub_fetch(&c->no_park_depth, t->no_park_depth, __ATOMIC_RELEASE);
    t->no_park_depth = 0;
}

static bool devices_ready(void) {
    return block_power_ready() && audio_power_ready() && network_power_ready() &&
           kernel_usb_power_ready();
}

void power_button_event(void) {
    /* IRQ notification is only a confirmation prompt, never a terminal action. */
    __atomic_add_fetch(&button_events, 1, __ATOMIC_RELAXED);
}

long power_request(struct task *t, uint64_t requested, uint64_t flags) {
    if (flags || requested < OS_POWER_OFF || requested > OS_POWER_CANCEL)
        return -E_INVAL;
    if (!t || !t->user || t->browser_sandbox || !t->context || t->context->web_role)
        return -E_PERM;
    uint64_t f = spin_lock_irqsave(&control_lock);
    long result = 0;
    if (requested == OS_POWER_CANCEL) {
        if (state == STATE_PENDING || state == STATE_FREEZING)
            __atomic_store_n(&canceled, true, __ATOMIC_RELEASE);
        else if (state == STATE_FAILED) {
            state = STATE_IDLE;
            last_error = 0;
            failure_stage[0] = 0;
        } else if (state != STATE_IDLE)
            result = -E_BUSY;
    } else if (!started || !acpi_power_available(requested) || !devices_ready())
        result = -E_NOTSUP;
    else if (state != STATE_IDLE && state != STATE_FAILED) {
        if ((state == STATE_PENDING && canceled) || action != requested || state >= STATE_COMMITTED)
            result = -E_BUSY;
    } else {
        if (epoch == (UINT64_MAX >> 3)) {
            result = -E_INVAL;
        } else {
            request_id++;
            action = requested;
            request_pid = t->pid;
            pending_since = tsc_uptime_us();
            state = STATE_PENDING;
            armed = canceled = prepared = gpes_disabled = false;
            last_error = firmware_status = 0;
            failure_stage[0] = 0;
        }
    }
    spin_unlock_irqrestore(&control_lock, f);
    return result;
}

void power_request_ready(unsigned pid) {
    uint64_t f = spin_lock_irqsave(&control_lock);
    if (state == STATE_PENDING && pid == request_pid && !armed) {
        /* The interactive grace starts only after the requester has released
           syscall locks/pins. Coalesced duplicates must never extend it. */
        pending_since = tsc_uptime_us();
        armed = true;
    }
    spin_unlock_irqrestore(&control_lock, f);
}

static void set_state(unsigned value) {
    uint64_t f = spin_lock_irqsave(&control_lock);
    state = value;
    spin_unlock_irqrestore(&control_lock, f);
}

static bool is_canceled(void) { return __atomic_load_n(&canceled, __ATOMIC_ACQUIRE); }

/* Linearizes accepted cancellation against the first firmware preparation.
   No CANCEL can return accepted after this lock-protected boundary. */
static bool begin_preparing(void) {
    uint64_t flags = spin_lock_irqsave(&control_lock);
    bool proceed = !canceled && state == STATE_FREEZING;
    if (proceed)
        state = STATE_PREPARING;
    spin_unlock_irqrestore(&control_lock, flags);
    return proceed;
}


static bool cpus_ack(unsigned phase) {
    for (unsigned i = 1; i < sched_cpu_count(); i++) {
        struct cpu_local *c = sched_cpu(i);
        if (!c || !__atomic_load_n(&c->online, __ATOMIC_ACQUIRE))
            continue;
        uint64_t seen = phase == PHASE_RENDEZVOUS ?
            __atomic_load_n(&c->park_ack_epoch, __ATOMIC_ACQUIRE) :
            phase == PHASE_COMMIT ? __atomic_load_n(&c->commit_ack_epoch, __ATOMIC_ACQUIRE) :
            __atomic_load_n(&c->resume_ack_epoch, __ATOMIC_ACQUIRE);
        if (seen != epoch)
            return false;
    }
    return true;
}

static bool wait_cpus(unsigned phase) {
    uint64_t start = tsc_uptime_us();
    while (!cpus_ack(phase)) {
        if (tsc_uptime_us() - start >= POWER_TIMEOUT_US)
            return false;
        sched_sleep(1000);
    }
    return true;
}

static bool send_park_requests(void) {
    for (unsigned i = 1; i < sched_cpu_count(); i++) {
        struct cpu_local *c = sched_cpu(i);
        if (c && __atomic_load_n(&c->online, __ATOMIC_ACQUIRE) &&
            !apic_send_ipi(c->lapic_id, POWER_IPI))
            return false;
    }
    return true;
}

static void fail_transition(const char *stage, int error, bool rendezvous) {
    /* RELEASE is published BEFORE gates reopen. Late/deferred/stale IPIs read
       the current generation and cannot recreate the canceled rendezvous. */
    if (rendezvous)
        __atomic_store_n(&command, make_command(epoch, PHASE_RELEASE), __ATOMIC_RELEASE);
    acpi_os_power_thaw();
    uint32_t restore = gpes_disabled ? acpi_power_restore_gpes() : 0;
    acpi_telemetry_thaw(epoch);
    __atomic_store_n(&frozen, false, __ATOMIC_RELEASE);
    bool resumed = !rendezvous || wait_cpus(PHASE_RELEASE);
    uint64_t f = spin_lock_irqsave(&control_lock);
    last_error = error;
    snprintf(failure_stage, sizeof(failure_stage), "%s", !resumed ? "release-unresponsive" :
             restore ? "runtime-gpe-restore" : stage);
    if (restore)
        firmware_status = restore;
    /* A release timeout remains non-idle: no new epoch may overlap it. */
    state = resumed && !restore ? STATE_FAILED : STATE_RECOVERING;
    spin_unlock_irqrestore(&control_lock, f);
    pr_warn("power: request %llu failed stage=%s error=%d firmware=0x%x software-resumed=%u prep-side-effects=%u\n",
            (unsigned long long)request_id, failure_stage, error, firmware_status,
            resumed, prepared);
}

static void terminal_failure(const char *stage, int error) {
    uint64_t f = spin_lock_irqsave(&control_lock);
    last_error = error;
    snprintf(failure_stage, sizeof(failure_stage), "%s", stage);
    state = STATE_COMMITTED;
    spin_unlock_irqrestore(&control_lock, f);
    pr_err("power: terminal failure stage=%s error=%d; applications remain stopped\n", stage, error);
    for (;;)
        __asm__ volatile("cli; hlt" ::: "memory");
}

static void transition(void) {
    if (test_mode == TEST_OMIT_ACK && sched_cpu_count() < 2) {
        fail_transition("test-needs-ap", -E_NOTSUP, false);
        return;
    }
    if (!devices_ready() || !acpi_power_available(action)) {
        fail_transition("preflight", -E_NOTSUP, false);
        return;
    }
    epoch++;
    set_state(STATE_FREEZING);
    __atomic_store_n(&frozen, true, __ATOMIC_RELEASE);
    acpi_telemetry_freeze(epoch);
    uint64_t start = tsc_uptime_us();
    while (!sched_power_drained() || !acpi_telemetry_idle(epoch) || test_mode == TEST_DRAIN_BUSY) {
        if (is_canceled() || tsc_uptime_us() - start >= POWER_TIMEOUT_US) {
            fail_transition(is_canceled() ? "canceled" : "resource-drain", -E_BUSY, false);
            return;
        }
        sched_sleep(1000);
    }
    if (!begin_preparing()) {
        fail_transition("canceled", -E_INTR, false);
        return;
    }
    pr_info("power: preparation BSP0 IF=1 request=%llu\n", (unsigned long long)request_id);
    firmware_status = acpi_power_prepare(action); /* IF=1, BSP/SCI/workers live */
    prepared = action == OS_POWER_OFF; /* even failure may have AML side effects */
    if (firmware_status) {
        fail_transition("firmware-prep", -E_IO, false);
        return;
    }
    gpes_disabled = true; /* restoration needed even if disable partly fails */
    firmware_status = acpi_power_disable_gpes();
    if (firmware_status) {
        fail_transition("gpe-disable", -E_IO, false);
        return;
    }
    start = tsc_uptime_us();
    while (!acpi_os_power_freeze()) {
        if (tsc_uptime_us() - start >= POWER_TIMEOUT_US) {
            fail_transition("firmware-work-drain", -E_BUSY, false);
            return;
        }
        sched_sleep(1000);
    }
    set_state(STATE_RENDEZVOUS);
    __atomic_store_n(&command, make_command(epoch, PHASE_RENDEZVOUS), __ATOMIC_RELEASE);
    if (!send_park_requests() || !wait_cpus(PHASE_RENDEZVOUS)) {
        fail_transition("cpu-rendezvous", -E_TIMEDOUT, true);
        return;
    }
    pr_info("power: reversible park acknowledged epoch=%llu cpus=%u\n",
            (unsigned long long)epoch, sched_cpu_count());
    set_state(STATE_FLUSHING);
    mutex_lock(&vfs_mutex);
    int error = vfs_power_ready() ? vfs_sync() : -E_NOTSUP;
    if (!error)
        error = block_power_flush();
    mutex_unlock(&vfs_mutex);
    if (!error && test_mode == TEST_FLUSH_FAIL)
        error = -E_IO;
    if (error) {
        fail_transition("filesystem-flush", error, true);
        return;
    }
    pr_info("power: filesystem and writable-device flush complete epoch=%llu\n",
            (unsigned long long)epoch);
    if (!devices_ready()) {
        fail_transition("controller-preflight", -E_NOTSUP, true);
        return;
    }
    if (test_mode) {
        fail_transition("dry-run", -E_INTR, true);
        return;
    }
    /* First controller stop is irreversible. Never resume applications after
       this point, even if a device refuses to stop or terminal write returns. */
    set_state(STATE_COMMITTED);
    __atomic_store_n(&command, make_command(epoch, PHASE_COMMIT), __ATOMIC_RELEASE);
    if (!wait_cpus(PHASE_COMMIT))
        terminal_failure("cpu-commit", -E_TIMEDOUT);
    if ((error = network_power_stop()) || (error = kernel_usb_power_stop()) ||
        (error = audio_power_stop()) || (error = block_power_stop()))
        terminal_failure("controller-stop", error);
    pr_notice("power: COMMIT request=%llu action=%u cpus=%u; flushed and controllers stopped\n",
              (unsigned long long)request_id, action, sched_cpu_count());
    __asm__ volatile("cli" ::: "memory");
    firmware_status = acpi_power_enter(action);
    /* AE_OK is only a completed register-write attempt, never success proof. */
    terminal_failure("terminal-write-returned", firmware_status ? -E_IO : -E_TIMEDOUT);
}

static void coordinator(void *unused) {
    (void)unused;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&control_lock);
        bool pending = state == STATE_PENDING && armed &&
                       tsc_uptime_us() - pending_since >= POWER_PENDING_US;
        bool cancel = pending && canceled;
        if (cancel) {
            state = STATE_IDLE;
            action = 0;
        }
        spin_unlock_irqrestore(&control_lock, f);
        if (pending && !cancel)
            transition();
        sched_sleep(1000);
    }
}

static bool boot_token(const char *token) {
    const char *p = boot_info.cmdline;
    size_t length = strlen(token);
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\t') end++;
        if ((size_t)(end - p) == length && !memcmp(p, token, length))
            return true;
        p = end;
    }
    return false;
}

bool power_start(bool firmware) {
    if (boot_token("power.test=dry-run")) test_mode = TEST_DRY_RUN;
    else if (boot_token("power.test=omit-ack")) test_mode = TEST_OMIT_ACK;
    else if (boot_token("power.test=flush-fail")) test_mode = TEST_FLUSH_FAIL;
    else if (boot_token("power.test=drain-busy")) test_mode = TEST_DRAIN_BUSY;
    if (test_mode)
        pr_warn("power: opt-in disposable test mode=%u; terminal action inhibited\n", test_mode);
    firmware_present = firmware;
    struct task *t = task_allocate("power", false, 0, 0);
    if (!t)
        return false;
    t->power_role = POWER_ROLE_COORDINATOR;
    t->entry = coordinator;
    task_publish(t);
    started = true;
    return true;
}

size_t power_info(char *out, size_t cap) {
    uint64_t f = spin_lock_irqsave(&control_lock);
    int n = snprintf(out, cap,
        "power_state=%s\npoweroff_ready=%u\nreset_ready=%u\nrequest_id=%llu\n"
        "action=%u\nlast_error=%d\nfailure_stage=%s\nfirmware_status=0x%x\n"
        "fixed_button_events=%llu\nfirmware_prep_attempted=%u\n"
        "Software/AP rollback is reversible before commit; arbitrary _PTS side effects are unverified.\n"
        "Firmware preparation AML can wait indefinitely; no unsafe live-AML cancellation is attempted.\n"
        "Current authorization: trusted single-user tasks only; browser roles denied.\n",
        state_names[state], started && acpi_power_available(OS_POWER_OFF) && devices_ready(),
        started && acpi_power_available(OS_POWER_RESET) && devices_ready(),
        (unsigned long long)request_id, action, last_error,
        failure_stage[0] ? failure_stage : "none", firmware_status,
        (unsigned long long)__atomic_load_n(&button_events, __ATOMIC_RELAXED), prepared);
    spin_unlock_irqrestore(&control_lock, f);
    return n < 0 || !cap ? 0 : min((size_t)n, cap - 1);
}

size_t power_acpi_info(char *out, size_t cap) {
    if (!cap)
        return 0;
    size_t used = acpi_power_info(out, cap);
    struct acpi_os_diagnostics d;
    acpi_os_get_diagnostics(&d);
    int n = snprintf(out + used, cap - used,
        "SCI/deferred workers=%u GPE_slots=%u event_peak=%llu notify_peak=%llu\n"
        "mapping_rejections=%llu event_rejections=%llu notify_rejections=%llu GPE_rejections=%llu\n",
        d.event_workers, d.gpe_capacity, (unsigned long long)d.event_high_water,
        (unsigned long long)d.notification_high_water, (unsigned long long)d.mapping_rejections,
        (unsigned long long)d.event_rejections, (unsigned long long)d.notification_rejections,
        (unsigned long long)d.gpe_rejections);
    if (n > 0)
        used += min((size_t)n, cap - used - 1);
    if (firmware_present && used < cap - 1)
        used += acpi_telemetry_info(out + used, cap - used);
    return used;
}
