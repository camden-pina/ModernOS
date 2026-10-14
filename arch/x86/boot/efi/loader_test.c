/* Host fixtures exercise untrusted file validation with a mock EFI allocator.
 * Unused firmware/privileged entry paths are discarded by --gc-sections. */
#include <assert.h>
#include <stdio.h>
#include "loader.c"

static _Alignas(4096) uint8_t mock_pages[16 * 4096];
static size_t mock_calls, mock_used;

static efi_status mock_allocate(uint32_t mode, uint32_t type, size_t pages, uint64_t *address) {
    assert(mode == EFI_ALLOCATE_MAX && type == EFI_LOADER_DATA);
    assert(*address == UINT32_MAX && pages <= 16 - mock_used);
    *address = (uintptr_t)(mock_pages + mock_used * 4096);
    mock_used += pages;
    mock_calls++;
    return 0;
}

static struct efi_boot_services mock_services = {.allocate_pages = mock_allocate};
static _Alignas(8) uint8_t fixture[4096];
static struct elf_header *header;
static struct elf_program_header *ph;
static struct kernel_image loaded;

static void reset_elf(void) {
    memset(fixture, 0, sizeof(fixture));
    header = (void *)fixture;
    ph = (void *)(fixture + sizeof(*header));
    memcpy(header->ident, "\177ELF", 4);
    header->ident[4] = 2;
    header->ident[5] = 1;
    header->ident[6] = 1;
    header->type = 2;
    header->machine = 62;
    header->version = 1;
    header->entry = UINT64_C(0xffffffff80000001);
    header->phoff = sizeof(*header);
    header->ehsize = sizeof(*header);
    header->phentsize = sizeof(*ph);
    header->phnum = 1;
    ph->type = 1;
    ph->flags = 5;
    ph->offset = 0x100;
    ph->vaddr = UINT64_C(0xffffffff80000000);
    ph->filesz = 4;
    ph->memsz = 512;
    ph->align = 1;
    memcpy(fixture + ph->offset, "MOS!", 4);
    allocation_count = mock_calls = mock_used = 0;
    bs = &mock_services;
}

static void reject(size_t size) {
    assert(load_kernel(fixture, size, &loaded) == EFI_LOAD_ERROR);
    assert(mock_calls == 0); /* Malformed files never allocate partial segments. */
}

static void test_elf(void) {
    reset_elf();
    reject(sizeof(*header) - 1);
    reset_elf();
    header->ident[4] = 1;
    reject(sizeof(fixture));
    reset_elf();
    header->machine = 3;
    reject(sizeof(fixture));
    reset_elf();
    header->type = 3;
    reject(sizeof(fixture));
    reset_elf();
    header->phoff = UINT64_MAX;
    reject(sizeof(fixture));
    reset_elf();
    header->phoff = sizeof(fixture) - 1;
    reject(sizeof(fixture));
    reset_elf();
    header->phentsize--;
    reject(sizeof(fixture));
    reset_elf();
    ph->filesz = ph->memsz + 1;
    reject(sizeof(fixture));
    reset_elf();
    ph->offset = UINT64_MAX;
    reject(sizeof(fixture));
    reset_elf();
    ph->filesz = 10;
    ph->offset = sizeof(fixture) - 1;
    reject(sizeof(fixture));
    reset_elf();
    ph->memsz = 0;
    reject(sizeof(fixture));
    reset_elf();
    ph->vaddr = UINT64_MAX;
    reject(sizeof(fixture));
    reset_elf();
    ph->vaddr = UINT64_C(0xffffffff7ffff000);
    reject(sizeof(fixture));
    reset_elf();
    ph->memsz = UINT64_MAX;
    reject(sizeof(fixture));
    reset_elf();
    ph->align = 3;
    reject(sizeof(fixture));
    reset_elf();
    ph->align = 4096;
    reject(sizeof(fixture));
    reset_elf();
    ph->flags = 6;
    reject(sizeof(fixture));
    reset_elf();
    header->entry += 4096;
    reject(sizeof(fixture));
    reset_elf();
    header->phnum = 2;
    ph[1] = ph[0];
    reject(sizeof(fixture));
    reset_elf();
    header->phnum = 2;
    ph[1].type = 2;
    reject(sizeof(fixture));
    reset_elf();
    header->phnum = 2;
    ph[1].type = 3;
    reject(sizeof(fixture));
    reset_elf();
    header->phnum = ELF_SEGMENT_LIMIT + 1;
    for (size_t i = 0; i < header->phnum; i++) {
        ph[i] = (struct elf_program_header){.type = 1,
                                            .flags = 5,
                                            .vaddr = UINT64_C(0xffffffff80000000) + i * PAGE,
                                            .memsz = PAGE,
                                            .align = 1};
    }
    reject(sizeof(fixture));
    reset_elf();
    assert(load_kernel(fixture, sizeof(fixture), &loaded) == 0);
    assert(mock_calls == 1 && loaded.count == 1 && loaded.segments[0].size == PAGE);
    assert(equal(mock_pages, "MOS!", 4));
    for (size_t i = 4; i < PAGE; i++)
        assert(mock_pages[i] == 0);
    reset_elf();
    ph->vaddr += 13;
    header->entry += 13;
    assert(load_kernel(fixture, sizeof(fixture), &loaded) == 0);
    assert(equal(mock_pages + 13, "MOS!", 4));
    for (size_t i = 0; i < 13; i++)
        assert(mock_pages[i] == 0);
}

static void test_config(void) {
    struct configuration cfg;
    char config[] =
        "kernel=/boot/other.elf\r\nmodule=/boot/initramfs.tar\nmodule_string=initramfs\n"
        "module=/boot/extra.bin\nmodule_string=extra data\nresolution=1920x1080\ncmdline=debug=1 console=serial\n";
    assert(parse_configuration(config, &cfg) == 0);
    assert(string_equal(cfg.kernel, "/boot/other.elf") && cfg.module_count == 2);
    assert(string_equal(cfg.modules[1].string, "extra data") && cfg.width == 1920 && cfg.height == 1080 && cfg.bpp == 32);
    assert(string_equal(cfg.cmdline, "debug=1 console=serial"));
    char empty_resolution[] = "resolution=\n";
    assert(parse_configuration(empty_resolution, &cfg) == 0 && !cfg.width && !cfg.height);
    char bad_resolution[] = "resolution=1920x1080x24\n";
    assert(parse_configuration(bad_resolution, &cfg) == EFI_LOAD_ERROR);
    char orphan_string[] = "module_string=no module\n";
    assert(parse_configuration(orphan_string, &cfg) == EFI_LOAD_ERROR);
    char bad_key[] = "unknown=1\n";
    assert(parse_configuration(bad_key, &cfg) == EFI_LOAD_ERROR);
}

static void test_memory_map(void) {
    static struct handoff handoff;
    struct efi_memory_descriptor entries[] = {
        {.type = EFI_LOADER_DATA, .physical = 0x800000, .pages = 8},
        {.type = EFI_CONVENTIONAL, .physical = 0x100000, .pages = 16},
        {.type = EFI_CONVENTIONAL, .physical = 0x80000, .pages = 128},
        {.type = EFI_BOOT_DATA, .physical = 0x110000, .pages = 16},
    };
    struct firmware_map map = {
        .buffer = entries, .size = sizeof(entries), .stride = sizeof(entries[0])};
    assert(convert_memory_map(&map, &handoff, 0x108000, 0x10000));
    assert(handoff.info.memory_map_count == 5);
    assert(handoff.ranges[0].base == 0x80000 && handoff.ranges[0].type == BOOT_MEMORY_RESERVED);
    assert(handoff.ranges[1].base == 0x100000 && handoff.ranges[1].type == BOOT_MEMORY_USABLE);
    assert(handoff.ranges[2].base == 0x108000 && handoff.ranges[2].length == 0x10000 && handoff.ranges[2].type == BOOT_MEMORY_FRAMEBUFFER);
    assert(handoff.ranges[3].base == 0x118000 && handoff.ranges[3].type == BOOT_MEMORY_USABLE);
    assert(handoff.ranges[4].base == 0x800000 && handoff.ranges[4].type == BOOT_MEMORY_RESERVED);
    entries[3].physical = 0x100000;
    assert(!convert_memory_map(&map, &handoff, 0x200000, 0x1000));
}

int main(void) {
    test_elf();
    test_config();
    test_memory_map();
    puts(
        "EFI loader fixtures PASS: malformed ELF, BSS/unaligned loads, config, sorted reserved maps");
    return 0;
}
