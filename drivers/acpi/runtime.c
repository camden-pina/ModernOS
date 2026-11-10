/* BSP-only preparation for the OSL's immutable runtime mapping/GPE policy. */
/* This driver uses kernel services; do not import ACPICA's private libc ABI. */
#define ACPI_USE_SYSTEM_CLIBRARY
#include <acpi.h>
#include <accommon.h>
#include <acdispat.h>
#include <acevents.h>
#include <acinterp.h>
#include <acnamesp.h>
#include <amlcode.h>
#include <acpi/osl.h>
#include <mm/pmm.h>
#include <printk.h>
#include <sched.h>

#define PREPARE_REGION_LIMIT (64u * 1024 * 1024)
#define PREPARE_PAGE_BUDGET (65536u) /* 256 MiB of bounded preparation work. */
static struct acpi_runtime_status runtime_status;
static bool gpes_prepared;

struct gpe_snapshot {
    void **contexts;
    size_t count, capacity;
};

static ACPI_STATUS snapshot_gpes(ACPI_GPE_XRUPT_INFO *interrupt,
                                ACPI_GPE_BLOCK_INFO *block, void *context) {
    (void)interrupt;
    struct gpe_snapshot *snapshot = context;
    if (block->GpeCount && !block->EventInfo)
        return AE_BAD_PARAMETER;
    if (block->GpeCount > SIZE_MAX - snapshot->count)
        return AE_LIMIT;
    if (snapshot->contexts) {
        if (snapshot->count + block->GpeCount > snapshot->capacity)
            return AE_LIMIT;
        for (unsigned i = 0; i < block->GpeCount; i++)
            snapshot->contexts[snapshot->count + i] = &block->EventInfo[i];
    }
    snapshot->count += block->GpeCount;
    return AE_OK;
}

bool acpi_os_prepare_gpes(void) {
    if (gpes_prepared)
        return true;
    if (sched_ready())
        return false;
    struct gpe_snapshot snapshot = {0};
    ACPI_STATUS status = AcpiEvWalkGpeList(snapshot_gpes, &snapshot);
    if (ACPI_FAILURE(status) || snapshot.count > SIZE_MAX / sizeof(void *))
        return false;
    snapshot.capacity = snapshot.count;
    snapshot.contexts = snapshot.count ? kmalloc(snapshot.count * sizeof(void *)) : NULL;
    if (snapshot.count && !snapshot.contexts)
        return false;
    snapshot.count = 0;
    /* The walker holds the GPE spinlock. The callbacks do not allocate, run
       AML or enter any other ACPICA API; the BSP owns topology construction. */
    status = AcpiEvWalkGpeList(snapshot_gpes, &snapshot);
    bool success = ACPI_SUCCESS(status) && snapshot.count == snapshot.capacity &&
                   acpi_os_reserve_gpes(snapshot.contexts, snapshot.count);
    kfree(snapshot.contexts);
    if (success) {
        gpes_prepared = true;
        pr_info("ACPICA: reserved dispatch/ordered-finish slots for %u GPEs\n",
                (unsigned)snapshot.count);
    }
    return success;
}

/* Only recognize the side-effect-free literal subset. Extra.AmlStart/Length
 * come from ACPICA's validated namespace declaration. Do not speculate through
 * name references, methods, Fields or arithmetic to warm an unused region. */
static bool skip_literal_integer(const uint8_t **cursor, const uint8_t *end) {
    if (*cursor == end)
        return false;
    unsigned bytes;
    switch (*(*cursor)++) {
    case AML_ZERO_OP: case AML_ONE_OP: case AML_ONES_OP:
        return true;
    case AML_BYTE_OP: bytes = 1; break;
    case AML_WORD_OP: bytes = 2; break;
    case AML_DWORD_OP: bytes = 4; break;
    case AML_QWORD_OP: bytes = 8; break;
    default: return false;
    }
    if ((size_t)(end - *cursor) < bytes)
        return false;
    *cursor += bytes;
    return true;
}

static bool literal_region_arguments(ACPI_OPERAND_OBJECT *region) {
    ACPI_OPERAND_OBJECT *extra = AcpiNsGetSecondaryObject(region);
    if (!extra || !extra->Extra.AmlStart || extra->Extra.AmlLength < 9 ||
        extra->Extra.AmlLength > 2 + 1 + 2 + 255 * 4 + 1 + 18)
        return false;
    const uint8_t *cursor = extra->Extra.AmlStart;
    const uint8_t *end = cursor + extra->Extra.AmlLength;
    if (*cursor++ != AML_EXTENDED_PREFIX || *cursor++ != (AML_REGION_OP & 255))
        return false;
    if (cursor < end && *cursor == AML_ROOT_PREFIX)
        cursor++;
    else
        while (cursor < end && *cursor == AML_PARENT_PREFIX)
            cursor++;
    if (cursor == end)
        return false;
    unsigned segments = 1;
    if (*cursor == AML_DUAL_NAME_PREFIX) {
        segments = 2;
        cursor++;
    } else if (*cursor == AML_MULTI_NAME_PREFIX) {
        cursor++;
        if (cursor == end || !(segments = *cursor++))
            return false;
    }
    if ((size_t)(end - cursor) < (size_t)segments * 4 + 1)
        return false;
    for (unsigned i = 0; i < segments * 4; i++) {
        uint8_t c = *cursor++;
        if (c != '_' && !(c >= 'A' && c <= 'Z') &&
            (!(i % 4) || c < '0' || c > '9'))
            return false;
    }
    if (*cursor++ != ACPI_ADR_SPACE_SYSTEM_MEMORY)
        return false;
    return skip_literal_integer(&cursor, end) && skip_literal_integer(&cursor, end) &&
           cursor == end;
}

static bool prepare_range(uint64_t address, uint64_t length, uint64_t maximum) {
    const uint64_t limit = UINT64_C(0x0000800000000000);
    if (!length || length > maximum || address >= limit || length > limit - address)
        return false;
    uint64_t pages = ((address & 4095) + length + 4095) / 4096;
    if (pages > PREPARE_PAGE_BUDGET - runtime_status.prepared_pages)
        return false;
    /* AcpiOsMapMemory groups contiguous typed pages, preserving known RAM/NVS
       cache attributes even when a region spans both RAM and MMIO. */
    if (!AcpiOsMapMemory(address, length))
        return false;
    runtime_status.prepared_pages += pages;
    return true;
}

static ACPI_STATUS prepare_region(ACPI_HANDLE object, UINT32 level,
                                  void *context, void **result) {
    (void)level; (void)context; (void)result;
    ACPI_OPERAND_OBJECT *region = AcpiNsGetAttachedObject(object);
    if (!region || region->Common.Type != ACPI_TYPE_REGION ||
        region->Region.SpaceId != ACPI_ADR_SPACE_SYSTEM_MEMORY)
        return AE_OK;
    runtime_status.memory_regions++;
    ACPI_STATUS status = AE_OK;
    if (!(region->Common.Flags & AOPOBJ_DATA_VALID)) {
        if (!literal_region_arguments(region)) {
            runtime_status.deferred_regions++;
            return AE_OK;
        }
        /* Namespace walking releases its mutex around this callback. This
           whitelisted declaration contains only literals, but ACPICA's deferred
           parser still requires the interpreter lock. SCI/IF remains enabled. */
        AcpiExEnterInterpreter();
        status = AcpiDsGetRegionArguments(region);
        AcpiExExitInterpreter();
    }
    if (ACPI_FAILURE(status) ||
        !prepare_range(region->Region.Address, region->Region.Length, PREPARE_REGION_LIMIT)) {
        runtime_status.memory_region_failures++;
        pr_warn("ACPICA: SystemMemory region %.4s unprepared (status 0x%x, address 0x%llx, length %u)\n",
                ((ACPI_NAMESPACE_NODE *)object)->Name.Ascii, status,
                (unsigned long long)region->Region.Address, region->Region.Length);
    } else {
        runtime_status.memory_regions_prepared++;
    }
    return AE_OK; /* Optional firmware objects must not abort the boot walk. */
}

static bool prepare_gas(const ACPI_GENERIC_ADDRESS *gas) {
    if (!gas->Address || !gas->BitWidth || gas->BitWidth > 64 ||
        gas->BitOffset + gas->BitWidth > 64 || gas->AccessWidth > 4 ||
        (gas->SpaceId != ACPI_ADR_SPACE_SYSTEM_MEMORY &&
         gas->SpaceId != ACPI_ADR_SPACE_SYSTEM_IO))
        return false;
    /* Match hwregs.c's access-unit rounding, including a final partial unit
       crossing a page. Merely mapping ceil(BitWidth/8) can miss that page. */
    unsigned bits = gas->BitOffset + gas->BitWidth, access;
    if (!gas->BitOffset && !(gas->BitWidth & (gas->BitWidth - 1)) && gas->BitWidth >= 8)
        access = gas->BitWidth;
    else if (gas->AccessWidth)
        access = 8u << (gas->AccessWidth - 1);
    else {
        access = 8;
        while (access < bits)
            access <<= 1;
        while (access > 8 && (gas->Address & (access / 8 - 1)))
            access >>= 1;
    }
    if (gas->SpaceId == ACPI_ADR_SPACE_SYSTEM_IO && access > 32)
        access = 32;
    unsigned bytes = ((bits + access - 1) / access) * (access / 8);
    if (gas->SpaceId == ACPI_ADR_SPACE_SYSTEM_IO)
        return gas->Address <= UINT16_MAX && bytes - 1 <= UINT16_MAX - gas->Address;
    return prepare_range(gas->Address, bytes, 16);
}

static void prepare_fixed_registers(void) {
    /* ACPICA's normalized FADT includes legacy-to-extended GAS conversion and
       zeros missing revision fields; use it instead of assuming raw length. */
    const ACPI_TABLE_FADT *fadt = &AcpiGbl_FADT;
    ACPI_GENERIC_ADDRESS reset = fadt->ResetRegister;
    /* AcpiReset intentionally ignores malformed I/O reset-register widths and
       writes exactly one byte, for firmware compatibility (hwxface.c). */
    if (reset.SpaceId == ACPI_ADR_SPACE_SYSTEM_IO) {
        reset.BitWidth = 8;
        reset.BitOffset = reset.AccessWidth = 0;
    }
    runtime_status.reset_mapping_ready = (fadt->Flags & ACPI_FADT_RESET_REGISTER) && prepare_gas(&reset);
    if ((fadt->Flags & ACPI_FADT_RESET_REGISTER) && !runtime_status.reset_mapping_ready)
        runtime_status.fixed_mapping_failures++;
    const ACPI_GENERIC_ADDRESS *required[2];
    if (fadt->Flags & ACPI_FADT_HW_REDUCED) {
        required[0] = &fadt->SleepControl;
        required[1] = &fadt->SleepStatus;
    } else {
        required[0] = &fadt->XPm1aControlBlock;
        required[1] = &fadt->XPm1aEventBlock; /* Wake status is touched by S5 too. */
    }
    runtime_status.sleep_mapping_ready = true;
    for (unsigned i = 0; i < 2; i++)
        if (!prepare_gas(required[i])) {
            runtime_status.sleep_mapping_ready = false;
            runtime_status.fixed_mapping_failures++;
        }
    if (!(fadt->Flags & ACPI_FADT_HW_REDUCED)) {
        const ACPI_GENERIC_ADDRESS *optional[] = {
            &fadt->XPm1bControlBlock, &fadt->XPm1bEventBlock,
            &fadt->XPm2ControlBlock, &fadt->XPmTimerBlock,
        };
        for (unsigned i = 0; i < sizeof(optional) / sizeof(optional[0]); i++)
            if (optional[i]->Address && !prepare_gas(optional[i])) {
                runtime_status.fixed_mapping_failures++;
                runtime_status.sleep_mapping_ready = false;
            }
    }
}

bool acpi_os_prepare_runtime(void) {
    if (runtime_status.prepared)
        return true;
    if (sched_ready() || !gpes_prepared)
        return false;
    prepare_fixed_registers(); /* Power register coverage takes budget priority. */
    ACPI_STATUS status = AcpiWalkNamespace(ACPI_TYPE_REGION, ACPI_ROOT_OBJECT,
                                         ACPI_UINT32_MAX, prepare_region, NULL, NULL, NULL);
    if (ACPI_FAILURE(status))
        return false;
    runtime_status.prepared = true;
    pr_info("ACPICA: runtime mappings %u/%u SystemMemory regions, %u failed, %u deferred; %llu pages; reset=%u sleep=%u fixed-failures=%u\n",
            runtime_status.memory_regions_prepared, runtime_status.memory_regions,
            runtime_status.memory_region_failures, runtime_status.deferred_regions,
            (unsigned long long)runtime_status.prepared_pages,
            runtime_status.reset_mapping_ready, runtime_status.sleep_mapping_ready,
            runtime_status.fixed_mapping_failures);
    return true;
}

void acpi_os_get_runtime_status(struct acpi_runtime_status *status) {
    if (status)
        *status = runtime_status; /* Immutable after BSP preparation. */
}
