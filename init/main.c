#include <kernel.h>
#include <descriptor_tables/gdt.h>
#include <descriptor_tables/idt.h>
#include <interrupts/lapic.h>
#include <acpi/acpi.h>
#include <acpi/osl.h>
#include <acpi/telemetry.h>
#include <power.h>
#include <mm/pmm.h>
#include <mm/vmem.h>
#include <mm/user.h>
#include <drivers/timer/hpet.h>
#include <drivers/pci/pci.h>
#include <sched.h>
#include <net/network.h>
#include <fs/vfs.h>
#include <drivers/audio.h>
#include <tty.h>
#include <tsc.h>
#include <usb_input.h>
#include <video/fbcon.h>
#include <panic.h>
#include <boot/info.h>

int64_t kernel_boot_epoch;

void kernel_main(void) {
    gdt_init();
    idt_init();
    user_cpu_init();

    tsc_init();
    printk_set_uptime_fn(tsc_uptime_us);
    if (boot_info.epoch > 0)
        kernel_boot_epoch = boot_info.epoch;

    pmm_init(boot_info.memory_map, boot_info.memory_map_count);
    struct boot_framebuffer *fb = boot_info.framebuffer;
    vmem_init(fb ? (uintptr_t)fb->address : 0, fb ? fb->pitch * fb->height : 0);

    if (fb)
        fbcon_init(fb);
    pr_notice("Coast 0.3: SMP, userland and filesystems\n");

    if (boot_info.rsdp)
        acpi_init(boot_info.rsdp);
    hpet_init();
    apic_init();
    pci_init();
    if (boot_info.rsdp)
        acpi_start();

    if (!kernel_usb_input_start())
        pr_warn("USB input unavailable; serial console remains active\n");

    vfs_init(boot_info.modules, boot_info.module_count);
    vfs_discover();
    audio_init();

    if (boot_info.rsdp) {
        uint64_t boot_irq_flags;
        __asm__ volatile("pushfq; popq %0" : "=r"(boot_irq_flags));
        if (!(boot_irq_flags & 0x200))
            panic("ACPICA: SCI interrupts disabled by a boot consumer");
        pr_info("ACPICA: boot consumers completed with SCI interrupts enabled (IF=1)\n");
    }
    /* Boot AML needs SCI delivery, but SMP/scheduler setup expects CLI. */
    __asm__ volatile("cli" ::: "memory");
    sched_init(boot_info.smp);
#if CONFIG_ELECTRON_PROFILE_SELFTEST
    extern void electron_budget_selftest(void);
    electron_budget_selftest();
#endif
#if CONFIG_RAMFS_SHARED_WRITE_VERIFY
    extern void ramfs_shared_write_selftest(void);
    ramfs_shared_write_selftest();
#if CONFIG_RAMFS_PARTIAL_WRITE_TEST
    extern void ramfs_partial_selftest(void);
    ramfs_partial_selftest();
#endif
#endif
    char *args[] = {"/bin/coast-sh", NULL};
    int pid = process_spawn("/bin/coast-sh", args, 0, "/", 0, true);
    if (pid < 0)
        panic("cannot launch /bin/coast-sh from initramfs: %d", pid);

    /* Preserve the initial user shell as PID 1. */
    if (boot_info.rsdp && !acpi_os_start_workers())
        panic("cannot create ACPICA event workers");
    if (boot_info.rsdp && !acpi_selftest_start())
        panic("cannot create ACPICA selftest task");

    if (boot_info.rsdp && !acpi_telemetry_start())
        pr_warn("ACPI power telemetry worker unavailable\n");
    if (!power_start(boot_info.rsdp != NULL))
        panic("cannot create power coordinator");

#if CONFIG_RAMFS_SHARED_WRITE_VERIFY && CONFIG_RAMFS_PARTIAL_WRITE_TEST
    extern void ramfs_shared_write_stress_start(void);
    ramfs_shared_write_stress_start();
#endif
    network_start();
    if (task_kernel("input", input_task, NULL, 0) < 0)
        panic("cannot create input task");

    pr_notice("System ready: serial and USB shell; type help\n");
    sched_start();
    __builtin_unreachable();
}
