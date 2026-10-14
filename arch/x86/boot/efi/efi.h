#ifndef MODERNOS_EFI_H
#define MODERNOS_EFI_H

/* The x64 UEFI ABI uses the Microsoft calling convention. These definitions
 * intentionally cover only the protocols needed by this freestanding loader. */
#include <stdint.h>
#include <stddef.h>

typedef uint64_t efi_status;
typedef void *efi_handle;
typedef uint16_t char16;
typedef struct {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
} efi_guid;

typedef struct {
    uint64_t signature;
    uint32_t revision, size, crc, reserved;
} efi_table_header;

#define EFI_ERROR(s) (((s) >> 63) != 0)
#define EFI_INVALID_PARAMETER (UINT64_C(0x8000000000000002))
#define EFI_BUFFER_TOO_SMALL (UINT64_C(0x8000000000000005))
#define EFI_NOT_FOUND (UINT64_C(0x800000000000000e))
#define EFI_LOAD_ERROR (UINT64_C(0x8000000000000001))
#define EFI_OUT_OF_RESOURCES (UINT64_C(0x8000000000000009))
#define EFI_UNSUPPORTED (UINT64_C(0x8000000000000003))
#define EFI_PAGE_SIZE 4096

enum { EFI_ALLOCATE_ANY, EFI_ALLOCATE_MAX, EFI_ALLOCATE_ADDRESS };
enum {
    EFI_RESERVED,
    EFI_LOADER_CODE,
    EFI_LOADER_DATA,
    EFI_BOOT_CODE,
    EFI_BOOT_DATA,
    EFI_RUNTIME_CODE,
    EFI_RUNTIME_DATA,
    EFI_CONVENTIONAL,
    EFI_UNUSABLE,
    EFI_ACPI_RECLAIM,
    EFI_ACPI_NVS,
    EFI_MMIO,
    EFI_MMIO_PORT,
    EFI_PAL_CODE,
    EFI_PERSISTENT,
    EFI_UNACCEPTED
};

struct efi_memory_descriptor {
    uint32_t type, pad;
    uint64_t physical, virtual_address, pages, attributes;
};

struct efi_text_output {
    void *reset;
    efi_status (*output_string)(struct efi_text_output *, const char16 *);
    void *test_string, *query_mode, *set_mode, *set_attribute;
    void *clear_screen, *set_cursor_position, *enable_cursor, *mode;
};

struct efi_boot_services {
    efi_table_header header;
    void *raise_tpl, *restore_tpl;
    efi_status (*allocate_pages)(uint32_t, uint32_t, size_t, uint64_t *);
    efi_status (*free_pages)(uint64_t, size_t);
    efi_status (*get_memory_map)(size_t *, void *, size_t *, size_t *, uint32_t *);
    efi_status (*allocate_pool)(uint32_t, size_t, void **);
    efi_status (*free_pool)(void *);
    void *create_event, *set_timer, *wait_for_event, *signal_event, *close_event, *check_event;
    void *install_protocol_interface, *reinstall_protocol_interface, *uninstall_protocol_interface;
    efi_status (*handle_protocol)(efi_handle, const efi_guid *, void **);
    void *reserved, *register_protocol_notify, *locate_handle, *locate_device_path;
    void *install_configuration_table, *load_image, *start_image, *exit, *unload_image;
    efi_status (*exit_boot_services)(efi_handle, size_t);
    void *get_next_monotonic_count, *stall;
    efi_status (*set_watchdog_timer)(size_t, uint64_t, size_t, char16 *);
    void *connect_controller, *disconnect_controller, *open_protocol;
    void *close_protocol, *open_protocol_information, *protocols_per_handle;
    efi_status (*locate_handle_buffer)(uint32_t, const efi_guid *, void *, size_t *, efi_handle **);
    efi_status (*locate_protocol)(const efi_guid *, void *, void **);
};

struct efi_time {
    uint16_t year;
    uint8_t month, day, hour, minute, second, pad1;
    uint32_t nanosecond;
    int16_t timezone;
    uint8_t daylight, pad2;
};

struct efi_runtime_services {
    efi_table_header header;
    efi_status (*get_time)(struct efi_time *, void *);
};

struct efi_configuration_table {
    efi_guid guid;
    void *table;
};

struct efi_system_table {
    efi_table_header header;
    char16 *firmware_vendor;
    uint32_t firmware_revision;
    efi_handle console_in_handle;
    void *console_in;
    efi_handle console_out_handle;
    struct efi_text_output *console_out;
    efi_handle stderr_handle;
    struct efi_text_output *stderr;
    struct efi_runtime_services *runtime;
    struct efi_boot_services *boot;
    size_t table_count;
    struct efi_configuration_table *tables;
};

struct efi_loaded_image {
    uint32_t revision;
    efi_handle parent;
    struct efi_system_table *system;
    efi_handle device;
    void *file_path, *reserved;
    uint32_t options_size;
    void *options, *image_base;
    uint64_t image_size;
    uint32_t code_type, data_type;
    void *unload;
};

struct efi_file {
    uint64_t revision;
    efi_status (*open)(struct efi_file *, struct efi_file **, const char16 *, uint64_t, uint64_t);
    efi_status (*close)(struct efi_file *);
    void *delete_file;
    efi_status (*read)(struct efi_file *, size_t *, void *);
    void *write, *get_position;
    efi_status (*set_position)(struct efi_file *, uint64_t);
    efi_status (*get_info)(struct efi_file *, const efi_guid *, size_t *, void *);
};

struct efi_simple_filesystem {
    uint64_t revision;
    efi_status (*open_volume)(struct efi_simple_filesystem *, struct efi_file **);
};

struct efi_file_info {
    uint64_t size, file_size, physical_size;
    struct efi_time create_time, access_time, modification_time;
    uint64_t attribute;
    char16 name[1];
};

struct efi_pixel_masks {
    uint32_t red, green, blue, reserved;
};

struct efi_gop_info {
    uint32_t version, width, height, format;
    struct efi_pixel_masks masks;
    uint32_t pixels_per_line;
};

struct efi_gop_mode {
    uint32_t max_mode, mode;
    struct efi_gop_info *info;
    size_t info_size;
    uint64_t framebuffer;
    size_t framebuffer_size;
};

struct efi_gop {
    efi_status (*query_mode)(struct efi_gop *, uint32_t, size_t *, struct efi_gop_info **);
    efi_status (*set_mode)(struct efi_gop *, uint32_t);
    void *blt;
    struct efi_gop_mode *mode;
};

_Static_assert(offsetof(struct efi_system_table, boot) == 96, "UEFI system table layout");
_Static_assert(offsetof(struct efi_boot_services, exit_boot_services) == 232,
               "UEFI boot table layout");
_Static_assert(offsetof(struct efi_loaded_image, image_base) == 64, "UEFI loaded image layout");
_Static_assert(sizeof(struct efi_memory_descriptor) == 40, "UEFI memory descriptor layout");
#endif
