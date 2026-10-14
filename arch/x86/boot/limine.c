/* Limine protocol details stop at this adapter. */
#include <boot/info.h>
#include <boot/smp.h>
#include <limine.h>
#include <panic.h>

__attribute__((used, section(".limine_requests"))) static volatile uint64_t base_revision[] =
    LIMINE_BASE_REVISION(6);
__attribute__((used, section(".limine_requests_start"))) static volatile uint64_t start_marker[] =
    LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t end_marker[] =
    LIMINE_REQUESTS_END_MARKER;
#define REQUEST(name, type, magic) \
    __attribute__((used,section(".limine_requests"))) \
    static volatile struct type name={.id=magic,.revision=0}

REQUEST(framebuffer_request, limine_framebuffer_request, LIMINE_FRAMEBUFFER_REQUEST_ID);
REQUEST(memmap_request, limine_memmap_request, LIMINE_MEMMAP_REQUEST_ID);
REQUEST(rsdp_request, limine_rsdp_request, LIMINE_RSDP_REQUEST_ID);
REQUEST(mp_request, limine_mp_request, LIMINE_MP_REQUEST_ID);
REQUEST(module_request, limine_module_request, LIMINE_MODULE_REQUEST_ID);
REQUEST(date_request, limine_date_at_boot_request, LIMINE_DATE_AT_BOOT_REQUEST_ID);
REQUEST(hhdm_request, limine_hhdm_request, LIMINE_HHDM_REQUEST_ID);
REQUEST(loader_request, limine_bootloader_info_request, LIMINE_BOOTLOADER_INFO_REQUEST_ID);
REQUEST(cmdline_request, limine_executable_cmdline_request, LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID);
__attribute__((used, section(".limine_requests"))) static volatile struct limine_stack_size_request
    stack_request = {.id = LIMINE_STACK_SIZE_REQUEST_ID, .revision = 0, .stack_size = 64 * 1024};

static struct boot_memory_range memory_map[BOOT_MAX_MEMMAP];
static struct boot_module modules[BOOT_MAX_MODULES];
static struct boot_framebuffer boot_fb;
#define LIMINE_MAX_CPUS 256

static struct boot_cpu cpus[LIMINE_MAX_CPUS];
static struct {
    boot_ap_entry entry;
    uintptr_t argument, stack_top;
    struct boot_cpu *cpu;
} dispatch[LIMINE_MAX_CPUS];
static struct boot_smp smp;

static void limine_ap_entry(struct limine_mp_info *native) {
    unsigned index = (unsigned)native->extra_argument;
    if (index >= smp.cpu_count)
        for (;;)
            __asm__ volatile("cli; hlt");
    boot_ap_enter(dispatch[index].entry, dispatch[index].cpu, dispatch[index].argument,
                  dispatch[index].stack_top);
}

static bool limine_start(struct boot_smp *mp, struct boot_cpu *cpu, boot_ap_entry entry,
                         uintptr_t argument, uintptr_t stack_top) {
    (void)mp; /* Limine supplies the initial tables; use the scheduler's stack. */
    unsigned index = (unsigned)(cpu - cpus);
    if (index >= smp.cpu_count || !cpu->loader_data)
        return false;
    struct limine_mp_info *native = cpu->loader_data;
    dispatch[index].entry = entry;
    dispatch[index].argument = argument;
    dispatch[index].cpu = cpu;
    dispatch[index].stack_top = stack_top;
    native->extra_argument = index;
    __atomic_store_n(&native->goto_address, limine_ap_entry, __ATOMIC_RELEASE);
    return true;
}

bool boot_limine_available(void) {
    return LIMINE_BASE_REVISION_SUPPORTED(base_revision) && memmap_request.response &&
           hhdm_request.response;
}

void boot_limine_init(void) {
    if (!boot_limine_available())
        panic("Limine revision 6 memory responses required");

    uint64_t count = memmap_request.response->entry_count;
    if (count > BOOT_MAX_MEMMAP)
        panic("too many Limine memory ranges");

    for (uint64_t i = 0; i < count; i++) {
        struct limine_memmap_entry *native = memmap_request.response->entries[i];
        if (!native)
            panic("missing Limine memory range");
        uint64_t type;
        switch (native->type) {
        case LIMINE_MEMMAP_USABLE:
            type = BOOT_MEMORY_USABLE;
            break;
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            type = BOOT_MEMORY_ACPI_RECLAIMABLE;
            break;
        case LIMINE_MEMMAP_ACPI_NVS:
            type = BOOT_MEMORY_ACPI_NVS;
            break;
        case LIMINE_MEMMAP_BAD_MEMORY:
            type = BOOT_MEMORY_BAD;
            break;
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            type = BOOT_MEMORY_RECLAIMABLE;
            break;
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            type = BOOT_MEMORY_KERNEL;
            break;
        case LIMINE_MEMMAP_FRAMEBUFFER:
            type = BOOT_MEMORY_FRAMEBUFFER;
            break;
        default:
            type = BOOT_MEMORY_RESERVED;
            break;
        }
        memory_map[i] = (struct boot_memory_range){native->base, native->length, type};
    }

    boot_info = (struct boot_info){
        .magic = BOOT_INFO_MAGIC,
        .version = BOOT_INFO_VERSION,
        .hhdm_offset = hhdm_request.response->offset,
        .memory_map = memory_map,
        .memory_map_count = count,
        .loader_name = loader_request.response ? loader_request.response->name : "Limine",
        .cmdline = cmdline_request.response ? cmdline_request.response->cmdline : ""};

    if (module_request.response) {
        count = module_request.response->module_count;
        if (count > BOOT_MAX_MODULES)
            panic("too many Limine modules");
        for (uint64_t i = 0; i < count; i++) {
            struct limine_file *native = module_request.response->modules[i];
            if (!native)
                panic("missing Limine module");
            modules[i] =
                (struct boot_module){native->address, native->size, native->path, native->string};
        }
        boot_info.modules = modules;
        boot_info.module_count = count;
    }

    if (framebuffer_request.response && framebuffer_request.response->framebuffer_count) {
        struct limine_framebuffer *f = framebuffer_request.response->framebuffers[0];
        if (f) {
            boot_fb = (struct boot_framebuffer){.address = f->address,
                                                .width = f->width,
                                                .height = f->height,
                                                .pitch = f->pitch,
                                                .bpp = f->bpp,
                                                .memory_model = f->memory_model,
                                                .red_mask_size = f->red_mask_size,
                                                .red_mask_shift = f->red_mask_shift,
                                                .green_mask_size = f->green_mask_size,
                                                .green_mask_shift = f->green_mask_shift,
                                                .blue_mask_size = f->blue_mask_size,
                                                .blue_mask_shift = f->blue_mask_shift};
            boot_info.framebuffer = &boot_fb;
        }
    }
    if (rsdp_request.response)
        boot_info.rsdp = rsdp_request.response->address;
    if (date_request.response)
        boot_info.epoch = date_request.response->timestamp;
    if (mp_request.response) {
        struct limine_mp_response *native = mp_request.response;
        if (!native->cpu_count)
            panic("invalid Limine CPU count");
        /* Keep the scheduler's existing CPU limit without refusing to boot
           machines where Limine reports more processors than our adapter. */
        uint64_t cpu_count =
            native->cpu_count < LIMINE_MAX_CPUS ? native->cpu_count : LIMINE_MAX_CPUS;
        for (uint64_t i = 0; i < cpu_count; i++) {
            struct limine_mp_info *cpu = native->cpus[i];
            if (!cpu)
                panic("missing Limine CPU");
            cpus[i] = (struct boot_cpu){
                .processor_id = cpu->processor_id, .lapic_id = cpu->lapic_id, .loader_data = cpu};
        }
        smp = (struct boot_smp){.bsp_lapic_id = native->bsp_lapic_id,
                                .cpu_count = cpu_count,
                                .cpus = cpus,
                                .start = limine_start};
        boot_info.smp = &smp;
    }
}
