#include <kernel.h>
#include <descriptor_tables/gdt.h>
#include <descriptor_tables/idt.h>
#include <interrupts/lapic.h>
#include <acpi/acpi.h>
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
    pr_notice("ModernOS 0.3: SMP, userland and filesystems\n");

    if (boot_info.rsdp)
        acpi_init(boot_info.rsdp);
    hpet_init();
    apic_init();
    pci_init();

    if (!kernel_usb_input_start())
        pr_warn("USB input unavailable; serial console remains active\n");

    vfs_init(boot_info.modules, boot_info.module_count);
    vfs_discover();
    audio_init();

    sched_init(boot_info.smp);
    char *args[] = {"/bin/modernos-sh", NULL};
    int pid = process_spawn("/bin/modernos-sh", args, 0, "/", 0, true);
    if (pid < 0)
        panic("cannot launch /bin/modernos-sh from initramfs: %d", pid);

    network_start();
    if (task_kernel("input", input_task, NULL, 0) < 0)
        panic("cannot create input task");

    pr_notice("System ready: serial and USB shell; type help\n");
    sched_start();
    __builtin_unreachable();
}
