#include <boot/info.h>
#include <printk.h>

/* Protocol selection belongs to the architecture entry layer. The common
 * kernel validates only its versioned handoff and never reads Limine structs. */
bool boot_limine_available(void);
void boot_limine_init(void);
void arch_boot_entry(const struct boot_info *info)
    __attribute__((force_align_arg_pointer, __noreturn__));

void arch_boot_entry(const struct boot_info *info) {
    __asm__ volatile("cli; cld" ::: "memory");
    printk_early_init();
    /* Limine leaves argument registers unspecified. Probe its responses before
       interpreting RDI as the native EFI adapter's handoff pointer. */
    if (boot_limine_available()) {
        boot_limine_init();
        info = &boot_info;
    }
    boot_entry(info);
}
