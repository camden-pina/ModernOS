#ifndef _KERNEL_H
#define _KERNEL_H 1

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <printk.h>

// Contains definitions for the data structures given
// to the kernel by the bootloader. The data structures
// include information about system memory, devices.
#define __boot_data __attribute__((section(".boot_data")))

//#include "Sys32/system.h"
// Register structs for interrupt/exception
typedef struct _register_t {
    unsigned long long r15, r14, r13, r12, r11, r10, r9, r8;
    unsigned long long rdi, rsi, rbp, rbx, rdx, rcx, rax;
    unsigned long long interrupt, errorcode;
    unsigned long long rip, cs, rflags, user_rsp, ss;
} register_t;

// Register structs for bios service
typedef struct register16 {
    unsigned short di;
    unsigned short si;
    unsigned short bp;
    unsigned short sp;
    unsigned short bx;
    unsigned short dx;
    unsigned short cx;
    unsigned short ax;

    unsigned short ds;
    unsigned short es;
    unsigned short fs;
    unsigned short gs;
    unsigned short ss;
    unsigned short eflags;
} register16_t;

#define __in    // identifies a pointer parameter as an input parameter
#define __out   // identifies a pointer parameter as an output parameter
#define __inout // identifies a pointer parameter as an input/output parameter (modified)

//
// General Definitions
//

#define MS_PER_SEC 1000LL
#define US_PER_SEC 1000000LL
#define NS_PER_SEC 1000000000LL
#define NS_PER_USEC 1000
#define FS_PER_SEC 1000000000000000

#define MS_TO_NS(ms) ((uint64_t)(ms) * (NS_PER_SEC / MS_PER_SEC))
#define US_TO_NS(us) ((uint64_t)(us) * (NS_PER_SEC / US_PER_SEC))
#define FS_TO_NS(fs) ((uint64_t)(fs) / (FS_PER_SEC / NS_PER_SEC))
#define MS_TO_US(ms) ((uint64_t)(ms) * (US_PER_SEC / MS_PER_SEC))

#define SIZE_1KB 0x400ULL
#define SIZE_2KB 0x800ULL
#define SIZE_4KB 0x1000ULL
#define SIZE_8KB 0x2000ULL
#define SIZE_16KB 0x4000ULL
#define SIZE_1MB 0x100000ULL
#define SIZE_2MB 0x200000ULL
#define SIZE_4MB 0x400000ULL
#define SIZE_8MB 0x800000ULL
#define SIZE_16MB 0x1000000ULL
#define SIZE_1GB 0x40000000ULL
#define SIZE_2GB 0x80000000ULL
#define SIZE_4GB 0x100000000ULL
#define SIZE_8GB 0x200000000ULL
#define SIZE_16GB 0x400000000ULL
#define SIZE_1TB 0x10000000000ULL

//
// General Macros
//

#define static_assert(expr) _Static_assert(expr, "")

#define offset_ptr(p, c) ((void *)(((uintptr_t)(p)) + (c)))
#define offset_addr(p, c) (((uintptr_t)(p)) + (c))
#define align(v, a) ((v) + (((a) - (v)) & ((a) - 1)))
#define align_down(v, a) ((v) & ~((a) - 1))
#define page_align(v) align(v, PAGE_SIZE)
#define page_trunc(v) align_down(v, PAGE_SIZE)
#define is_aligned(v, a) (((v) & ((a) - 1)) == 0)
#define is_pow2(v) (((v) & ((v) - 1)) == 0)
#define prev_pow2(v) (1 << ((sizeof(v) * 8 - 1) - __builtin_clz(v)))
#define next_pow2(v) (1 << ((sizeof(v) * 8 - __builtin_clz((v) - 1))))
#define align_ptr(p, a) ((void *)(align((uintptr_t)(p), (a))))
#define ptr_after(s) ((void *)(((uintptr_t)(s)) + (sizeof(*(s)))))

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define abs(a) (((a) < 0) ? (-(a)) : (a))
#define diff(a, b) abs((a) - (b))
#define udiff(a, b) (max(a, b) - min(a, b))

#define moveptr(objptr) ({ typeof(objptr) __tmp = (objptr); (objptr) = NULL; __tmp; })

#define LABEL(l) l: NULL

#define cpu_pause() __asm volatile("pause" ::: "memory");
#define WHILE_TRUE ({ while (true) cpu_pause(); })

#define ASSERT_IS_TYPE(type, value)                           \
    _Static_assert(_Generic(value, type: 1, default: 0) == 1, \
                   "Failed type assertion: " #value " is not of type " #type)

#define __expect_true(expr) __builtin_expect((expr), 1)
#define __expect_false(expr) __builtin_expect((expr), 0)

#define todo(msg) kprintf("TODO: %s:%d: %s\n", __FILE__, __LINE__, #msg); WHILE_TRUE

#define __type_checked(type, param, rest) ({ ASSERT_IS_TYPE(type, param); rest; })
#define __const_type_checked(type, param, rest) ({ ASSERT_IS_TYPE_OR_CONST(type, param); rest; })

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

//
// Compiler Attributes
//

#define noreturn _Noreturn

// #define packed __attribute((packed))
#define noinline __attribute((noinline))

// #define always_inline inline __attribute__((always_inline))
#define __aligned(val) __attribute((aligned((val))))
#define deprecated __attribute((deprecated))
#define warn_unused_result __attribute((warn_unused_result))

#define weak __attribute((weak))
#define UNUSED __attribute((unused))
#define alias(name) __attribute((alias(name)))
#define __used __attribute((used))
#define __likely(expr) __builtin_expect((expr), 1)
#define __unlikely(expr) __builtin_expect((expr), 0)

#define __malloc_like __attribute((malloc))

/**
 * The STATIC_INIT macro provides a way to register initializer functions that are invoked
 * at the end of the 'static' phase. These functions may only use the memory, time, and
 * irq APIs, and are called from within the proc0 context.
 */
#define STATIC_INIT(fn) static __attribute__((section(".init_array.static"))) void (*__do_static_init_##fn)() = fn

typedef struct {
    unsigned char Magic[2];
    unsigned char Mode;
    unsigned char CharacterSize;
} psf1_header;

typedef struct {
    psf1_header *header;
    void *GlyphBuffer;
} psf1_font;

typedef struct {
    void *base_addr;
    size_t buffer_sz;
    unsigned int px_width;
    unsigned int px_height;
    unsigned int bpp;
    unsigned int pps;
} framebuffer;

typedef struct flexboot_header_t {
    // Existing fields…
    void *memoryMap;
    unsigned long long mapSize;
    unsigned long long mapKey;
    unsigned long long descriptorSize;
    unsigned long long descriptorVersion;

    // GOP info
    framebuffer *fb;

    psf1_font *font;

    // ACPI
    unsigned long long *rsdp_addr;

    // New: Kernel relocation offset (VMA - LMA)
    unsigned long long kernel_offset;
} flexboot_header_t;

typedef void (*KernelMainFunc)(flexboot_header_t *header);

#define __boot_data __attribute__((section(".boot_data")))

#define KERNEL_MAX_SIZE SIZE_2MB

// Memory map
#define MEMORY_UNKNOWN 0
#define MEMORY_UNUSABLE 1
#define MEMORY_USABLE 2
#define MEMORY_RESERVED 3
#define MEMORY_ACPI 4
#define MEMORY_ACPI_NVS 5
#define MEMORY_MAPPED_IO 6
#define MEMORY_EFI_RUNTIME_CODE 7
#define MEMORY_EFI_RUNTIME_DATA 8

// Framebuffer pixel format
#define FB_PIXEL_FORMAT_UNKNOWN 0x0
#define FB_PIXEL_FORMAT_RGB 0x1
#define FB_PIXEL_FORMAT_BGR 0x2

#define BOOT_MAGIC "BOOT"
#define BOOT_MAGIC0 'B'
#define BOOT_MAGIC1 'O'
#define BOOT_MAGIC2 'O'
#define BOOT_MAGIC3 'T'

// Section Loading

typedef struct loaded_section {
    const char *name;
    uintptr_t phys_addr;
    uintptr_t virt_addr;
    size_t size;
} loaded_section_t;

// Framebuffer

#define PIXEL_RGB 0
#define PIXEL_BGR 1
#define PIXEL_BITMASK 2

typedef struct pixel_bitmask {
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t reserved_mask;
} pixel_bitmask_t;

// Memory Map

typedef struct memory_map_entry {
    uint32_t type;
    uint32_t : 32; // reserved
    uint64_t base;
    uint64_t size;
} memory_map_entry_t;

typedef struct memory_map {
    uint32_t size;           // size of the memory map
    uint32_t capacity;       // size allocated for the memory map
    memory_map_entry_t *map; // pointer to the memory map
} memory_map_t;

// The full boot structure

typedef struct boot_info_v2 {
    uint8_t magic[4]; // boot signature ('BOOT')
    // kernel info
    uint32_t kernel_phys_addr; // kernel physical address
    uint64_t kernel_virt_addr; // kernel virtual address
    uint32_t kernel_size;      // kernel size in bytes
    uint32_t pml4_addr;        // pml4 table address
    // memory info
    uint64_t mem_total;   // total memory
    memory_map_t mem_map; // system memory map
    // framebuffer
    uint64_t fb_addr;         // framebuffer base address
    uint64_t fb_size;         // framebuffer size in bytes
    uint32_t fb_width;        // framebuffer width
    uint32_t fb_height;       // framebuffer height
    uint32_t fb_pixel_format; // framebuffer pixel format
    uint32_t : 32;            // reserved
    // initrd
    uint64_t initrd_addr; // initrd address
    uint64_t initrd_size; // initrd size in bytes
    // system configuration
    uint32_t efi_runtime_services; // EFI Runtime Services table
    uint32_t acpi_ptr;             // ACPI RDSP table address
    uint32_t smbios_ptr;           // SMBIOS Entry Point table address
    uint32_t : 32;                 // reserved
} boot_info_v2_t;

//
// Global Symbols
//

extern boot_info_v2_t *boot_info_v2;
extern uint32_t system_num_cpus;
extern bool is_smp_enabled;
extern bool is_debug_enabled;

// linker provided symbols
extern uintptr_t __kernel_address;
extern uintptr_t __kernel_virtual_offset;
extern uintptr_t __kernel_code_start;
extern uintptr_t __kernel_code_end;
extern uintptr_t __kernel_data_end;

extern boot_info_v2_t *boot_info_v2;

void kern_main(boot_info_v2_t *header);

#endif
