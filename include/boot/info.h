#ifndef MODERNOS_BOOT_INFO_H
#define MODERNOS_BOOT_INFO_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Private, versioned handoff shared by the EFI stub and kernel adapters.
 * All pointers are kernel virtual addresses and remain reserved in the map. */
#define BOOT_INFO_MAGIC UINT64_C(0x4d4f53424f4f5431)
#define BOOT_INFO_VERSION 1
#define BOOT_MAX_MEMMAP 2048
#define BOOT_MAX_MODULES 32
#define BOOT_HHDM_BASE UINT64_C(0xffff800000000000)

enum boot_memory_type {
    BOOT_MEMORY_USABLE,
    BOOT_MEMORY_RESERVED,
    BOOT_MEMORY_ACPI_RECLAIMABLE,
    BOOT_MEMORY_ACPI_NVS,
    BOOT_MEMORY_BAD,
    BOOT_MEMORY_RECLAIMABLE,
    BOOT_MEMORY_KERNEL,
    BOOT_MEMORY_FRAMEBUFFER
};

struct boot_memory_range {
    uint64_t base, length, type;
};

struct boot_module {
    void *address;
    uint64_t size;
    const char *path, *string;
};

struct boot_framebuffer {
    void *address;
    uint64_t width, height, pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size, red_mask_shift, green_mask_size, green_mask_shift;
    uint8_t blue_mask_size, blue_mask_shift;
};

struct boot_smp;
struct boot_info {
    uint64_t magic, version, hhdm_offset;
    struct boot_memory_range *memory_map;
    uint64_t memory_map_count;
    struct boot_module *modules;
    uint64_t module_count;
    struct boot_framebuffer *framebuffer;
    void *rsdp;
    int64_t epoch;
    const char *loader_name, *cmdline;
    struct boot_smp *smp;
    uint64_t ap_trampoline; /* Reserved low physical pages for native x86 APs. */
};

extern struct boot_info boot_info;
void boot_entry(const struct boot_info *info) __attribute__((__noreturn__));
void kernel_main(void) __attribute__((__noreturn__));
#endif
