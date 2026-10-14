#include <boot/info.h>
#include <boot/smp.h>
#include <acpi/acpi.h>
#include <acpi/tables.h>
#include <cpu.h>
#include <interrupts/lapic.h>
#include <mm/pmm.h>
#include <mm/user.h>
#include <mm/vmem_io.h>
#include <printk.h>
#include <string.h>
#include <tsc.h>

/* Native EFI entry leaves AP startup to the kernel, using the same discovery
 * and INIT/SIPI sequence regardless of which EFI boot manager loaded it. */
struct native_ap {
    boot_ap_entry entry;
};

static struct boot_cpu native_cpus[MAX_CPUS];
static struct native_ap native_contexts[MAX_CPUS];
static struct boot_smp native_smp;
static uint64_t trampoline_physical;
static bool x2apic;
extern const unsigned char boot_ap_trampoline_start[], boot_ap_trampoline_end[];
extern const unsigned char boot_ap_trampoline_gdt[], boot_ap_trampoline_gdt_base[];
extern const unsigned char boot_ap_trampoline_pm_target[], boot_ap_trampoline_pm_entry[];
extern const unsigned char boot_ap_trampoline_lm_target[], boot_ap_trampoline_lm_entry[];
extern const unsigned char boot_ap_trampoline_cr3[], boot_ap_trampoline_stack[];
extern const unsigned char boot_ap_trampoline_cpu[], boot_ap_trampoline_argument[];
extern const unsigned char boot_ap_trampoline_entry[];

static uint64_t msr_read(unsigned n) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(n));
    return lo | ((uint64_t)hi << 32);
}

static void msr_write(unsigned n, uint64_t v) {
    __asm__ volatile("wrmsr" ::"c"(n), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

static void delay_us(uint64_t us) {
    uint64_t start = tsc_uptime_us();
    while (tsc_uptime_us() - start < us)
        __asm__ volatile("pause");
}

static bool ipi(unsigned lapic_id, unsigned command) {
    if (x2apic) {
        msr_write(0x830, ((uint64_t)lapic_id << 32) | command);
        return true;
    }
    uint64_t start = tsc_uptime_us();
    while (apic_read(0x300) & (1u << 12)) {
        if (tsc_uptime_us() - start > 100000)
            return false;
        __asm__ volatile("pause");
    }
    apic_write(0x310, lapic_id << 24);
    apic_write(0x300, command);
    start = tsc_uptime_us();
    while (apic_read(0x300) & (1u << 12)) {
        if (tsc_uptime_us() - start > 100000)
            return false;
        __asm__ volatile("pause");
    }
    return true;
}

static void patch(const unsigned char *field, uint64_t value, size_t size) {
    size_t offset = (uintptr_t)field - (uintptr_t)boot_ap_trampoline_start;
    memcpy((unsigned char *)phys_ptr(trampoline_physical) + offset, &value, size);
}

static void native_ap_entry(struct boot_cpu *cpu, uintptr_t argument) {
    /* AP INIT resets x2APIC mode; match the BSP before shared APIC helpers run. */
    if (x2apic)
        msr_write(0x1b, msr_read(0x1b) | 0xc00);
    struct native_ap *context = cpu->loader_data;
    context->entry(cpu, argument);
    for (;;)
        __asm__ volatile("cli; hlt");
}

static bool native_start(struct boot_smp *smp, struct boot_cpu *cpu, boot_ap_entry entry,
                         uintptr_t argument, uintptr_t stack_top) {
    (void)smp;
    struct native_ap *context = cpu->loader_data;
    context->entry = entry;
    patch(boot_ap_trampoline_stack, stack_top, 8);
    patch(boot_ap_trampoline_cpu, (uintptr_t)cpu, 8);
    patch(boot_ap_trampoline_argument, argument, 8);
    patch(boot_ap_trampoline_entry, (uintptr_t)native_ap_entry, 8);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (!ipi(cpu->lapic_id, 0xc500))
        return false; /* INIT, level assert. */
    delay_us(10000);
    if (!ipi(cpu->lapic_id, 0x8500))
        return false; /* INIT, level deassert. */
    delay_us(200);
    unsigned vector = trampoline_physical >> 12;
    if (!ipi(cpu->lapic_id, 0x600 | vector))
        return false;
    delay_us(200);
    return ipi(cpu->lapic_id, 0x600 | vector);
}

static bool prepare_trampoline(void) {
    uint64_t base = boot_info.ap_trampoline;
    size_t size = (uintptr_t)boot_ap_trampoline_end - (uintptr_t)boot_ap_trampoline_start;
    /* Four consecutive firmware-reserved pages hold code and startup paging.
     * The EFI adapter reserves five pages so its low allocation stays simple. */
    if (!base || (base & (PAGE_SIZE - 1)) || base + 4 * PAGE_SIZE > 0x100000 || size > PAGE_SIZE)
        return false;
    trampoline_physical = base;
    for (unsigned i = 0; i < 4; i++)
        if (!vmem_map_page_in_root(kernel_cr3(), (uintptr_t)phys_ptr(base + i * PAGE_SIZE),
                                   base + i * PAGE_SIZE))
            return false;
    memset(phys_ptr(base), 0, 4 * PAGE_SIZE);
    memcpy(phys_ptr(base), boot_ap_trampoline_start, size);
    uint64_t *root = phys_ptr(base + PAGE_SIZE);
    uint64_t *pdpt = phys_ptr(base + 2 * PAGE_SIZE);
    uint64_t *pd = phys_ptr(base + 3 * PAGE_SIZE);
    uint64_t *kernel = phys_ptr(kernel_cr3());
    memcpy(root + 256, kernel + 256, 256 * sizeof(*root));
    root[0] = (base + 2 * PAGE_SIZE) | 3;
    pdpt[0] = (base + 3 * PAGE_SIZE) | 3;
    pd[0] = 0x83; /* Identity map only the low 2 MiB during the mode transition. */
    patch(boot_ap_trampoline_gdt_base,
          base + ((uintptr_t)boot_ap_trampoline_gdt - (uintptr_t)boot_ap_trampoline_start), 4);
    patch(boot_ap_trampoline_pm_target,
          base + ((uintptr_t)boot_ap_trampoline_pm_entry - (uintptr_t)boot_ap_trampoline_start), 4);
    patch(boot_ap_trampoline_lm_target,
          base + ((uintptr_t)boot_ap_trampoline_lm_entry - (uintptr_t)boot_ap_trampoline_start), 4);
    patch(boot_ap_trampoline_cr3, base + PAGE_SIZE, 8);
    return true;
}

static void add_cpu(unsigned processor_id, unsigned lapic_id) {
    if (!x2apic && lapic_id > 255)
        return;
    for (uint64_t i = 0; i < native_smp.cpu_count; i++)
        if (native_cpus[i].lapic_id == lapic_id)
            return;
    if (native_smp.cpu_count == MAX_CPUS)
        return;
    unsigned i = native_smp.cpu_count++;
    native_cpus[i] = (struct boot_cpu){processor_id, lapic_id, &native_contexts[i]};
}

struct boot_smp *boot_smp_native_init(void) {
    memset(&native_smp, 0, sizeof(native_smp));
    x2apic = (msr_read(0x1b) & 0x400) != 0;
    unsigned bsp_id = apic_read(0x20);
    if (!x2apic)
        bsp_id >>= 24;
    native_smp.bsp_lapic_id = bsp_id;
    native_smp.cpus = native_cpus;
    native_smp.start = native_start;
    add_cpu(0, bsp_id);

    _acpi_sdt_hdr_t *madt = acpi_locate_table("APIC");
    if (!madt || madt->len < 44)
        return &native_smp;

    const uint8_t *p = (const uint8_t *)madt + 44, *end = (const uint8_t *)madt + madt->len;
    while (end - p >= 2) {
        unsigned type = p[0], length = p[1];
        if (length < 2 || (size_t)(end - p) < length)
            break;
        uint32_t flags, id, uid;
        if (type == 0 && length >= 8) {
            memcpy(&flags, p + 4, 4);
            if (flags & 1)
                add_cpu(p[2], p[3]);
        } else if (type == 9 && length >= 16) {
            memcpy(&id, p + 4, 4);
            memcpy(&flags, p + 8, 4);
            memcpy(&uid, p + 12, 4);
            if (flags & 1)
                add_cpu(uid, id);
        }
        p += length;
    }

    if (native_smp.cpu_count > 1 && !prepare_trampoline()) {
        pr_warn("boot: AP startup memory unavailable; using BSP only\n");
        native_smp.cpu_count = 1;
    }

    return &native_smp;
}
