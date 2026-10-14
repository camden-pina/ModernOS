#include <boot/info.h>
#include <printk.h>
#include <panic.h>

struct boot_info boot_info;

void boot_entry(const struct boot_info *info) {
    if ((uintptr_t)info < BOOT_HHDM_BASE || info->magic != BOOT_INFO_MAGIC ||
        info->version != BOOT_INFO_VERSION)
        panic("unsupported or missing boot handoff");

    boot_info = *info;
    if (!boot_info.hhdm_offset || !boot_info.memory_map || !boot_info.memory_map_count ||
        boot_info.memory_map_count > BOOT_MAX_MEMMAP || boot_info.module_count > BOOT_MAX_MODULES ||
        (boot_info.module_count && !boot_info.modules))
        panic("invalid boot memory or module handoff");

    /* Allocators require sorted, disjoint physical ranges, regardless of
       firmware descriptor order or boot protocol. Preserve reserved regions. */
    for (uint64_t i = 1; i < boot_info.memory_map_count; i++) {
        struct boot_memory_range range = boot_info.memory_map[i];
        uint64_t j = i;

        while (j && boot_info.memory_map[j - 1].base > range.base) {
            boot_info.memory_map[j] = boot_info.memory_map[j - 1];
            --j;
        }

        boot_info.memory_map[j] = range;
    }

    uint64_t end = 0;
    for (uint64_t i = 0; i < boot_info.memory_map_count; i++) {
        struct boot_memory_range *range = &boot_info.memory_map[i];
        if (!range->length || range->length > UINT64_MAX - range->base || range->base < end)
            panic("invalid or overlapping boot memory ranges");
        end = range->base + range->length;
    }

    pr_info("Boot: %s, unified handoff v%llu\n",
            boot_info.loader_name ? boot_info.loader_name : "unknown",
            (unsigned long long)boot_info.version);
    kernel_main();
}
