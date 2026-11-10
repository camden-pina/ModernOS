/* ACPICA OS Services Layer for Coast (x86-64). */
#include <acpi.h>
#include <acpi/osl.h>
#include <boot/info.h>
#include <descriptor_tables/isr.h>
#include <interrupts/lapic.h>
#include <io.h>
#include <mm/pmm.h>
#include <mm/user.h>
#include <mm/vmem_io.h>
#include <panic.h>
#include <printk.h>
#include <sched.h>
#include <power.h>
#include <string.h>
#include <sync.h>
#include <tsc.h>

#define ACPI_SCI_VECTOR 0x52
#define ACPI_WORK_CAPACITY 256
#define ACPI_EVENT_WORKERS 4
#define ACPI_MAP_LIMIT (64u * 1024 * 1024)
#define ACPI_PAGE_ADDRESS UINT64_C(0x000ffffffffff000)

/* The platform's FACS lock macros use byte offset 16 to avoid taking the
   address of a packed member. Keep that ABI assumption checked here. */
_Static_assert(offsetof(ACPI_TABLE_FACS, GlobalLock) == 16,
               "ACPICA FACS GlobalLock offset changed");

static uint64_t root_pointer;
static spinlock_t mapping_lock;
static struct {
    uint64_t first, end;
} firmware_memory[256];
static unsigned firmware_memory_count;
static struct acpi_os_diagnostics diagnostics;

static void diagnostic_increment(uint64_t *counter) {
    __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
}

static bool interrupts_enabled(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}

static uint64_t msr_read(uint32_t index) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return low | ((uint64_t)high << 32);
}

/* Keep privileged CPU boundaries small enough for policy-preserving tests. */
static uint64_t control_register(unsigned number) {
    uint64_t value;
    if (number == 3)
        __asm__ volatile("mov %%cr3,%0" : "=r"(value));
    else
        __asm__ volatile("mov %%cr4,%0" : "=r"(value));
    return value;
}

static bool cpu_has_pat(void) {
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (d & (1u << 16)) != 0;
}

static void wait_briefly(void) {
    if (sched_ready() && interrupts_enabled())
        sched_sleep(1000);
    else
        __asm__ volatile("pause" ::: "memory");
}

void acpi_os_set_root_pointer(uint64_t physical_rsdp) {
    root_pointer = physical_rsdp;
    if (physical_rsdp && !acpi_os_mark_firmware_memory(physical_rsdp, sizeof(ACPI_TABLE_RSDP)))
        panic("cannot register ACPI root pointer memory");
}

bool acpi_os_mark_firmware_memory(uint64_t physical, size_t length) {
    const uint64_t limit = UINT64_C(0x0000800000000000);
    if (!length || physical >= limit || length > limit - physical)
        return false;
    uint64_t first = physical & ~UINT64_C(4095);
    uint64_t end = (physical + length + 4095) & ~UINT64_C(4095);
    uint64_t flags = spin_lock_irqsave(&mapping_lock);
    for (unsigned i = 0; i < firmware_memory_count;) {
        if (first <= firmware_memory[i].end && end >= firmware_memory[i].first) {
            if (firmware_memory[i].first < first)
                first = firmware_memory[i].first;
            if (firmware_memory[i].end > end)
                end = firmware_memory[i].end;
            firmware_memory[i] = firmware_memory[--firmware_memory_count];
            i = 0;
        } else {
            i++;
        }
    }
    bool success = firmware_memory_count < sizeof(firmware_memory) / sizeof(firmware_memory[0]);
    if (success) {
        firmware_memory[firmware_memory_count].first = first;
        firmware_memory[firmware_memory_count++].end = end;
    }
    spin_unlock_irqrestore(&mapping_lock, flags);
    return success;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void) {
    return root_pointer;
}

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *initial,
                                   ACPI_STRING *replacement) {
    if (!initial || !replacement)
        return AE_BAD_PARAMETER;
    *replacement = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *existing, ACPI_TABLE_HEADER **replacement) {
    if (!existing || !replacement)
        return AE_BAD_PARAMETER;
    *replacement = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *existing,
                                       ACPI_PHYSICAL_ADDRESS *address, UINT32 *length) {
    if (!existing || !address || !length)
        return AE_BAD_PARAMETER;
    *address = 0;
    *length = 0;
    return AE_OK;
}

void *AcpiOsAllocate(ACPI_SIZE size) {
    return size ? kmalloc(size) : NULL;
}

void *AcpiOsAllocateZeroed(ACPI_SIZE size) {
    return size ? kmallocz(size) : NULL;
}

void AcpiOsFree(void *memory) {
    kfree(memory);
}

/* Preserve the cache attributes of RAM, especially firmware ACPI NVS. */
static bool physical_ram(uint64_t address, size_t length) {
    while (length) {
        size_t covered = 0;
        for (unsigned i = 0; i < firmware_memory_count; i++) {
            if (address >= firmware_memory[i].first && address < firmware_memory[i].end) {
                uint64_t remaining = firmware_memory[i].end - address;
                covered = remaining > length ? length : remaining;
                break;
            }
        }
        for (uint64_t i = 0; !covered && i < boot_info.memory_map_count; i++) {
            const struct boot_memory_range *range = &boot_info.memory_map[i];
            if (range->type == BOOT_MEMORY_RESERVED || range->type == BOOT_MEMORY_BAD ||
                range->type == BOOT_MEMORY_FRAMEBUFFER)
                continue;
            if (address >= range->base && address - range->base < range->length) {
                uint64_t remaining = range->length - (address - range->base);
                covered = remaining > length ? length : remaining;
                break;
            }
        }
        if (!covered)
            return false;
        address += covered;
        length -= covered;
    }
    return true;
}

/* Cache types belong to complete pages. Refuse an ambiguous mixed page rather
 * than create a UC alias for part of RAM or a WB alias for device registers. */
static int physical_page_kind(uint64_t page) {
    if (physical_ram(page, PAGE_SIZE))
        return 1;
    for (unsigned i = 0; i < firmware_memory_count; i++)
        if (page < firmware_memory[i].end && page + PAGE_SIZE > firmware_memory[i].first)
            return -1;
    for (uint64_t i = 0; i < boot_info.memory_map_count; i++) {
        const struct boot_memory_range *range = &boot_info.memory_map[i];
        if (range->type == BOOT_MEMORY_RESERVED || range->type == BOOT_MEMORY_BAD ||
            range->type == BOOT_MEMORY_FRAMEBUFFER || !range->length)
            continue;
        if (page < range->base ? range->base - page < PAGE_SIZE :
            page - range->base < range->length)
            return -1;
    }
    return 0;
}

/* Inspect an existing mapping without splitting a huge leaf or changing it. */
static bool page_attributes(uint64_t virtual_address, uint64_t *physical,
                            bool *writable, unsigned *pat_index) {
    uint64_t cr4 = control_register(4);
    if (cr4 & (UINT64_C(1) << 12))
        return false; /* The kernel VM supports four-level paging only. */
    if (virtual_address > UINT64_C(0x00007fffffffffff) &&
        virtual_address < UINT64_C(0xffff800000000000))
        return false;
    uint64_t root = control_register(3);
    const uint64_t *table = phys_ptr(root & ACPI_PAGE_ADDRESS);
    bool write = true;
    for (unsigned level = 3;; level--) {
        uint64_t entry = table[(virtual_address >> (12 + level * 9)) & 511];
        if (!(entry & 1))
            return false;
        write = write && (entry & 2);
        if (!level || (level < 3 && (entry & 128))) {
            uint64_t mask = (UINT64_C(1) << (12 + level * 9)) - 1;
            *physical = ((entry & ACPI_PAGE_ADDRESS) & ~mask) | (virtual_address & mask);
            *writable = write;
            *pat_index = ((entry >> 3) & 3) | ((entry >> (level ? 10 : 5)) & 4);
            return true;
        }
        if (entry & 128)
            return false;
        table = phys_ptr(entry & ACPI_PAGE_ADDRESS);
    }
}

static bool uncached_mapping(unsigned index) {
    if (!cpu_has_pat())
        return (index & 3) == 3;
    return ((msr_read(0x277) >> (index * 8)) & 0xff) == 0;
}

void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS where, ACPI_SIZE length) {
    const uint64_t limit = UINT64_C(0x0000800000000000);
    if (!length || length > ACPI_MAP_LIMIT || where >= limit || length > limit - where ||
        !boot_info.hhdm_offset || boot_info.hhdm_offset > UINT64_MAX - (where + length - 1)) {
        diagnostic_increment(&diagnostics.mapping_rejections);
        return NULL;
    }
    uint64_t first = where & ~UINT64_C(4095), last = (where + length - 1) & ~UINT64_C(4095);
    uint64_t flags = spin_lock_irqsave(&mapping_lock);
    /* The VM currently has no SMP TLB shootdowns. Runtime calls may borrow
       existing mappings, but must never create/retype a global mapping. */
    if (sched_ready()) {
        for (uint64_t page = first;; page += PAGE_SIZE) {
            int kind = physical_page_kind(page);
            uint64_t physical;
            bool writable;
            unsigned pat;
            if (kind < 0 || !page_attributes((uintptr_t)phys_ptr(page), &physical, &writable, &pat) ||
                physical != page || !writable || (!kind && !uncached_mapping(pat))) {
                diagnostic_increment(&diagnostics.mapping_rejections);
                spin_unlock_irqrestore(&mapping_lock, flags);
                return NULL;
            }
            if (page == last)
                break;
        }
        spin_unlock_irqrestore(&mapping_lock, flags);
        return phys_ptr(where);
    }
    bool valid = true;
    for (uint64_t page = first;;) {
        int kind = physical_page_kind(page);
        if (kind < 0) {
            valid = false;
            break;
        }
        if (kind) {
            uint64_t physical;
            bool writable;
            unsigned pat;
            bool present = page_attributes((uintptr_t)phys_ptr(page), &physical, &writable, &pat);
            if ((present && physical != page) ||
                ((!present || !writable) &&
                 !vmem_map_page_in_root(kernel_cr3(), (uintptr_t)phys_ptr(page), page))) {
                valid = false;
                break;
            }
        } else {
            uint64_t end = page;
            while (end < last && physical_page_kind(end + PAGE_SIZE) == 0)
                end += PAGE_SIZE;
            if (!vmem_map_mmio(page, end - page + PAGE_SIZE)) {
                valid = false;
                break;
            }
            page = end;
        }
        if (page == last)
            break;
        page += PAGE_SIZE;
    }
    void *result = valid ? phys_ptr(where) : NULL;
    spin_unlock_irqrestore(&mapping_lock, flags);
    if (!result)
        diagnostic_increment(&diagnostics.mapping_rejections);
    return result;
}

void AcpiOsUnmapMemory(void *logical, ACPI_SIZE length) {
    /* Borrowed shared direct mappings are also used by other drivers. */
    (void)logical;
    (void)length;
}

ACPI_STATUS AcpiOsGetPhysicalAddress(void *logical, ACPI_PHYSICAL_ADDRESS *physical) {
    uint64_t address;
    if (!logical || !physical)
        return AE_BAD_PARAMETER;
    if (!vmem_translate((uintptr_t)logical, &address))
        return AE_BAD_ADDRESS;
    *physical = address;
    return AE_OK;
}

static BOOLEAN memory_accessible(void *pointer, ACPI_SIZE length, bool write) {
    uint64_t address = (uintptr_t)pointer;
    if (!pointer || !length || length - 1 > UINT64_MAX - address)
        return FALSE;
    uint64_t last = (address + length - 1) & ~UINT64_C(4095);
    for (uint64_t page = address & ~UINT64_C(4095);; page += PAGE_SIZE) {
        uint64_t physical;
        bool writable;
        unsigned pat;
        if (!page_attributes(page, &physical, &writable, &pat) || (write && !writable))
            return FALSE;
        if (page == last)
            return TRUE;
    }
}

BOOLEAN AcpiOsReadable(void *pointer, ACPI_SIZE length) {
    return memory_accessible(pointer, length, false);
}

BOOLEAN AcpiOsWritable(void *pointer, ACPI_SIZE length) {
    return memory_accessible(pointer, length, true);
}

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *handle) {
    if (!handle)
        return AE_BAD_PARAMETER;
    *handle = kmallocz(sizeof(spinlock_t));
    return *handle ? AE_OK : AE_NO_MEMORY;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK handle) {
    kfree(handle);
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK handle) {
    return spin_lock_irqsave(handle);
}

void AcpiOsReleaseLock(ACPI_SPINLOCK handle, ACPI_CPU_FLAGS flags) {
    spin_unlock_irqrestore(handle, flags);
}

struct acpi_semaphore {
    spinlock_t lock;
    uint32_t available, maximum;
};

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 maximum, UINT32 initial, ACPI_SEMAPHORE *handle) {
    if (!handle || !maximum || initial > maximum)
        return AE_BAD_PARAMETER;
    struct acpi_semaphore *sem = kmallocz(sizeof(*sem));
    if (!sem)
        return AE_NO_MEMORY;
    sem->maximum = maximum;
    sem->available = initial;
    *handle = sem;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE handle) {
    if (!handle)
        return AE_BAD_PARAMETER;
    /* ACPICA guarantees there are no remaining users of a deleted object. */
    kfree(handle);
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE handle, UINT32 units, UINT16 timeout) {
    struct acpi_semaphore *sem = handle;
    if (!sem || !units || units > sem->maximum)
        return AE_BAD_PARAMETER;
    uint64_t start = tsc_uptime_us();
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&sem->lock);
        if (sem->available >= units) {
            sem->available -= units;
            spin_unlock_irqrestore(&sem->lock, flags);
            return AE_OK;
        }
        spin_unlock_irqrestore(&sem->lock, flags);
        if (!timeout || (timeout != ACPI_WAIT_FOREVER &&
                         tsc_uptime_us() - start >= (uint64_t)timeout * 1000))
            return AE_TIME;
        wait_briefly();
    }
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE handle, UINT32 units) {
    struct acpi_semaphore *sem = handle;
    if (!sem || !units)
        return AE_BAD_PARAMETER;
    uint64_t flags = spin_lock_irqsave(&sem->lock);
    if (units > sem->maximum - sem->available) {
        spin_unlock_irqrestore(&sem->lock, flags);
        return AE_LIMIT;
    }
    sem->available += units;
    spin_unlock_irqrestore(&sem->lock, flags);
    return AE_OK;
}

bool acpi_os_acquire_global_lock(volatile uint32_t *lock) {
    if (!lock)
        return true;
    uint32_t old = __atomic_load_n(lock, __ATOMIC_ACQUIRE), desired;
    do {
        desired = (old & ~3u) | 2u;
        if (old & 2u)
            desired |= 1u;
    } while (!__atomic_compare_exchange_n(lock, &old, desired, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return !(desired & 1u);
}

bool acpi_os_release_global_lock(volatile uint32_t *lock) {
    if (!lock)
        return false;
    uint32_t old = __atomic_load_n(lock, __ATOMIC_ACQUIRE);
    while (!__atomic_compare_exchange_n(lock, &old, old & ~3u, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        ;
    return (old & 1u) != 0;
}

struct acpi_work {
    ACPI_OSD_EXEC_CALLBACK function;
    void *context;
};

/* General work is bounded; hardware GPE dispatch and completion admission is
 * reserved from the actual boot topology and cannot be consumed by Notify().
 * Four event workers permit a GPE to wait for another independent GPE. Firmware
 * dependency chains that occupy all workers can still stall: this is not an
 * unbounded thread-per-method implementation. There is one FIFO notify worker.
 */
struct acpi_work_queue {
    struct acpi_work entries[ACPI_WORK_CAPACITY];
    uint64_t order[ACPI_WORK_CAPACITY];
    unsigned head, count, active;
};
struct acpi_gpe_slot {
    void *context;
    ACPI_OSD_EXEC_CALLBACK event, finish;
    uint64_t event_order, finish_order;
    bool event_pending, event_running, finish_pending, finish_running;
};
static struct acpi_work_queue work_queues[2];
static struct acpi_gpe_slot *gpe_slots;
static size_t gpe_slot_count;
static bool gpe_slots_reserved;
static uint64_t work_order;
static spinlock_t work_lock;
static bool workers_started, work_stopping, work_frozen;

bool acpi_os_reserve_gpes(void *const *contexts, size_t count) {
    if (sched_ready() || workers_started || gpe_slots_reserved ||
        (count && !contexts) || count > UINT32_MAX ||
        count > SIZE_MAX / sizeof(*gpe_slots))
        return false;
    struct acpi_gpe_slot *slots = count ? kmallocz(count * sizeof(*slots)) : NULL;
    if (count && !slots)
        return false;
    for (size_t i = 0; i < count; i++) {
        if (!contexts[i]) {
            kfree(slots);
            return false;
        }
        slots[i].context = contexts[i];
    }
    /* BSP-only setup, before runtime GPEs are enabled. The upstream GPE lock
       must never be acquired while holding work_lock (IRQ dispatch nests the
       locks in the other order). No allocator is used in AcpiOsExecute. */
    gpe_slots = slots;
    gpe_slot_count = count;
    gpe_slots_reserved = true;
    return true;
}

void acpi_os_get_diagnostics(struct acpi_os_diagnostics *out) {
    if (!out)
        return;
    uint64_t flags = spin_lock_irqsave(&work_lock);
    *out = (struct acpi_os_diagnostics){
        .mapping_rejections = __atomic_load_n(&diagnostics.mapping_rejections, __ATOMIC_RELAXED),
        .notification_rejections = diagnostics.notification_rejections,
        .event_rejections = diagnostics.event_rejections,
        .gpe_rejections = diagnostics.gpe_rejections,
        .notification_high_water = diagnostics.notification_high_water,
        .event_high_water = diagnostics.event_high_water,
        .gpe_capacity = gpe_slot_count,
        .event_workers = workers_started ? ACPI_EVENT_WORKERS : 0,
    };
    spin_unlock_irqrestore(&work_lock, flags);
}

ACPI_STATUS AcpiOsInitialize(void) {
    /* This boot-lifetime port does not recreate terminated kernel workers. */
    return work_stopping ? AE_SUPPORT : AE_OK;
}

static bool order_before(uint64_t a, uint64_t b) {
    return (int64_t)(a - b) < 0;
}

static bool run_one_work(unsigned index) {
    uint64_t flags = spin_lock_irqsave(&work_lock);
    struct acpi_work_queue *queue = &work_queues[index];
    struct acpi_gpe_slot *selected = NULL;
    uint64_t order = queue->count ? queue->order[queue->head] : 0;
    bool found = queue->count != 0;
    for (size_t i = 0; i < gpe_slot_count; i++) {
        struct acpi_gpe_slot *slot = &gpe_slots[i];
        bool pending = index ? slot->finish_pending : slot->event_pending;
        bool running = index ? slot->finish_running : slot->event_running;
        uint64_t candidate = index ? slot->finish_order : slot->event_order;
        if (pending && !running && (!found || order_before(candidate, order))) {
            selected = slot;
            order = candidate;
            found = true;
        }
    }
    if (!found) {
        spin_unlock_irqrestore(&work_lock, flags);
        return false;
    }
    struct acpi_work item;
    if (selected) {
        item.context = selected->context;
        item.function = index ? selected->finish : selected->event;
        if (index) {
            selected->finish_pending = false;
            selected->finish_running = true;
        } else {
            selected->event_pending = false;
            selected->event_running = true;
        }
    } else {
        item = queue->entries[queue->head];
        queue->head = (queue->head + 1) % ACPI_WORK_CAPACITY;
        queue->count--;
    }
    queue->active++;
    spin_unlock_irqrestore(&work_lock, flags);
    item.function(item.context);
    flags = spin_lock_irqsave(&work_lock);
    if (selected) {
        if (index)
            selected->finish_running = false;
        else
            selected->event_running = false;
    }
    queue->active--;
    spin_unlock_irqrestore(&work_lock, flags);
    return true;
}

static bool work_pending(unsigned index) {
    if (work_queues[index].count || work_queues[index].active)
        return true;
    for (size_t i = 0; i < gpe_slot_count; i++)
        if (index ? gpe_slots[i].finish_pending : gpe_slots[i].event_pending)
            return true;
    return false;
}

static void acpi_worker(void *argument) {
    unsigned index = (uintptr_t)argument;
    for (;;) {
        if (run_one_work(index))
            continue;
        uint64_t flags = spin_lock_irqsave(&work_lock);
        bool stopping = work_stopping && !work_pending(index);
        spin_unlock_irqrestore(&work_lock, flags);
        if (stopping)
            return;
        sched_sleep(1000);
    }
}

bool acpi_os_start_workers(void) {
    if (workers_started)
        return true;
    if (!gpe_slots_reserved || work_stopping)
        return false;
    struct task *tasks[ACPI_EVENT_WORKERS + 1];
    for (unsigned i = 0; i < ACPI_EVENT_WORKERS + 1; i++) {
        tasks[i] = task_allocate(i == ACPI_EVENT_WORKERS ? "acpi-notify" : "acpi-events",
                                 false, 0, 0);
        if (!tasks[i]) {
            while (i)
                task_discard(tasks[--i]);
            return false;
        }
        tasks[i]->power_role = POWER_ROLE_FIRMWARE;
        tasks[i]->entry = acpi_worker;
        tasks[i]->argument = (void *)(uintptr_t)(i == ACPI_EVENT_WORKERS);
    }
    for (unsigned i = 0; i < ACPI_EVENT_WORKERS + 1; i++)
        task_publish(tasks[i]);
    workers_started = true;
    return true;
}

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE type, ACPI_OSD_EXEC_CALLBACK function, void *context) {
    if (!function || type < OSL_GLOBAL_LOCK_HANDLER || type > OSL_EC_BURST_HANDLER)
        return AE_BAD_PARAMETER;
    if (type == OSL_DEBUGGER_MAIN_THREAD || type == OSL_DEBUGGER_EXEC_THREAD)
        return AE_SUPPORT;
    unsigned index = type == OSL_NOTIFY_HANDLER ? 1 : 0;
    uint64_t flags = spin_lock_irqsave(&work_lock);
    struct acpi_work_queue *queue = &work_queues[index];
    struct acpi_gpe_slot *gpe = NULL;
    if (type == OSL_GPE_HANDLER || type == OSL_NOTIFY_HANDLER)
        for (size_t i = 0; i < gpe_slot_count; i++)
            if (gpe_slots[i].context == context) {
                gpe = &gpe_slots[i];
                break;
            }
    ACPI_STATUS status = AE_OK;
    if (work_stopping || work_frozen)
        status = AE_NOT_EXIST;
    else if (gpe) {
        /* Completion can re-enable/retrigger while the original callback is
           still returning. Keep one pending next generation, and never run
           two callbacks of the same kind/context concurrently. Hardware stays
           disabled until its finish callback, so a second pending generation
           violates the ACPICA dispatch lifecycle rather than queue capacity. */
        bool *pending = index ? &gpe->finish_pending : &gpe->event_pending;
        if (*pending)
            status = AE_ALREADY_EXISTS;
        else if (index) {
            gpe->finish = function;
            gpe->finish_order = work_order++;
            gpe->finish_pending = true;
        } else {
            gpe->event = function;
            gpe->event_order = work_order++;
            gpe->event_pending = true;
        }
    } else if (type == OSL_GPE_HANDLER) {
        status = AE_SUPPORT; /* Runtime topology changes are not supported. */
    } else if (queue->count == ACPI_WORK_CAPACITY) {
        status = AE_NO_MEMORY;
    } else {
        unsigned tail = (queue->head + queue->count) % ACPI_WORK_CAPACITY;
        queue->entries[tail] = (struct acpi_work){function, context};
        queue->order[tail] = work_order++;
        queue->count++;
        uint64_t *peak = index ? &diagnostics.notification_high_water : &diagnostics.event_high_water;
        if (queue->count > *peak)
            *peak = queue->count;
    }
    uint64_t rejected_count = 0;
    if (ACPI_FAILURE(status)) {
        uint64_t *counter = gpe || type == OSL_GPE_HANDLER ? &diagnostics.gpe_rejections :
                            index ? &diagnostics.notification_rejections : &diagnostics.event_rejections;
        rejected_count = ++*counter;
    }
    spin_unlock_irqrestore(&work_lock, flags);
    /* Never call the console while holding the queue lock. Logarithmic reports
       avoid flooding serial output; the full counts remain queryable. */
    if (rejected_count && !(rejected_count & (rejected_count - 1)))
        pr_warn("ACPICA: deferred type %u rejected status=0x%x count=%llu\n",
                type, status, (unsigned long long)rejected_count);
    return status;
}

bool acpi_os_power_idle(void) {
    uint64_t flags = spin_lock_irqsave(&work_lock);
    bool idle = !work_pending(0) && !work_pending(1);
    spin_unlock_irqrestore(&work_lock, flags);
    return idle;
}

bool acpi_os_power_freeze(void) {
    uint64_t flags = spin_lock_irqsave(&work_lock);
    bool idle = !work_pending(0) && !work_pending(1);
    if (idle)
        work_frozen = true; /* idle test + admission close is one transaction */
    spin_unlock_irqrestore(&work_lock, flags);
    return idle;
}

void acpi_os_power_thaw(void) {
    uint64_t flags = spin_lock_irqsave(&work_lock);
    work_frozen = false;
    spin_unlock_irqrestore(&work_lock, flags);
}

void AcpiOsWaitEventsComplete(void) {
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&work_lock);
        bool done = !work_pending(0) && !work_pending(1);
        spin_unlock_irqrestore(&work_lock, flags);
        if (done)
            return;
        if (!sched_ready()) {
            /* Explicit boot drain only, never IRQ-time callback execution. */
            run_one_work(0);
            run_one_work(1);
        } else {
            wait_briefly();
        }
    }
}

ACPI_STATUS AcpiOsTerminate(void) {
    /* Backend queue shutdown only; full ACPICA unload is unsupported. */
    uint64_t flags = spin_lock_irqsave(&work_lock);
    work_stopping = true;
    spin_unlock_irqrestore(&work_lock, flags);
    AcpiOsWaitEventsComplete();
    /* Workers consult the immutable slot table until their exit. It remains a
       boot-lifetime reservation; full teardown must join workers before free. */
    return AE_OK;
}

ACPI_THREAD_ID AcpiOsGetThreadId(void) {
    if (!sched_ready())
        return 1;
    struct cpu_local *cpu = this_cpu();
    if (cpu->current && !cpu->current->idle)
        return (ACPI_THREAD_ID)cpu->current->pid + 1;
    return UINT64_C(0x100000000) + cpu->id;
}

UINT64 AcpiOsGetTimer(void) {
    return tsc_uptime_us() * 10; /* ACPICA uses 100 ns units. */
}

void AcpiOsStall(UINT32 microseconds) {
    uint64_t start = tsc_uptime_us();
    while (tsc_uptime_us() - start < microseconds)
        __asm__ volatile("pause" ::: "memory");
}

void AcpiOsSleep(UINT64 milliseconds) {
    while (milliseconds) {
        uint64_t part = milliseconds > 1000 ? 1000 : milliseconds;
        uint64_t start = tsc_uptime_us(), microseconds = part * 1000;
        for (;;) {
            uint64_t elapsed = tsc_uptime_us() - start;
            if (elapsed >= microseconds)
                break;
            uint64_t remaining = microseconds - elapsed;
            if (sched_ready() && interrupts_enabled())
                sched_sleep(remaining);
            else
                __asm__ volatile("pause" ::: "memory");
        }
        milliseconds -= part;
    }
}

ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS address, UINT32 *value, UINT32 width) {
    if (!value || (width != 8 && width != 16 && width != 32) ||
        address > UINT16_MAX || width / 8 - 1 > UINT16_MAX - address)
        return AE_BAD_PARAMETER;
    if (width == 8)
        *value = inb(address);
    else if (width == 16)
        *value = inw(address);
    else
        *value = indw(address);
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS address, UINT32 value, UINT32 width) {
    if ((width != 8 && width != 16 && width != 32) ||
        address > UINT16_MAX || width / 8 - 1 > UINT16_MAX - address)
        return AE_BAD_PARAMETER;
    if (width == 8)
        outb(address, value);
    else if (width == 16)
        outw(address, value);
    else
        outdw(address, value);
    return AE_OK;
}

/* x86 supports unaligned transfers. Packed, alias-safe lvalues preserve the
 * firmware-requested bus width without C alignment UB or byte-at-a-time I/O. */
struct acpi_io16 { uint16_t value; } __attribute__((packed, may_alias));
struct acpi_io32 { uint32_t value; } __attribute__((packed, may_alias));
struct acpi_io64 { uint64_t value; } __attribute__((packed, may_alias));

static uint64_t read_volatile(const volatile void *pointer, unsigned width) {
    if (width == 8)
        return *(const volatile uint8_t *)pointer;
    if (width == 16)
        return ((const volatile struct acpi_io16 *)pointer)->value;
    if (width == 32)
        return ((const volatile struct acpi_io32 *)pointer)->value;
    return ((const volatile struct acpi_io64 *)pointer)->value;
}

static void write_volatile(volatile void *pointer, uint64_t value, unsigned width) {
    if (width == 8)
        *(volatile uint8_t *)pointer = value;
    else if (width == 16)
        ((volatile struct acpi_io16 *)pointer)->value = value;
    else if (width == 32)
        ((volatile struct acpi_io32 *)pointer)->value = value;
    else
        ((volatile struct acpi_io64 *)pointer)->value = value;
}

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS address, UINT64 *value, UINT32 width) {
    if (!value || (width != 8 && width != 16 && width != 32 && width != 64))
        return AE_BAD_PARAMETER;
    void *pointer = AcpiOsMapMemory(address, width / 8);
    if (!pointer)
        return AE_BAD_ADDRESS;
    *value = read_volatile(pointer, width);
    return AE_OK;
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS address, UINT64 value, UINT32 width) {
    if (width != 8 && width != 16 && width != 32 && width != 64)
        return AE_BAD_PARAMETER;
    void *pointer = AcpiOsMapMemory(address, width / 8);
    if (!pointer)
        return AE_BAD_ADDRESS;
    write_volatile(pointer, value, width);
    return AE_OK;
}

/* Serialized CF8/CFC fallback permits ACPI PCI fields on machines without MCFG. */
static spinlock_t pci_lock;

static ACPI_STATUS pci_configuration(ACPI_PCI_ID *id, UINT32 reg, UINT64 *value,
                                     UINT32 width, bool write) {
    if (!id || !value || id->Bus > 255 || id->Device > 31 || id->Function > 7 ||
        (width != 8 && width != 16 && width != 32 && width != 64) ||
        reg >= 4096 || width / 8 > 4096 - reg || (reg & (width / 8 - 1)))
        return AE_BAD_PARAMETER;
    ACPI_TABLE_HEADER *header;
    ACPI_STATUS status = AcpiGetTable(ACPI_SIG_MCFG, 1, &header);
    if (ACPI_SUCCESS(status)) {
        if (header->Length < sizeof(ACPI_TABLE_MCFG) ||
            (header->Length - sizeof(ACPI_TABLE_MCFG)) % sizeof(ACPI_MCFG_ALLOCATION)) {
            AcpiPutTable(header);
            return AE_BAD_HEADER;
        }
        size_t count = (header->Length - sizeof(ACPI_TABLE_MCFG)) / sizeof(ACPI_MCFG_ALLOCATION);
        ACPI_MCFG_ALLOCATION *entries = (void *)((uint8_t *)header + sizeof(ACPI_TABLE_MCFG));
        for (size_t i = 0; i < count; i++) {
            ACPI_MCFG_ALLOCATION *entry = &entries[i];
            if (entry->PciSegment != id->Segment || id->Bus < entry->StartBusNumber ||
                id->Bus > entry->EndBusNumber || !entry->Address ||
                (entry->Address & UINT64_C(0xfffff)))
                continue;
            uint64_t offset = ((uint64_t)id->Bus << 20) | ((uint64_t)id->Device << 15) |
                              ((uint64_t)id->Function << 12);
            uint64_t base = entry->Address;
            AcpiPutTable(header);
            if (base > UINT64_MAX - offset)
                return AE_BAD_ADDRESS;
            void *function = AcpiOsMapMemory(base + offset, 4096);
            if (!function)
                return AE_BAD_ADDRESS;
            uint64_t flags = spin_lock_irqsave(&pci_lock);
            if (write) {
                /* PCI defines DWORD accesses, including a split 64-bit field. */
                write_volatile((uint8_t *)function + reg, *value, width == 64 ? 32 : width);
                if (width == 64)
                    write_volatile((uint8_t *)function + reg + 4, *value >> 32, 32);
            } else {
                *value = read_volatile((uint8_t *)function + reg, width == 64 ? 32 : width);
                if (width == 64)
                    *value |= read_volatile((uint8_t *)function + reg + 4, 32) << 32;
            }
            spin_unlock_irqrestore(&pci_lock, flags);
            return AE_OK;
        }
        AcpiPutTable(header);
    } else if (status != AE_NOT_FOUND) {
        return status;
    }
    if (id->Segment || reg >= 256 || width / 8 > 256 - reg)
        return AE_SUPPORT;
    uint64_t flags = spin_lock_irqsave(&pci_lock);
    uint32_t old_address = indw(0xcf8);
    unsigned parts = width == 64 ? 2 : 1;
    uint64_t data = 0;
    for (unsigned i = 0; i < parts; i++) {
        uint32_t offset = reg + i * 4;
        outdw(0xcf8, UINT32_C(0x80000000) | ((uint32_t)id->Bus << 16) |
                         ((uint32_t)id->Device << 11) | ((uint32_t)id->Function << 8) |
                         (offset & 0xfc));
        unsigned port = 0xcfc + (offset & 3);
        if (write)
            AcpiOsWritePort(port, *value >> (i * 32), width == 64 ? 32 : width);
        else {
            UINT32 part;
            AcpiOsReadPort(port, &part, width == 64 ? 32 : width);
            data |= (uint64_t)part << (i * 32);
        }
    }
    outdw(0xcf8, old_address);
    if (!write)
        *value = data;
    spin_unlock_irqrestore(&pci_lock, flags);
    return AE_OK;
}

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *id, UINT32 reg, UINT64 *value, UINT32 width) {
    return pci_configuration(id, reg, value, width, false);
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *id, UINT32 reg, UINT64 value, UINT32 width) {
    return pci_configuration(id, reg, &value, width, true);
}

static spinlock_t sci_lock;
static ACPI_OSD_HANDLER sci_handler;
static void *sci_context;
static volatile uint32_t *sci_ioapic;
static unsigned sci_redirection, sci_active;
static bool sci_removing;
static uint32_t sci_number;

static uint32_t ioapic_read(volatile uint32_t *mapping, unsigned reg) {
    mapping[0] = reg;
    return mapping[4];
}

static void ioapic_write(volatile uint32_t *mapping, unsigned reg, uint32_t value) {
    mapping[0] = reg;
    mapping[4] = value;
}

static void sci_interrupt(void) {
    uint64_t flags = spin_lock_irqsave(&sci_lock);
    ACPI_OSD_HANDLER handler = sci_handler;
    void *context = sci_context;
    if (handler)
        sci_active++;
    spin_unlock_irqrestore(&sci_lock, flags);
    if (handler) {
        handler(context);
        flags = spin_lock_irqsave(&sci_lock);
        sci_active--;
        spin_unlock_irqrestore(&sci_lock, flags);
    }
}

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 number, ACPI_OSD_HANDLER handler, void *context) {
    if (!handler)
        return AE_BAD_PARAMETER;
    ACPI_TABLE_HEADER *header;
    ACPI_STATUS status = AcpiGetTable(ACPI_SIG_MADT, 1, &header);
    if (ACPI_FAILURE(status))
        return status;
    if (header->Length < sizeof(ACPI_TABLE_MADT)) {
        AcpiPutTable(header);
        return AE_BAD_HEADER;
    }
    uint32_t gsi = number;
    uint16_t inti = 0;
    /* Validate every subtable before the two routing passes. */
    for (size_t offset = sizeof(ACPI_TABLE_MADT); offset < header->Length;) {
        ACPI_SUBTABLE_HEADER *sub = (void *)((uint8_t *)header + offset);
        if (header->Length - offset < sizeof(*sub) || sub->Length < sizeof(*sub) ||
            sub->Length > header->Length - offset ||
            (sub->Type == ACPI_MADT_TYPE_IO_APIC && sub->Length < sizeof(ACPI_MADT_IO_APIC)) ||
            (sub->Type == ACPI_MADT_TYPE_INTERRUPT_OVERRIDE &&
             sub->Length < sizeof(ACPI_MADT_INTERRUPT_OVERRIDE))) {
            AcpiPutTable(header);
            return AE_BAD_HEADER;
        }
        if (sub->Type == ACPI_MADT_TYPE_INTERRUPT_OVERRIDE) {
            ACPI_MADT_INTERRUPT_OVERRIDE *override = (void *)sub;
            if (!override->Bus && override->SourceIrq == number) {
                gsi = override->GlobalIrq;
                inti = override->IntiFlags;
            }
        }
        offset += sub->Length;
    }
    if ((inti & 3) == 2 || ((inti >> 2) & 3) == 2) {
        AcpiPutTable(header);
        return AE_BAD_VALUE;
    }
    uint32_t destination = apic_read(0x20);
    if (!(msr_read(0x1b) & 0x400))
        destination >>= 24;
    if (destination > 255) {
        AcpiPutTable(header);
        return AE_SUPPORT; /* Requires interrupt remapping, not available yet. */
    }
    status = AE_NOT_FOUND;
    for (size_t offset = sizeof(ACPI_TABLE_MADT); offset < header->Length;) {
        ACPI_SUBTABLE_HEADER *sub = (void *)((uint8_t *)header + offset);
        if (sub->Type == ACPI_MADT_TYPE_IO_APIC) {
            ACPI_MADT_IO_APIC *io = (void *)sub;
            if (gsi >= io->GlobalIrqBase) {
                if (!io->Address || (io->Address & 4095u)) {
                    status = AE_BAD_ADDRESS;
                    break;
                }
                volatile uint32_t *mapping = AcpiOsMapMemory(io->Address, 32);
                if (!mapping) {
                    status = AE_BAD_ADDRESS;
                    break;
                }
                uint64_t flags = spin_lock_irqsave(&sci_lock);
                unsigned maximum = (ioapic_read(mapping, 1) >> 16) & 255;
                if (gsi - io->GlobalIrqBase <= maximum && gsi - io->GlobalIrqBase < 120) {
                    if (sci_handler || sci_removing) {
                        status = AE_ALREADY_EXISTS;
                    } else {
                        sci_ioapic = mapping;
                        sci_redirection = 0x10 + (gsi - io->GlobalIrqBase) * 2;
                        uint32_t previous_low = ioapic_read(mapping, sci_redirection);
                        ioapic_write(mapping, sci_redirection, previous_low | (1u << 16));
                        sci_number = number;
                        sci_context = context;
                        sci_handler = handler;
                        register_interrupt_handler(ACPI_SCI_VECTOR, sci_interrupt);
                        uint32_t low = ACPI_SCI_VECTOR;
                        /* SCI conforms to active-low, level-triggered. */
                        if ((inti & 3) != 1)
                            low |= 1u << 13;
                        if (((inti >> 2) & 3) != 1)
                            low |= 1u << 15;
                        ioapic_write(mapping, sci_redirection + 1, destination << 24);
                        ioapic_write(mapping, sci_redirection, low);
                        status = AE_OK;
                        pr_info("ACPI: SCI IRQ %u GSI %u vector %u (%s, %s)\n", number, gsi,
                                ACPI_SCI_VECTOR, low & (1u << 13) ? "low" : "high",
                                low & (1u << 15) ? "level" : "edge");
                    }
                    spin_unlock_irqrestore(&sci_lock, flags);
                    break;
                }
                spin_unlock_irqrestore(&sci_lock, flags);
            }
        }
        offset += sub->Length;
    }
    AcpiPutTable(header);
    return status;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 number, ACPI_OSD_HANDLER handler) {
    uint64_t flags = spin_lock_irqsave(&sci_lock);
    if (!handler || handler != sci_handler || number != sci_number) {
        spin_unlock_irqrestore(&sci_lock, flags);
        return AE_BAD_PARAMETER;
    }
    ioapic_write(sci_ioapic, sci_redirection,
                 ioapic_read(sci_ioapic, sci_redirection) | (1u << 16));
    sci_handler = NULL;
    sci_removing = true;
    register_interrupt_handler(ACPI_SCI_VECTOR, NULL);
    spin_unlock_irqrestore(&sci_lock, flags);
    for (;;) {
        flags = spin_lock_irqsave(&sci_lock);
        bool done = sci_active == 0;
        spin_unlock_irqrestore(&sci_lock, flags);
        if (done)
            break;
        wait_briefly();
    }
    flags = spin_lock_irqsave(&sci_lock);
    /* interrupt_dispatch sends LAPIC EOI after sci_interrupt returns. Keep
       the masked route's vector unchanged so an outstanding remote EOI can
       still clear the I/O APIC's remote-IRR, including across reinstall. */
    sci_context = NULL;
    sci_ioapic = NULL;
    sci_removing = false;
    spin_unlock_irqrestore(&sci_lock, flags);
    return AE_OK;
}

void AcpiOsVprintf(const char *format, va_list arguments) {
    vprintk(format, arguments);
}

void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    AcpiOsVprintf(format, arguments);
    va_end(arguments);
}

ACPI_STATUS AcpiOsSignal(UINT32 function, void *info) {
    if (function == ACPI_SIGNAL_FATAL) {
        if (!info)
            return AE_BAD_PARAMETER;
        ACPI_SIGNAL_FATAL_INFO *fatal = info;
        panic("ACPICA fatal: type=%u code=%u argument=%u", fatal->Type, fatal->Code,
              fatal->Argument);
    }
    if (function == ACPI_SIGNAL_BREAKPOINT) {
        pr_debug("ACPICA breakpoint: %s\n", info ? (const char *)info : "");
        return AE_OK;
    }
    return AE_BAD_PARAMETER;
}

ACPI_STATUS AcpiOsEnterSleep(UINT8 state, UINT32 register_a, UINT32 register_b) {
    (void)state;
    (void)register_a;
    (void)register_b;
    return AE_OK; /* Continue ACPICA's hardware sleep-register writes. */
}
