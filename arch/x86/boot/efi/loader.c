/* Native UEFI OS loader. Firmware protocols end here; the kernel receives the
 * same private boot ABI used by the Limine adapter. */
#include "efi.h"
#include <boot/info.h>

#define PAGE 4096ULL
#define LARGE_PAGE 0x200000ULL
#define PHYSICAL_LIMIT (UINT64_C(1) << 47)
#define CONFIG_LIMIT (64 * 1024)
#define FILE_LIMIT (UINT64_C(1) << 30)
#define PATH_LIMIT 512
#define ELF_SEGMENT_LIMIT 32
#define STACK_PAGES 32
#define MAP_SLACK 128

static const efi_guid loaded_image_guid = {
    0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const efi_guid filesystem_guid = {
    0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const efi_guid file_info_guid = {
    0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const efi_guid gop_guid = {
    0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
static const efi_guid acpi2_guid = {
    0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
static const efi_guid acpi1_guid = {
    0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
static struct efi_system_table *system_table;
static struct efi_boot_services *bs;
static struct efi_file *root;
struct allocation_record {
    uint64_t address;
    size_t pages;
};

static struct allocation_record allocations[128];
static size_t allocation_count;

void *memset(void *p, int c, size_t n) {
    unsigned char *d = p;
    while (n--)
        *d++ = (unsigned char)c;
    return p;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *out = d;
    const unsigned char *in = s;
    while (n--)
        *out++ = *in++;
    return d;
}

static int equal(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    while (n--)
        if (*x++ != *y++)
            return 0;
    return 1;
}

static size_t string_length(const char *s) {
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static int string_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint64_t round_up(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

static void *virtual_pointer(const void *p) {
    return (void *)(BOOT_HHDM_BASE + (uintptr_t)p);
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init(void) {
    outb(0x3f9, 0);
    outb(0x3fb, 0x80);
    outb(0x3f8, 1);
    outb(0x3f9, 0);
    outb(0x3fb, 3);
    outb(0x3fa, 0xc7);
    outb(0x3fc, 3);
}

static void serial_write(const char *s) {
    while (*s) {
        for (unsigned wait = 0; wait < 100000; wait++)
            if (inb(0x3fd) & 0x20)
                break;
        outb(0x3f8, (uint8_t)*s++);
    }
}

static void message(const char *text) {
    char16 buffer[160];
    size_t i = 0;
    serial_write("[efi] ");
    serial_write(text);
    serial_write("\r\n");
    while (text[i] && i < 157) {
        buffer[i] = (unsigned char)text[i];
        i++;
    }
    buffer[i++] = '\r';
    buffer[i++] = '\n';
    buffer[i] = 0;
    if (system_table->console_out)
        system_table->console_out->output_string(system_table->console_out, buffer);
}

static efi_status allocation(size_t pages, uint64_t *address) {
    /* Keep loader objects below 4 GiB, also usable by the AP startup tables. */
    if (!pages || allocation_count == sizeof(allocations) / sizeof(allocations[0]))
        return EFI_OUT_OF_RESOURCES;
    *address = UINT32_MAX;
    efi_status status = bs->allocate_pages(EFI_ALLOCATE_MAX, EFI_LOADER_DATA, pages, address);
    if (!EFI_ERROR(status)) {
        allocations[allocation_count++] = (struct allocation_record){*address, pages};
        memset((void *)(uintptr_t)*address, 0, pages * PAGE);
    }
    return status;
}

static efi_status open_volume(efi_handle device) {
    struct efi_simple_filesystem *fs;
    efi_status status = bs->handle_protocol(device, &filesystem_guid, (void **)&fs);
    if (EFI_ERROR(status))
        return status;
    struct efi_file *candidate;
    status = fs->open_volume(fs, &candidate);
    if (EFI_ERROR(status))
        return status;
    /* GRUB can chainload a PE image from ISO9660 while its device handle has
     * no firmware file protocol. The ISO also contains a FAT EFI boot image
     * with all payloads, so discover that volume by its handoff config. */
    static const char16 path[] = {'\\', 'b', 'o', 'o', 't', '\\', 'm', 'o', 'd', 'e',
                                  'r',  'n', 'o', 's', '.', 'c',  'o', 'n', 'f', 0};
    struct efi_file *probe;
    status = candidate->open(candidate, &probe, path, 1, 0);
    if (EFI_ERROR(status)) {
        candidate->close(candidate);
        return status;
    }
    probe->close(probe);
    root = candidate;
    return 0;
}

static efi_status open_root(efi_handle image) {
    struct efi_loaded_image *loaded;
    efi_status status = bs->handle_protocol(image, &loaded_image_guid, (void **)&loaded);
    if (!EFI_ERROR(status)) {
        status = open_volume(loaded->device);
        if (!EFI_ERROR(status))
            return 0;
    }
    size_t count = 0;
    efi_handle *handles;
    status = bs->locate_handle_buffer(2 /* ByProtocol */, &filesystem_guid, NULL, &count, &handles);
    if (EFI_ERROR(status))
        return status;
    status = EFI_NOT_FOUND;
    for (size_t i = 0; i < count; i++) {
        status = open_volume(handles[i]);
        if (!EFI_ERROR(status))
            break;
    }
    bs->free_pool(handles);
    return status;
}

static efi_status read_file(const char *path, uint64_t limit, void **contents, size_t *size) {
    char16 filename[PATH_LIMIT];
    struct efi_file *file;
    size_t path_length = string_length(path);
    if (!path_length || path_length >= PATH_LIMIT || path[0] != '/')
        return EFI_INVALID_PARAMETER;
    for (size_t i = 0; i <= path_length; i++) {
        if ((unsigned char)path[i] > 127)
            return EFI_INVALID_PARAMETER;
        filename[i] = path[i] == '/' ? '\\' : (unsigned char)path[i];
    }
    efi_status status = root->open(root, &file, filename, 1, 0);
    if (EFI_ERROR(status))
        return status;
    uint8_t info_buffer[sizeof(struct efi_file_info) + PATH_LIMIT * sizeof(char16)];
    size_t info_size = sizeof(info_buffer);
    status = file->get_info(file, &file_info_guid, &info_size, info_buffer);
    if (EFI_ERROR(status)) {
        file->close(file);
        return status;
    }
    struct efi_file_info *info = (void *)info_buffer;
    if (info_size < offsetof(struct efi_file_info, name) || info->file_size > limit ||
        !info->file_size || (info->attribute & 0x10)) {
        file->close(file);
        return EFI_LOAD_ERROR;
    }
    *size = (size_t)info->file_size;
    uint64_t physical;
    status = allocation((size_t)round_up(*size + 1, PAGE) / PAGE, &physical);
    if (EFI_ERROR(status)) {
        file->close(file);
        return status;
    }
    *contents = (void *)(uintptr_t)physical;
    size_t remaining = *size, offset = 0;
    while (remaining) {
        size_t bytes = remaining;
        status = file->read(file, &bytes, (uint8_t *)*contents + offset);
        if (EFI_ERROR(status) || !bytes || bytes > remaining) {
            file->close(file);
            return EFI_ERROR(status) ? status : EFI_LOAD_ERROR;
        }
        offset += bytes;
        remaining -= bytes;
    }
    file->close(file);
    return 0;
}

struct configuration {
    char *kernel, *cmdline;
    struct {
        char *path, *string;
    } modules[BOOT_MAX_MODULES];
    size_t module_count;
    uint32_t width, height, bpp;
};

static int decimal(char **s, uint32_t *number) {
    uint32_t value = 0;
    char *p = *s;
    if (*p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9') {
        if (value > (UINT32_MAX - (*p - '0')) / 10)
            return 0;
        value = value * 10 + (*p++ - '0');
    }
    *s = p;
    *number = value;
    return 1;
}

static efi_status parse_configuration(char *buffer, struct configuration *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->kernel = "/boot/kernel.elf";
    cfg->cmdline = "";
    cfg->width = 2560;
    cfg->height = 1600;
    cfg->bpp = 32;
    for (char *line = buffer; *line;) {
        char *next = line;
        while (*next && *next != '\n')
            next++;
        if (*next)
            *next++ = 0;
        while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
        size_t length = string_length(line);
        while (length &&
               (line[length - 1] == '\r' || line[length - 1] == ' ' || line[length - 1] == '\t'))
            line[--length] = 0;
        if (!*line || *line == '#') {
            line = next;
            continue;
        }
        char *value = line;
        while (*value && *value != '=')
            value++;
        if (!*value)
            return EFI_LOAD_ERROR;
        *value++ = 0;
        if (string_equal(line, "kernel"))
            cfg->kernel = value;
        else if (string_equal(line, "cmdline"))
            cfg->cmdline = value;
        else if (string_equal(line, "module")) {
            if (cfg->module_count == BOOT_MAX_MODULES || !*value)
                return EFI_LOAD_ERROR;
            cfg->modules[cfg->module_count].path = value;
            cfg->modules[cfg->module_count++].string = "";
        } else if (string_equal(line, "module_string")) {
            if (!cfg->module_count)
                return EFI_LOAD_ERROR;
            cfg->modules[cfg->module_count - 1].string = value;
        } else if (string_equal(line, "resolution")) {
            char *p = value;
            if (!*p) {
                cfg->width = cfg->height = 0;
            } else {
                cfg->bpp = 32;
                if (!decimal(&p, &cfg->width) || *p++ != 'x' || !decimal(&p, &cfg->height) ||
                    (*p && (*p++ != 'x' || !decimal(&p, &cfg->bpp))) || *p || !cfg->width ||
                    !cfg->height || cfg->bpp != 32)
                    return EFI_LOAD_ERROR;
            }
        } else
            return EFI_LOAD_ERROR;
        line = next;
    }
    return 0;
}

struct elf_header {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf_program_header {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct kernel_segment {
    uint64_t virtual_base, physical_base, size, flags;
};

struct kernel_image {
    uint64_t entry;
    size_t count;
    struct kernel_segment segments[ELF_SEGMENT_LIMIT];
};

static efi_status load_kernel(const void *file, size_t size, struct kernel_image *kernel) {
    const struct elf_header *header = file;
    if (size < sizeof(*header) || !equal(header->ident, "\177ELF", 4) || header->ident[4] != 2 ||
        header->ident[5] != 1 || header->ident[6] != 1 || header->type != 2 ||
        header->machine != 62 || header->version != 1 || header->ehsize != sizeof(*header) ||
        header->phentsize != sizeof(struct elf_program_header) || !header->phnum ||
        header->phnum > 128 || header->phoff > size ||
        header->phnum > (size - header->phoff) / sizeof(struct elf_program_header))
        return EFI_LOAD_ERROR;
    const struct elf_program_header *ph = (const void *)((const uint8_t *)file + header->phoff);
    kernel->count = 0;
    kernel->entry = header->entry;
    uint64_t total = 0;
    int entry_found = 0;
    for (unsigned i = 0; i < header->phnum; i++) {
        /* A dynamically linked executable needs relocations this loader does
         * not implement. Validate all headers before allocating any segment. */
        if (ph[i].type == 2 || ph[i].type == 3)
            return EFI_LOAD_ERROR;
        if (ph[i].type != 1)
            continue;
        if (!ph[i].memsz) {
            if (ph[i].filesz)
                return EFI_LOAD_ERROR;
            continue;
        }
        if (kernel->count == ELF_SEGMENT_LIMIT || ph[i].filesz > ph[i].memsz ||
            ph[i].offset > size || ph[i].filesz > size - ph[i].offset ||
            ph[i].vaddr < UINT64_C(0xffffffff80000000) ||
            ph[i].vaddr >= UINT64_C(0xffffffffffff0000) ||
            ph[i].memsz > UINT64_C(0xffffffffffff0000) - ph[i].vaddr ||
            (ph[i].align > 1 && ((ph[i].align & (ph[i].align - 1)) ||
                                 (ph[i].vaddr % ph[i].align != ph[i].offset % ph[i].align))))
            return EFI_LOAD_ERROR;
        uint64_t start = ph[i].vaddr & ~(PAGE - 1);
        uint64_t bytes = round_up((ph[i].vaddr - start) + ph[i].memsz, PAGE);
        if (bytes > FILE_LIMIT || total > FILE_LIMIT - bytes)
            return EFI_LOAD_ERROR;
        total += bytes;
        for (size_t j = 0; j < kernel->count; j++)
            if (start < kernel->segments[j].virtual_base + kernel->segments[j].size &&
                kernel->segments[j].virtual_base < start + bytes)
                return EFI_LOAD_ERROR;
        struct kernel_segment *segment = &kernel->segments[kernel->count++];
        segment->virtual_base = start;
        segment->size = bytes;
        segment->flags = ph[i].flags;
        if ((ph[i].flags & 1) && header->entry >= ph[i].vaddr &&
            header->entry - ph[i].vaddr < ph[i].memsz)
            entry_found = 1;
    }
    if (!kernel->count || !entry_found)
        return EFI_LOAD_ERROR;
    size_t loaded = 0;
    for (unsigned i = 0; i < header->phnum; i++) {
        if (ph[i].type != 1 || !ph[i].memsz)
            continue;
        struct kernel_segment *segment = &kernel->segments[loaded++];
        efi_status status = allocation((size_t)(segment->size / PAGE), &segment->physical_base);
        if (EFI_ERROR(status))
            return status;
        memcpy((void *)(uintptr_t)(segment->physical_base + ph[i].vaddr - segment->virtual_base),
               (const uint8_t *)file + ph[i].offset, (size_t)ph[i].filesz);
    }
    return 0;
}

static int pixel_mask(uint32_t mask, uint8_t *shift, uint8_t *size) {
    if (!mask)
        return 0;
    *shift = 0;
    *size = 0;
    while (!(mask & 1)) {
        (*shift)++;
        mask >>= 1;
    }
    while (mask & 1) {
        (*size)++;
        mask >>= 1;
    }
    return !mask && *size == 8;
}

static int framebuffer_format(struct efi_gop_info *mode, struct boot_framebuffer *fb) {
    uint32_t red, green, blue;
    if (mode->format == 0) {
        red = 0xff;
        green = 0xff00;
        blue = 0xff0000;
    } else if (mode->format == 1) {
        red = 0xff0000;
        green = 0xff00;
        blue = 0xff;
    } else if (mode->format == 2) {
        red = mode->masks.red;
        green = mode->masks.green;
        blue = mode->masks.blue;
    } else
        return 0;
    if ((red & green) || (red & blue) || (green & blue) ||
        !pixel_mask(red, &fb->red_mask_shift, &fb->red_mask_size) ||
        !pixel_mask(green, &fb->green_mask_shift, &fb->green_mask_size) ||
        !pixel_mask(blue, &fb->blue_mask_shift, &fb->blue_mask_size) || !mode->width ||
        !mode->height || mode->pixels_per_line < mode->width)
        return 0;
    fb->width = mode->width;
    fb->height = mode->height;
    fb->pitch = (uint64_t)mode->pixels_per_line * 4;
    fb->bpp = 32;
    fb->memory_model = 1;
    return 1;
}

static efi_status setup_framebuffer(const struct configuration *cfg, struct boot_framebuffer *fb,
                                    uint64_t *physical, uint64_t *bytes) {
    struct efi_gop *gop;
    efi_status status = bs->locate_protocol(&gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(status) || !gop->mode)
        return EFI_UNSUPPORTED;
    uint32_t choice = UINT32_MAX;
    uint64_t best_score = UINT64_MAX;
    for (uint32_t i = 0; i < gop->mode->max_mode; i++) {
        struct efi_gop_info *info;
        struct boot_framebuffer candidate;
        size_t info_size = 0;
        status = gop->query_mode(gop, i, &info_size, &info);
        if (EFI_ERROR(status))
            continue;
        if (info_size >= sizeof(*info) && framebuffer_format(info, &candidate)) {
            uint64_t dx =
                info->width > cfg->width ? info->width - cfg->width : cfg->width - info->width;
            uint64_t dy = info->height > cfg->height ? info->height - cfg->height
                                                     : cfg->height - info->height;
            uint64_t score = cfg->width && cfg->height ? dx + dy : (i == gop->mode->mode ? 0 : 1);
            if (score < best_score) {
                best_score = score;
                choice = i;
            }
        }
        bs->free_pool(info);
    }
    if (choice != UINT32_MAX && choice != gop->mode->mode) {
        status = gop->set_mode(gop, choice);
        /* A firmware may refuse a queried mode; retain its valid current mode. */
        if (EFI_ERROR(status))
            message("Requested display mode unavailable; using current mode");
    }
    if (!gop->mode->info || !framebuffer_format(gop->mode->info, fb) || !gop->mode->framebuffer ||
        !gop->mode->framebuffer_size || fb->height > gop->mode->framebuffer_size / fb->pitch)
        return EFI_UNSUPPORTED;
    *physical = gop->mode->framebuffer;
    *bytes = gop->mode->framebuffer_size;
    if (*physical >= PHYSICAL_LIMIT || *bytes > PHYSICAL_LIMIT - *physical)
        return EFI_UNSUPPORTED;
    fb->address = virtual_pointer((void *)(uintptr_t)*physical);
    return 0;
}

static void *find_rsdp(void) {
    void *fallback = NULL;
    for (size_t i = 0; i < system_table->table_count; i++) {
        if (equal(&system_table->tables[i].guid, &acpi2_guid, sizeof(efi_guid)))
            return virtual_pointer(system_table->tables[i].table);
        if (equal(&system_table->tables[i].guid, &acpi1_guid, sizeof(efi_guid)))
            fallback = virtual_pointer(system_table->tables[i].table);
    }
    return fallback;
}

static int64_t firmware_epoch(void) {
    struct efi_time time;
    if (!system_table->runtime || EFI_ERROR(system_table->runtime->get_time(&time, NULL)) ||
        time.year < 1970 || time.year > 9999 || time.month < 1 || time.month > 12 || time.day < 1 ||
        time.day > 31 || time.hour > 23 || time.minute > 59 || time.second > 59)
        return 0;
    static const unsigned cumulative[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int64_t year = time.year - 1;
    int64_t days = (time.year - 1970) * 365 + year / 4 - year / 100 + year / 400 - 477;
    days += cumulative[time.month - 1] + time.day - 1;
    if (time.month > 2 && (!(time.year % 4) && (time.year % 100 || !(time.year % 400))))
        days++;
    int64_t epoch = ((days * 24 + time.hour) * 60 + time.minute) * 60 + time.second;
    if (time.timezone >= -1440 && time.timezone <= 1440)
        epoch -= time.timezone * 60;
    return epoch;
}

struct firmware_map {
    void *buffer;
    size_t capacity, size, key, stride;
    uint32_t version;
};

static efi_status get_map(struct firmware_map *map) {
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        map->size = map->capacity;
        efi_status status =
            bs->get_memory_map(&map->size, map->buffer, &map->key, &map->stride, &map->version);
        if (status != EFI_BUFFER_TOO_SMALL) {
            if (!EFI_ERROR(status) &&
                (map->stride < sizeof(struct efi_memory_descriptor) || map->size % map->stride ||
                 map->size / map->stride > BOOT_MAX_MEMMAP - 16))
                return EFI_LOAD_ERROR;
            return status;
        }
        size_t stride = map->stride >= sizeof(struct efi_memory_descriptor)
                            ? map->stride
                            : sizeof(struct efi_memory_descriptor);
        if (map->size > (BOOT_MAX_MEMMAP - MAP_SLACK) * stride)
            return EFI_OUT_OF_RESOURCES;
        size_t capacity = (size_t)round_up(map->size + MAP_SLACK * stride, PAGE);
        uint64_t physical;
        status = allocation(capacity / PAGE, &physical);
        if (EFI_ERROR(status))
            return status;
        /* Older map buffers stay reserved loader data, so nothing can invalidate
         * pointers retained in the handoff or page tables. */
        map->buffer = (void *)(uintptr_t)physical;
        map->capacity = capacity;
    }
    return EFI_OUT_OF_RESOURCES;
}

static struct efi_memory_descriptor *descriptor(const struct firmware_map *map, size_t i) {
    return (void *)((uint8_t *)map->buffer + i * map->stride);
}

struct page_tables {
    uint64_t *arena, *pml4;
    size_t pages, used;
};

static uint64_t *new_table(struct page_tables *tables) {
    if (tables->used == tables->pages)
        return NULL;
    return tables->arena + 512 * tables->used++;
}

static uint64_t *next_table(struct page_tables *tables, uint64_t *parent, unsigned index) {
    if (!(parent[index] & 1)) {
        uint64_t *table = new_table(tables);
        if (!table)
            return NULL;
        parent[index] = (uint64_t)(uintptr_t)table | 3;
    }
    if (parent[index] & 0x80)
        return NULL;
    return (void *)(uintptr_t)(parent[index] & UINT64_C(0x000ffffffffff000));
}

static int map_page(struct page_tables *tables, uint64_t virt, uint64_t phys, int large,
                    uint64_t flags) {
    uint64_t *pdpt = next_table(tables, tables->pml4, (virt >> 39) & 511);
    if (!pdpt)
        return 0;
    uint64_t *pd = next_table(tables, pdpt, (virt >> 30) & 511);
    if (!pd)
        return 0;
    unsigned index = (virt >> 21) & 511;
    if (large) {
        uint64_t entry = phys | flags | 0x81;
        if (pd[index] && pd[index] != entry)
            return 0;
        pd[index] = entry;
    } else {
        uint64_t *pt = next_table(tables, pd, index);
        if (!pt)
            return 0;
        index = (virt >> 12) & 511;
        uint64_t entry = phys | flags | 1;
        if (pt[index] && pt[index] != entry)
            return 0;
        pt[index] = entry;
    }
    return 1;
}

static int map_physical(struct page_tables *tables, uint64_t base, uint64_t size) {
    if (!size)
        return 1;
    if (base >= PHYSICAL_LIMIT || size > PHYSICAL_LIMIT - base)
        return 0;
    uint64_t end = round_up(base + size, LARGE_PAGE);
    for (uint64_t address = base & ~(LARGE_PAGE - 1); address < end; address += LARGE_PAGE)
        if (!map_page(tables, address, address, 1, 2) ||
            !map_page(tables, BOOT_HHDM_BASE + address, address, 1, 2))
            return 0;
    return 1;
}

static efi_status build_page_tables(const struct firmware_map *map,
                                    const struct kernel_image *kernel, uint64_t fb_base,
                                    uint64_t fb_size, struct page_tables *tables) {
    /* Two aliases need a PD per physical GiB and a PDPT per 512 GiB.
     * Overestimate per descriptor, including room for the kernel's 4 KiB PTs
     * and allocations made after this snapshot. */
    size_t pages = 64;
    for (size_t i = 0; i < map->size / map->stride; i++) {
        struct efi_memory_descriptor *d = descriptor(map, i);
        if (!d->pages || d->physical >= PHYSICAL_LIMIT ||
            d->pages > (PHYSICAL_LIMIT - d->physical) / PAGE)
            continue;
        uint64_t bytes = d->pages * PAGE;
        uint64_t count = 2 * (bytes / (UINT64_C(1) << 30) + bytes / (UINT64_C(1) << 39) + 4);
        if (count > 262144 || pages > 262144 - count)
            return EFI_OUT_OF_RESOURCES;
        pages += (size_t)count;
    }
    for (size_t i = 0; i < kernel->count; i++)
        pages += (size_t)(kernel->segments[i].size / LARGE_PAGE + 2);
    pages += (size_t)(fb_size / (UINT64_C(1) << 30) + 8);
    uint64_t physical;
    efi_status status = allocation(pages, &physical);
    if (EFI_ERROR(status))
        return status;
    tables->arena = (void *)(uintptr_t)physical;
    tables->pages = pages;
    tables->used = 0;
    tables->pml4 = new_table(tables);
    if (!map_physical(tables, 0, LARGE_PAGE) || !map_physical(tables, fb_base, fb_size))
        return EFI_LOAD_ERROR;
    for (size_t i = 0; i < map->size / map->stride; i++) {
        struct efi_memory_descriptor *d = descriptor(map, i);
        if (d->physical >= PHYSICAL_LIMIT)
            continue;
        if (d->pages > (PHYSICAL_LIMIT - d->physical) / PAGE ||
            !map_physical(tables, d->physical, d->pages * PAGE))
            return EFI_LOAD_ERROR;
    }
    /* The arena allocation lies in memory already mapped as conventional RAM. */
    for (size_t i = 0; i < kernel->count; i++) {
        const struct kernel_segment *segment = &kernel->segments[i];
        for (uint64_t offset = 0; offset < segment->size; offset += PAGE)
            if (!map_page(tables, segment->virtual_base + offset, segment->physical_base + offset,
                          0, (segment->flags & 2) ? 2 : 0))
                return EFI_LOAD_ERROR;
    }
    return 0;
}

struct handoff {
    struct boot_info info;
    struct boot_framebuffer framebuffer;
    struct boot_module modules[BOOT_MAX_MODULES];
    struct boot_memory_range ranges[BOOT_MAX_MEMMAP];
    char loader_name[32];
};

static uint64_t memory_type(uint32_t type) {
    switch (type) {
    case EFI_CONVENTIONAL:
    case EFI_BOOT_CODE:
    case EFI_BOOT_DATA:
        return BOOT_MEMORY_USABLE;
    case EFI_ACPI_RECLAIM:
        return BOOT_MEMORY_ACPI_RECLAIMABLE;
    case EFI_ACPI_NVS:
        return BOOT_MEMORY_ACPI_NVS;
    case EFI_UNUSABLE:
    case EFI_UNACCEPTED:
        return BOOT_MEMORY_BAD;
    default:
        return BOOT_MEMORY_RESERVED;
    }
}

static int append_range(struct handoff *handoff, uint64_t base, uint64_t length, uint64_t type) {
    if (!length)
        return 1;
    size_t count = (size_t)handoff->info.memory_map_count;
    if (count == BOOT_MAX_MEMMAP)
        return 0;
    size_t position = count;
    while (position && handoff->ranges[position - 1].base > base) {
        handoff->ranges[position] = handoff->ranges[position - 1];
        position--;
    }
    handoff->ranges[position] = (struct boot_memory_range){base, length, type};
    handoff->info.memory_map_count++;
    return 1;
}

static int convert_memory_map(const struct firmware_map *map, struct handoff *handoff,
                              uint64_t fb_base, uint64_t fb_size) {
    handoff->info.memory_map_count = 0;
    for (size_t i = 0; i < map->size / map->stride; i++) {
        struct efi_memory_descriptor *d = descriptor(map, i);
        if (d->pages > (UINT64_MAX - d->physical) / PAGE)
            return 0;
        uint64_t base = d->physical, bytes = d->pages * PAGE;
        uint64_t type = memory_type(d->type);
        if (base >= PHYSICAL_LIMIT || bytes > PHYSICAL_LIMIT - base)
            type = BOOT_MEMORY_RESERVED;
        /* Real mode startup and firmware data below 1 MiB must survive PMM. */
        if (base < 0x100000) {
            uint64_t low = bytes < 0x100000 - base ? bytes : 0x100000 - base;
            if (!append_range(handoff, base, low, BOOT_MEMORY_RESERVED))
                return 0;
            base += low;
            bytes -= low;
        }
        /* GOP memory is sometimes described as conventional RAM. Split it
         * explicitly so the allocator never overwrites the visible display. */
        uint64_t end = base + bytes, fb_end = fb_base + fb_size;
        if (bytes && base < fb_end && fb_base < end) {
            uint64_t begin = base > fb_base ? base : fb_base;
            uint64_t finish = end < fb_end ? end : fb_end;
            if (!append_range(handoff, base, begin - base, type) ||
                !append_range(handoff, begin, finish - begin, BOOT_MEMORY_FRAMEBUFFER) ||
                !append_range(handoff, finish, end - finish, type))
                return 0;
        } else if (!append_range(handoff, base, bytes, type))
            return 0;
    }
    size_t out = 0, count = (size_t)handoff->info.memory_map_count;
    for (size_t i = 0; i < count; i++) {
        struct boot_memory_range range = handoff->ranges[i];
        if (out && handoff->ranges[out - 1].base + handoff->ranges[out - 1].length > range.base)
            return 0;
        if (out && handoff->ranges[out - 1].base + handoff->ranges[out - 1].length == range.base &&
            handoff->ranges[out - 1].type == range.type)
            handoff->ranges[out - 1].length += range.length;
        else
            handoff->ranges[out++] = range;
    }
    handoff->info.memory_map_count = out;
    return 1;
}

extern void efi_enter_kernel(uint64_t cr3, uint64_t stack, uint64_t entry, const struct boot_info *)
    __attribute__((noreturn));

static efi_status loader_main(efi_handle image, struct efi_system_table *table) {
    system_table = table;
    bs = table->boot;
    serial_init();
    message("ModernOS native UEFI loader");
    bs->set_watchdog_timer(0, 0, 0, NULL);

    uint64_t cr4;
    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1 << 12)) {
        message("Five-level firmware paging is unsupported");
        return EFI_UNSUPPORTED;
    }

    efi_status status = open_root(image);
    if (EFI_ERROR(status)) {
        message("Cannot open the boot filesystem");
        return status;
    }

    void *config_buffer;
    size_t config_size;
    status = read_file("/boot/modernos.conf", CONFIG_LIMIT, &config_buffer, &config_size);
    if (EFI_ERROR(status)) {
        message("Cannot read /boot/modernos.conf");
        return status;
    }
    for (size_t i = 0; i < config_size; i++)
        if (!((char *)config_buffer)[i] || (unsigned char)((char *)config_buffer)[i] > 127) {
            message("Invalid boot configuration encoding");
            return EFI_LOAD_ERROR;
        }

    struct configuration config;
    status = parse_configuration(config_buffer, &config);
    if (EFI_ERROR(status)) {
        message("Invalid boot configuration");
        return status;
    }

    uint64_t physical;
    status = allocation((size_t)round_up(sizeof(struct handoff), PAGE) / PAGE, &physical);
    if (EFI_ERROR(status))
        return status;

    struct handoff *handoff = (void *)(uintptr_t)physical;
    handoff->info.magic = BOOT_INFO_MAGIC;
    handoff->info.version = BOOT_INFO_VERSION;
    handoff->info.hhdm_offset = BOOT_HHDM_BASE;
    handoff->info.memory_map = virtual_pointer(handoff->ranges);
    handoff->info.modules = virtual_pointer(handoff->modules);
    handoff->info.framebuffer = virtual_pointer(&handoff->framebuffer);
    memcpy(handoff->loader_name, "ModernOS UEFI", sizeof("ModernOS UEFI"));
    handoff->info.loader_name = virtual_pointer(handoff->loader_name);
    handoff->info.cmdline = virtual_pointer(config.cmdline);
    handoff->info.rsdp = find_rsdp();
    handoff->info.epoch = firmware_epoch();
    if (!handoff->info.rsdp) {
        message("ACPI root table unavailable");
        return EFI_UNSUPPORTED;
    }

    uint64_t fb_base, fb_size;
    status = setup_framebuffer(&config, &handoff->framebuffer, &fb_base, &fb_size);
    if (EFI_ERROR(status)) {
        message("No supported 32-bit RGB framebuffer");
        return status;
    }

    void *kernel_buffer;
    size_t kernel_size;
    status = read_file(config.kernel, FILE_LIMIT, &kernel_buffer, &kernel_size);
    if (EFI_ERROR(status)) {
        message("Cannot read the kernel ELF");
        return status;
    }

    struct kernel_image kernel;
    status = load_kernel(kernel_buffer, kernel_size, &kernel);
    if (EFI_ERROR(status)) {
        message("Invalid kernel ELF or insufficient memory");
        return status;
    }

    for (size_t i = 0; i < config.module_count; i++) {
        void *buffer;
        size_t bytes;
        status = read_file(config.modules[i].path, FILE_LIMIT, &buffer, &bytes);
        if (EFI_ERROR(status)) {
            message("Cannot read a boot module");
            return status;
        }
        handoff->modules[i].address = virtual_pointer(buffer);
        handoff->modules[i].size = bytes;
        handoff->modules[i].path = virtual_pointer(config.modules[i].path);
        handoff->modules[i].string = virtual_pointer(config.modules[i].string);
        handoff->info.module_count++;
    }

    root->close(root);
    root = NULL;

    uint64_t stack;
    status = allocation(STACK_PAGES, &stack);
    if (EFI_ERROR(status))
        return status;

    uint64_t trampoline = 0x8000;
    status = bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA, 5, &trampoline);
    if (EFI_ERROR(status)) {
        trampoline = 0xfffff;
        status = bs->allocate_pages(EFI_ALLOCATE_MAX, EFI_LOADER_DATA, 5, &trampoline);
    }
    if (!EFI_ERROR(status)) {
        allocations[allocation_count++] = (struct allocation_record){trampoline, 5};
        memset((void *)(uintptr_t)trampoline, 0, 5 * PAGE);
        handoff->info.ap_trampoline = trampoline;
    }

    struct firmware_map map = {0};
    status = get_map(&map);
    if (EFI_ERROR(status)) {
        message("Cannot obtain firmware memory map");
        return status;
    }

    struct page_tables tables;
    status = build_page_tables(&map, &kernel, fb_base, fb_size, &tables);
    if (EFI_ERROR(status)) {
        message("Cannot construct kernel page tables");
        return status;
    }

    message("Exiting firmware and entering the kernel");
    /* No console, file, or allocation calls between a final map and exit. The
     * only permitted recovery after a failed exit is a fresh map and retry. */
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        status = get_map(&map);
        if (EFI_ERROR(status))
            return status;
        if (!convert_memory_map(&map, handoff, fb_base, fb_size))
            return EFI_LOAD_ERROR;
        status = bs->exit_boot_services(image, map.key);
        if (!EFI_ERROR(status))
            efi_enter_kernel((uint64_t)(uintptr_t)tables.pml4,
                             BOOT_HHDM_BASE + stack + STACK_PAGES * PAGE, kernel.entry,
                             virtual_pointer(&handoff->info));
        if (status != EFI_INVALID_PARAMETER)
            return status;
    }

    return status;
}

efi_status efi_main(efi_handle image, struct efi_system_table *table) {
    root = NULL;
    allocation_count = 0;
    efi_status status = loader_main(image, table);
    /* A failed menu entry can be selected again. Release owned pages before
     * returning to its boot manager, rather than accumulating failed loads. */
    if (root) {
        root->close(root);
        root = NULL;
    }
    while (allocation_count) {
        struct allocation_record *record = &allocations[--allocation_count];
        bs->free_pages(record->address, record->pages);
    }
    return status;
}
