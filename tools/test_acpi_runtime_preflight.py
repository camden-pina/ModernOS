#!/usr/bin/env python3
"""Sanitize the real runtime preflight with mocked kernel/ACPICA boundaries.

The production runtime.c is included unchanged and compiled against the vendored
ACPICA headers. Exact-sized raw AML declarations test the literal-only filter;
spies prove rejected declarations never reach deferred argument evaluation.
The interpreter, namespace/GPE walkers, normalized FADT, allocation, and mapping
boundaries are mocks. This is not an AML-interpreter, page-table, or hardware
test; guest/hardware tests must establish those integration properties.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

KERNEL = r'''
#ifndef TEST_KERNEL_H
#define TEST_KERNEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool sched_ready(void);
void *kmalloc(size_t);
void kfree(void *);
int printk(const char *, ...);
#define pr_info(...) printk(__VA_ARGS__)
#define pr_warn(...) printk(__VA_ARGS__)
#endif
'''

SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_kernel.h"
#include "RUNTIME_SOURCE"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_GPE_ENTRIES 2048
ACPI_TABLE_FADT AcpiGbl_FADT;
static bool ready, allocation_fails, reservation_fails, mapping_fails;
static bool inside_gpe_walk;
static unsigned allocations, frees, live_allocations, reservation_calls;
static unsigned gpe_walks, fail_gpe_walk, mutate_gpe_walk;
static int mutate_gpe_delta;
static unsigned maps, fail_map_call, interpreter_enters, interpreter_exits;
static unsigned interpreter_depth, argument_calls, namespace_walks;
static ACPI_STATUS argument_status, namespace_status;
static uint64_t resolved_address;
static uint32_t resolved_length;
static struct { uint64_t address, length; } mappings[64];
static ACPI_GPE_XRUPT_INFO interrupts[3];
static ACPI_GPE_BLOCK_INFO blocks[4];
static ACPI_GPE_EVENT_INFO events[MAX_GPE_ENTRIES];
static unsigned block_count;
static void *reserved[MAX_GPE_ENTRIES];
static size_t reserved_count;
static ACPI_NAMESPACE_NODE nodes[16];
static ACPI_OPERAND_OBJECT regions[16], extras[16];
static unsigned node_count, fixture_count;

bool sched_ready(void) { return ready; }
void *kmalloc(size_t length) {
    assert(!inside_gpe_walk); /* The GPE walker models its held spinlock. */
    allocations++;
    if (allocation_fails)
        return NULL;
    void *memory = malloc(length);
    assert(memory);
    live_allocations++;
    return memory;
}
void kfree(void *memory) {
    assert(!inside_gpe_walk);
    if (memory) {
        assert(live_allocations);
        live_allocations--;
        frees++;
    }
    free(memory);
}
int printk(const char *format, ...) { (void)format; return 0; }
void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS address, ACPI_SIZE length) {
    assert(!inside_gpe_walk && !interpreter_depth);
    assert(maps < ARRAY_SIZE(mappings));
    mappings[maps].address = address;
    mappings[maps++].length = length;
    return mapping_fails || maps == fail_map_call ? NULL : mappings;
}
bool acpi_os_reserve_gpes(void *const *contexts, size_t count) {
    assert(!inside_gpe_walk && count <= MAX_GPE_ENTRIES);
    assert(!count || contexts);
    reservation_calls++;
    reserved_count = count;
    if (count)
        memcpy(reserved, contexts, count * sizeof(*contexts));
    return !reservation_fails;
}
ACPI_STATUS AcpiEvWalkGpeList(ACPI_GPE_CALLBACK callback, void *context) {
    assert(!inside_gpe_walk);
    gpe_walks++;
    if (gpe_walks == fail_gpe_walk)
        return AE_ERROR;
    if (gpe_walks == mutate_gpe_walk) {
        assert(block_count && (int)blocks[0].GpeCount + mutate_gpe_delta >= 0);
        blocks[0].GpeCount += mutate_gpe_delta;
    }
    inside_gpe_walk = true;
    ACPI_STATUS status = AE_OK;
    for (unsigned i = 0; i < block_count; i++) {
        status = callback(blocks[i].XruptBlock, &blocks[i], context);
        if (ACPI_FAILURE(status))
            break;
    }
    inside_gpe_walk = false;
    return status;
}
ACPI_OPERAND_OBJECT *AcpiNsGetAttachedObject(ACPI_NAMESPACE_NODE *node) {
    assert(node);
    return node->Object;
}
ACPI_OPERAND_OBJECT *AcpiNsGetSecondaryObject(ACPI_OPERAND_OBJECT *region) {
    return region->Common.NextObject;
}
void AcpiExEnterInterpreter(void) {
    assert(!interpreter_depth && !inside_gpe_walk);
    interpreter_enters++;
    interpreter_depth++;
}
void AcpiExExitInterpreter(void) {
    assert(interpreter_depth == 1);
    interpreter_exits++;
    interpreter_depth--;
}
ACPI_STATUS AcpiDsGetRegionArguments(ACPI_OPERAND_OBJECT *region) {
    assert(interpreter_depth == 1 && !inside_gpe_walk);
    argument_calls++;
    if (ACPI_SUCCESS(argument_status)) {
        region->Region.Address = resolved_address;
        region->Region.Length = resolved_length;
        region->Common.Flags |= AOPOBJ_DATA_VALID;
    }
    return argument_status;
}
ACPI_STATUS AcpiWalkNamespace(ACPI_OBJECT_TYPE type, ACPI_HANDLE start,
                            UINT32 depth, ACPI_WALK_CALLBACK descending,
                            ACPI_WALK_CALLBACK ascending, void *context,
                            void **result) {
    assert(type == ACPI_TYPE_REGION && start == ACPI_ROOT_OBJECT);
    assert(depth == ACPI_UINT32_MAX && descending && !ascending);
    assert(!context && !result && !ready && gpes_prepared);
    namespace_walks++;
    if (ACPI_FAILURE(namespace_status))
        return namespace_status;
    for (unsigned i = 0; i < node_count; i++)
        assert(descending(&nodes[i], i, context, result) == AE_OK);
    return AE_OK;
}

static void reset_fixture(void) {
    assert(!live_allocations && !interpreter_depth && !inside_gpe_walk);
    fixture_count++;
    memset(&runtime_status, 0, sizeof(runtime_status));
    memset(&AcpiGbl_FADT, 0, sizeof(AcpiGbl_FADT));
    memset(interrupts, 0, sizeof(interrupts));
    memset(blocks, 0, sizeof(blocks));
    memset(events, 0, sizeof(events));
    memset(nodes, 0, sizeof(nodes));
    memset(regions, 0, sizeof(regions));
    memset(extras, 0, sizeof(extras));
    memset(mappings, 0, sizeof(mappings));
    memset(reserved, 0, sizeof(reserved));
    gpes_prepared = ready = allocation_fails = reservation_fails = mapping_fails = false;
    allocations = frees = reservation_calls = gpe_walks = fail_gpe_walk = 0;
    mutate_gpe_walk = maps = fail_map_call = interpreter_enters = 0;
    interpreter_exits = argument_calls = namespace_walks = 0;
    block_count = node_count = 0;
    reserved_count = 0;
    mutate_gpe_delta = 0;
    namespace_status = argument_status = AE_OK;
    resolved_address = 0x8123;
    resolved_length = 8192;
}
static ACPI_OPERAND_OBJECT *new_region(unsigned space, bool valid,
                                       uint64_t address, uint32_t length) {
    assert(node_count < ARRAY_SIZE(nodes));
    unsigned i = node_count++;
    nodes[i].Object = &regions[i];
    nodes[i].Type = ACPI_TYPE_REGION;
    memcpy(nodes[i].Name.Ascii, "REGN", 4);
    regions[i].Common.Type = ACPI_TYPE_REGION;
    regions[i].Common.NextObject = &extras[i];
    regions[i].Common.Flags = valid ? AOPOBJ_DATA_VALID : 0;
    regions[i].Region.Node = &nodes[i];
    regions[i].Region.SpaceId = space;
    regions[i].Region.Address = address;
    regions[i].Region.Length = length;
    extras[i].Common.Type = ACPI_TYPE_LOCAL_EXTRA;
    return &regions[i];
}
static void visit(unsigned index) {
    assert(index < node_count);
    assert(prepare_region(&nodes[index], 0, NULL, NULL) == AE_OK);
    assert(!interpreter_depth && interpreter_enters == interpreter_exits);
}
static ACPI_GENERIC_ADDRESS gas(unsigned space, uint64_t address,
                                 unsigned width, unsigned offset, unsigned access) {
    ACPI_GENERIC_ADDRESS value = {0};
    value.SpaceId = space;
    value.Address = address;
    value.BitWidth = width;
    value.BitOffset = offset;
    value.AccessWidth = access;
    return value;
}

struct bytes { const uint8_t *data; size_t length; };
static const uint8_t single_name[] = {'R','E','G','0'};
static const uint8_t dual_name[] = {AML_DUAL_NAME_PREFIX,'A','B','C','D','R','E','G','0'};
static const uint8_t multi_name[] = {AML_MULTI_NAME_PREFIX,3,
    '_','S','B','_','P','C','I','0','R','E','G','0'};
static const uint8_t root_name[] = {AML_ROOT_PREFIX,'R','E','G','0'};
static const uint8_t root_dual[] = {AML_ROOT_PREFIX,AML_DUAL_NAME_PREFIX,
    'A','B','C','D','R','E','G','0'};
static const uint8_t root_multi[] = {AML_ROOT_PREFIX,AML_MULTI_NAME_PREFIX,2,
    '_','S','B','_','R','E','G','0'};
static const uint8_t parent_name[] = {AML_PARENT_PREFIX,'_','R','0','_' };
static const uint8_t parent_dual[] = {AML_PARENT_PREFIX,AML_PARENT_PREFIX,
    AML_DUAL_NAME_PREFIX,'A','B','C','D','R','E','G','0'};
static const uint8_t parent_multi[] = {AML_PARENT_PREFIX,AML_PARENT_PREFIX,
    AML_PARENT_PREFIX,AML_MULTI_NAME_PREFIX,2,'A','B','C','D','R','E','G','0'};
static const struct bytes names[] = {
    {single_name,sizeof(single_name)}, {dual_name,sizeof(dual_name)},
    {multi_name,sizeof(multi_name)}, {root_name,sizeof(root_name)},
    {root_dual,sizeof(root_dual)}, {root_multi,sizeof(root_multi)},
    {parent_name,sizeof(parent_name)}, {parent_dual,sizeof(parent_dual)},
    {parent_multi,sizeof(parent_multi)},
};
static const uint8_t zero[] = {AML_ZERO_OP}, one[] = {AML_ONE_OP}, ones[] = {AML_ONES_OP};
static const uint8_t byte[] = {AML_BYTE_OP,0x32};
static const uint8_t word[] = {AML_WORD_OP,0x32,0x81};
static const uint8_t dword[] = {AML_DWORD_OP,0x32,0x81,0x56,0x78};
static const uint8_t qword[] = {AML_QWORD_OP,0x32,0x81,0x56,0x78,0x9a,0xbc,0xde,0xf0};
static const struct bytes integers[] = {
    {zero,sizeof(zero)}, {one,sizeof(one)}, {ones,sizeof(ones)},
    {byte,sizeof(byte)}, {word,sizeof(word)}, {dword,sizeof(dword)},
    {qword,sizeof(qword)},
};
static size_t declaration(uint8_t *out, struct bytes name,
                          struct bytes address, struct bytes length) {
    size_t n = 0;
    out[n++] = AML_EXTENDED_PREFIX;
    out[n++] = AML_REGION_OP & 255;
    memcpy(out + n, name.data, name.length); n += name.length;
    out[n++] = ACPI_ADR_SPACE_SYSTEM_MEMORY;
    memcpy(out + n, address.data, address.length); n += address.length;
    memcpy(out + n, length.data, length.length); n += length.length;
    return n;
}
static void check_declaration(const uint8_t *data, size_t length, bool allowed) {
    reset_fixture();
    /* ASan redzones immediately follow the declared AML span. */
    uint8_t *aml = malloc(length ? length : 1);
    assert(aml);
    memcpy(aml, data, length);
    ACPI_OPERAND_OBJECT *region = new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, false, 0, 0);
    extras[0].Extra.AmlStart = aml;
    extras[0].Extra.AmlLength = length;
    assert(literal_region_arguments(region) == allowed);
    visit(0);
    assert(runtime_status.memory_regions == 1 && !runtime_status.memory_region_failures);
    assert(argument_calls == (unsigned)allowed && interpreter_enters == (unsigned)allowed);
    assert(maps == (unsigned)allowed && runtime_status.memory_regions_prepared == (unsigned)allowed);
    assert(runtime_status.deferred_regions == (unsigned)!allowed);
    if (allowed) {
        assert(mappings[0].address == resolved_address && mappings[0].length == resolved_length);
        assert(runtime_status.prepared_pages == 3);
    }
    free(aml);
}
static void literal_tests(void) {
    uint8_t aml[2048];
    for (unsigned n = 0; n < ARRAY_SIZE(names); n++)
        for (unsigned a = 0; a < ARRAY_SIZE(integers); a++)
            for (unsigned l = 0; l < ARRAY_SIZE(integers); l++) {
                size_t length = declaration(aml, names[n], integers[a], integers[l]);
                check_declaration(aml, length, true);
            }
    /* MultiName's count byte includes its maximum, 255, and count-one form. */
    uint8_t maximal_name[1023];
    maximal_name[0] = AML_MULTI_NAME_PREFIX;
    maximal_name[1] = 255;
    for (unsigned i = 0; i < 255; i++)
        memcpy(maximal_name + 2 + 4 * i, single_name, 4);
    struct bytes maximal = {maximal_name, 1022};
    size_t length = declaration(aml, maximal, integers[6], integers[6]);
    check_declaration(aml, length, true);
    memmove(maximal_name + 1, maximal_name, maximal.length);
    maximal_name[0] = AML_ROOT_PREFIX;
    maximal.length++;
    length = declaration(aml, maximal, integers[6], integers[6]);
    assert(length == 1044);
    check_declaration(aml, length, true);
    maximal_name[0] = AML_PARENT_PREFIX;
    length = declaration(aml, maximal, integers[6], integers[6]);
    check_declaration(aml, length, true);
    maximal_name[0] = AML_MULTI_NAME_PREFIX;
    maximal_name[1] = 1;
    memcpy(maximal_name + 2, single_name, sizeof(single_name));
    maximal.length = 6;
    length = declaration(aml, maximal, integers[0], integers[1]);
    check_declaration(aml, length, true);
}
static void reject_expression_tests(void) {
    static const uint8_t expressions[][9] = {
        {AML_ADD_OP,AML_ZERO_OP,AML_ONE_OP,AML_ZERO_OP},
        {'M','T','H','D'}, /* Method invocation NameString. */
        {'A','D','D','R'}, /* Named integer reference. */
        {'F','L','D','0'}, /* Field reference, potentially device I/O. */
        {AML_METHOD_OP,6,'M','T','H','D',0},
        {AML_EXTENDED_PREFIX,AML_FIELD_OP & 255,1},
        {0x60}, /* Local0. */ {0x68}, /* Arg0. */
        {0x83,'F','L','D','0'}, /* DerefOf. */
        {AML_STRING_OP,'x',0}, {AML_BUFFER_OP,2,AML_ZERO_OP},
        {AML_PACKAGE_OP,2,0},
    };
    static const unsigned lengths[] = {4,4,4,4,7,3,1,1,5,3,3,3};
    _Static_assert(ARRAY_SIZE(expressions) == ARRAY_SIZE(lengths), "expression sizes");
    uint8_t aml[64];
    for (unsigned i = 0; i < ARRAY_SIZE(expressions); i++) {
        struct bytes expression = {expressions[i],lengths[i]};
        size_t length = declaration(aml, names[0], expression, integers[1]);
        check_declaration(aml, length, false);
        length = declaration(aml, names[0], integers[0], expression);
        check_declaration(aml, length, false);
    }
    /* An unused/absent deferred declaration is a tolerated unresolved region. */
    reset_fixture();
    ACPI_OPERAND_OBJECT *region = new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, false, 0, 0);
    region->Common.NextObject = NULL;
    visit(0);
    region->Common.NextObject = &extras[0];
    visit(0); /* Missing AmlStart and zero AmlLength. */
    assert(runtime_status.deferred_regions == 2 && !argument_calls && !maps);
    assert(!runtime_status.memory_region_failures);
}
static void malformed_aml_tests(void) {
    uint8_t aml[2048], original[2048];
    size_t length = declaration(original, names[5], integers[6], integers[6]);
    for (size_t n = 0; n < length; n++)
        check_declaration(original, n, false);
    for (unsigned integer = 3; integer < ARRAY_SIZE(integers); integer++) {
        length = declaration(original, names[0], integers[integer], integers[integer]);
        for (size_t n = 0; n < length; n++)
            check_declaration(original, n, false);
    }
    length = declaration(original, names[0], integers[0], integers[1]);
    static const struct { unsigned offset; uint8_t value; } corruptions[] = {
        {0,0}, {1,0}, {2,'0'}, {2,'a'}, {3,'a'}, {4,'-'},
        {2,0}, {6,ACPI_ADR_SPACE_SYSTEM_IO}, {7,0x02}, {8,0x02},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(corruptions); i++) {
        memcpy(aml, original, length);
        aml[corruptions[i].offset] = corruptions[i].value;
        check_declaration(aml, length, false);
    }
    memcpy(aml, original, length);
    aml[length] = AML_ZERO_OP;
    check_declaration(aml, length + 1, false); /* Trailing operand. */
    static const uint8_t bad_names[][10] = {
        {AML_MULTI_NAME_PREFIX,0}, {AML_DUAL_NAME_PREFIX,'A','B','C','D'},
        {AML_MULTI_NAME_PREFIX,2,'A','B','C','D'},
        {AML_ROOT_PREFIX,AML_PARENT_PREFIX,'R','E','G','0'},
        {AML_PARENT_PREFIX,AML_ROOT_PREFIX,'R','E','G','0'},
        {AML_ROOT_PREFIX,AML_ROOT_PREFIX,'R','E','G','0'},
        {AML_ROOT_PREFIX}, {AML_PARENT_PREFIX}, {AML_MULTI_NAME_PREFIX},
    };
    static const unsigned bad_name_lengths[] = {2,5,6,6,6,6,1,1,1};
    for (unsigned i = 0; i < ARRAY_SIZE(bad_names); i++) {
        struct bytes name = {bad_names[i],bad_name_lengths[i]};
        length = declaration(aml, name, integers[0], integers[1]);
        check_declaration(aml, length, false);
    }
    memset(aml, 'A', sizeof(aml));
    check_declaration(aml, sizeof(aml), false); /* Bounded declaration length. */
    reset_fixture();
    ACPI_OPERAND_OBJECT *region = new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, false, 0, 0);
    uint8_t prefix = AML_EXTENDED_PREFIX;
    extras[0].Extra.AmlStart = &prefix;
    extras[0].Extra.AmlLength = UINT32_MAX;
    assert(!literal_region_arguments(region)); /* Reject before examining bytes. */
    visit(0);
    assert(runtime_status.deferred_regions == 1 && !argument_calls && !maps);
}

static void region_and_budget_tests(void) {
    reset_fixture();
    new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, true, 0x2fff, 2);
    new_region(ACPI_ADR_SPACE_SYSTEM_IO, false, 0, 0);
    new_region(ACPI_ADR_SPACE_PCI_CONFIG, true, 0, 4096);
    new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, true, 0, 1);
    for (unsigned i = 0; i < node_count; i++)
        visit(i);
    assert(runtime_status.memory_regions == 2 && runtime_status.memory_regions_prepared == 2);
    assert(maps == 2 && runtime_status.prepared_pages == 3 && !argument_calls);
    assert(!runtime_status.deferred_regions && !runtime_status.memory_region_failures);
    nodes[0].Object = NULL;
    visit(0);
    nodes[0].Object = &regions[0];
    regions[0].Common.Type = ACPI_TYPE_INTEGER;
    visit(0);
    assert(runtime_status.memory_regions == 2);

    static const struct { uint64_t address; uint32_t length; } invalid[] = {
        {0x1000,0}, {0x1000,PREPARE_REGION_LIMIT + 1},
        {UINT64_C(0x0000800000000000),1},
        {UINT64_C(0x00007fffffffffff),2}, {UINT64_MAX,1},
    };
    reset_fixture();
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, true, invalid[i].address, invalid[i].length);
        visit(i);
    }
    assert(runtime_status.memory_region_failures == ARRAY_SIZE(invalid) && !maps);
    assert(!runtime_status.prepared_pages);

    reset_fixture();
    /* Aligned 64-MiB ranges exactly exhaust the 256-MiB page budget. */
    for (unsigned i = 0; i < 4; i++) {
        new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, true,
                   UINT64_C(0x10000000) + (uint64_t)i * PREPARE_REGION_LIMIT,
                   PREPARE_REGION_LIMIT);
        visit(i);
    }
    new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, true, 0x1000, 1);
    visit(4);
    assert(maps == 4 && runtime_status.prepared_pages == PREPARE_PAGE_BUDGET);
    assert(runtime_status.memory_regions_prepared == 4 && runtime_status.memory_region_failures == 1);

    reset_fixture();
    runtime_status.prepared_pages = PREPARE_PAGE_BUDGET - 1;
    new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, true, 0x1fff, 2);
    visit(0); /* Two pages cannot consume the remaining single page. */
    new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, true, 0x1fff, 1);
    visit(1);
    assert(maps == 1 && runtime_status.prepared_pages == PREPARE_PAGE_BUDGET);
    assert(runtime_status.memory_region_failures == 1 && runtime_status.memory_regions_prepared == 1);

    reset_fixture();
    mapping_fails = true;
    new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, true, 0x1000, 4096);
    visit(0);
    assert(maps == 1 && runtime_status.memory_region_failures == 1);
    assert(!runtime_status.prepared_pages && !runtime_status.memory_regions_prepared);

    reset_fixture();
    uint8_t aml[32];
    size_t length = declaration(aml, names[0], integers[0], integers[1]);
    new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, false, 0, 0);
    extras[0].Extra.AmlStart = aml;
    extras[0].Extra.AmlLength = length;
    argument_status = AE_ERROR;
    visit(0);
    assert(argument_calls == 1 && interpreter_enters == 1 && interpreter_exits == 1);
    assert(!maps && runtime_status.memory_region_failures == 1 && !runtime_status.deferred_regions);
}

static void fixed_register_tests(void) {
    reset_fixture();
    AcpiGbl_FADT.Flags = ACPI_FADT_RESET_REGISTER | ACPI_FADT_HW_REDUCED;
    AcpiGbl_FADT.ResetRegister = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x1fff,8,0,0);
    AcpiGbl_FADT.SleepControl = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x2fff,12,1,2);
    AcpiGbl_FADT.SleepStatus = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x3000,8,0,0);
    prepare_fixed_registers();
    assert(runtime_status.reset_mapping_ready && runtime_status.sleep_mapping_ready);
    assert(!runtime_status.fixed_mapping_failures && runtime_status.prepared_pages == 4);
    assert(maps == 3 && mappings[1].address == 0x2fff && mappings[1].length == 2);

    for (unsigned failure = 1; failure <= 3; failure++) {
        reset_fixture();
        AcpiGbl_FADT.Flags = ACPI_FADT_RESET_REGISTER | ACPI_FADT_HW_REDUCED;
        AcpiGbl_FADT.ResetRegister = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x1000,8,0,0);
        AcpiGbl_FADT.SleepControl = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x2000,8,0,0);
        AcpiGbl_FADT.SleepStatus = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x3000,8,0,0);
        fail_map_call = failure;
        prepare_fixed_registers();
        assert(runtime_status.reset_mapping_ready == (failure != 1));
        assert(runtime_status.sleep_mapping_ready == (failure == 1));
        assert(runtime_status.fixed_mapping_failures == 1);
        assert(maps == 3 && runtime_status.prepared_pages == 2);
    }

    reset_fixture();
    /* Normalized extended GAS values are authoritative, even for old raw FADTs. */
    AcpiGbl_FADT.Header.Length = ACPI_FADT_V1_SIZE;
    AcpiGbl_FADT.Pm1aControlBlock = 0; /* Not the register used by preflight. */
    AcpiGbl_FADT.XPm1aControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x404,16,0,0);
    AcpiGbl_FADT.XPm1aEventBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x400,32,0,0);
    prepare_fixed_registers();
    assert(runtime_status.sleep_mapping_ready && !runtime_status.reset_mapping_ready);
    assert(!maps && !runtime_status.fixed_mapping_failures);

    reset_fixture();
    AcpiGbl_FADT.Flags = ACPI_FADT_RESET_REGISTER;
    AcpiGbl_FADT.ResetRegister = gas(ACPI_ADR_SPACE_SYSTEM_IO,0xcf9,8,0,0);
    AcpiGbl_FADT.XPm1aControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x1000,16,0,0);
    AcpiGbl_FADT.XPm1bControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x2000,16,0,0);
    AcpiGbl_FADT.XPm1aEventBlock = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x3000,32,0,0);
    AcpiGbl_FADT.XPm1bEventBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x500,32,0,0);
    AcpiGbl_FADT.XPm2ControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x4000,8,0,0);
    AcpiGbl_FADT.XPmTimerBlock = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x5000,32,0,0);
    prepare_fixed_registers();
    assert(runtime_status.reset_mapping_ready && runtime_status.sleep_mapping_ready);
    assert(maps == 5 && runtime_status.prepared_pages == 5 && !runtime_status.fixed_mapping_failures);

    reset_fixture();
    AcpiGbl_FADT.XPm1aControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x1000,16,0,0);
    AcpiGbl_FADT.XPm1aEventBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x400,32,0,0);
    AcpiGbl_FADT.XPm1bControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x2000,16,0,0);
    fail_map_call = 2;
    prepare_fixed_registers();
    assert(!runtime_status.sleep_mapping_ready && runtime_status.fixed_mapping_failures == 1);
    assert(maps == 2 && runtime_status.prepared_pages == 1);

    reset_fixture();
    AcpiGbl_FADT.XPm1aControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x404,16,0,0);
    AcpiGbl_FADT.XPm1aEventBlock = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x1000,32,0,0);
    mapping_fails = true;
    prepare_fixed_registers();
    assert(!runtime_status.sleep_mapping_ready && runtime_status.fixed_mapping_failures == 1);

    reset_fixture();
    AcpiGbl_FADT.XPm1aControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x404,16,0,0);
    prepare_fixed_registers(); /* Missing PM1a wake-status cannot claim S5 coverage. */
    assert(!runtime_status.sleep_mapping_ready && runtime_status.fixed_mapping_failures == 1);
    assert(!maps);

    for (unsigned optional = 0; optional < 3; optional++) {
        reset_fixture();
        AcpiGbl_FADT.XPm1aControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x404,16,0,0);
        AcpiGbl_FADT.XPm1aEventBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x400,32,0,0);
        ACPI_GENERIC_ADDRESS *registers[] = {&AcpiGbl_FADT.XPm1bEventBlock,
            &AcpiGbl_FADT.XPm2ControlBlock,&AcpiGbl_FADT.XPmTimerBlock};
        *registers[optional] = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x1000,8,0,0);
        mapping_fails = true;
        prepare_fixed_registers();
        assert(!runtime_status.sleep_mapping_ready && runtime_status.fixed_mapping_failures == 1);
        assert(maps == 1 && !runtime_status.prepared_pages);
    }

    reset_fixture();
    AcpiGbl_FADT.Flags = ACPI_FADT_RESET_REGISTER;
    AcpiGbl_FADT.ResetRegister = gas(ACPI_ADR_SPACE_SYSTEM_IO,UINT16_MAX,0,255,255);
    AcpiGbl_FADT.XPm1aControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x404,16,0,0);
    AcpiGbl_FADT.XPm1aEventBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x400,32,0,0);
    prepare_fixed_registers(); /* Upstream AcpiReset writes exactly one I/O byte. */
    assert(runtime_status.reset_mapping_ready && runtime_status.sleep_mapping_ready);
    assert(!runtime_status.fixed_mapping_failures && !maps);

    static const struct { unsigned space, width, offset, access; uint64_t address; } invalid[] = {
        {ACPI_ADR_SPACE_SYSTEM_MEMORY,8,0,0,0},
        {ACPI_ADR_SPACE_SYSTEM_MEMORY,0,0,0,0x1000},
        {ACPI_ADR_SPACE_SYSTEM_MEMORY,65,0,0,0x1000},
        {ACPI_ADR_SPACE_SYSTEM_MEMORY,8,64,0,0x1000},
        {ACPI_ADR_SPACE_SYSTEM_MEMORY,64,1,0,0x1000},
        {ACPI_ADR_SPACE_SYSTEM_MEMORY,8,0,5,0x1000},
        {ACPI_ADR_SPACE_PCI_CONFIG,8,0,0,0x1000},
        {ACPI_ADR_SPACE_SYSTEM_IO,8,0,0,0x10000},
        {ACPI_ADR_SPACE_SYSTEM_IO,16,0,0,UINT16_MAX},
        {ACPI_ADR_SPACE_SYSTEM_IO,32,0,0,UINT16_MAX},
        {ACPI_ADR_SPACE_SYSTEM_MEMORY,8,0,0,UINT64_C(0x0000800000000000)},
    };
    reset_fixture();
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        ACPI_GENERIC_ADDRESS value = gas(invalid[i].space,invalid[i].address,
                                         invalid[i].width,invalid[i].offset,invalid[i].access);
        assert(!prepare_gas(&value));
    }
    assert(!maps && !runtime_status.prepared_pages);
    ACPI_GENERIC_ADDRESS value = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x1ffd,20,1,2);
    assert(prepare_gas(&value) && mappings[0].length == 4);
    value = gas(ACPI_ADR_SPACE_SYSTEM_IO,UINT16_MAX,8,0,0);
    assert(prepare_gas(&value) && maps == 1);
    value = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x3000,8,0,4);
    assert(prepare_gas(&value) && mappings[1].length == 1); /* FADT format ignores AccessWidth. */

    reset_fixture();
    value = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x1ff9,40,0,3);
    assert(prepare_gas(&value) && maps == 1 && mappings[0].length == 8);
    assert(runtime_status.prepared_pages == 2); /* Two DWORDs, not five packed bytes. */

    reset_fixture();
    AcpiGbl_FADT.Flags = ACPI_FADT_RESET_REGISTER | ACPI_FADT_HW_REDUCED;
    prepare_fixed_registers();
    assert(!runtime_status.reset_mapping_ready && !runtime_status.sleep_mapping_ready);
    assert(runtime_status.fixed_mapping_failures == 3 && !maps);
}

static void configure_gpes(void) {
    block_count = 4;
    static const unsigned counts[] = {8,0,768,248};
    unsigned offset = 0;
    for (unsigned i = 0; i < block_count; i++) {
        blocks[i].XruptBlock = &interrupts[i % ARRAY_SIZE(interrupts)];
        blocks[i].GpeCount = counts[i];
        blocks[i].EventInfo = counts[i] ? events + offset : NULL;
        offset += counts[i];
    }
    assert(offset == 1024);
    /* Unbound, disabled, method, handler and notify entries are all configured. */
    for (unsigned i = 0; i < offset; i++) {
        events[i].Flags = i % 5;
        events[i].RuntimeCount = i % 2;
    }
}
static void gpe_tests(void) {
    reset_fixture();
    configure_gpes();
    assert(acpi_os_prepare_gpes());
    assert(gpes_prepared && gpe_walks == 2 && allocations == 1 && frees == 1);
    assert(reservation_calls == 1 && reserved_count == 1024 && !live_allocations);
    for (unsigned i = 0; i < 1024; i++)
        assert(reserved[i] == &events[i]);
    assert(acpi_os_prepare_gpes() && gpe_walks == 2 && reservation_calls == 1);

    reset_fixture();
    assert(acpi_os_prepare_gpes()); /* Empty configured topology needs no allocation. */
    assert(gpe_walks == 2 && reservation_calls == 1 && !reserved_count && !allocations);

    reset_fixture();
    ready = true;
    assert(!acpi_os_prepare_gpes() && !gpe_walks && !allocations && !reservation_calls);

    for (unsigned failure = 1; failure <= 2; failure++) {
        reset_fixture();
        configure_gpes();
        fail_gpe_walk = failure;
        assert(!acpi_os_prepare_gpes() && !gpes_prepared && !reservation_calls);
        assert(!live_allocations && allocations == failure - 1 && frees == failure - 1);
    }
    reset_fixture();
    configure_gpes();
    allocation_fails = true;
    assert(!acpi_os_prepare_gpes() && gpe_walks == 1 && allocations == 1);
    assert(!gpes_prepared && !reservation_calls && !live_allocations);
    allocation_fails = false;
    assert(acpi_os_prepare_gpes()); /* A failed pre-boot allocation can be retried. */

    reset_fixture();
    configure_gpes();
    reservation_fails = true;
    assert(!acpi_os_prepare_gpes() && !gpes_prepared && reservation_calls == 1);
    assert(allocations == 1 && frees == 1 && !live_allocations);

    reset_fixture();
    configure_gpes();
    blocks[0].EventInfo = NULL;
    assert(!acpi_os_prepare_gpes() && !allocations && !reservation_calls);

    for (int delta = -1; delta <= 1; delta += 2) {
        reset_fixture();
        configure_gpes();
        mutate_gpe_walk = 2;
        mutate_gpe_delta = delta;
        assert(!acpi_os_prepare_gpes() && !gpes_prepared && !reservation_calls);
        assert(allocations == 1 && frees == 1 && !live_allocations);
    }

    /* Overflow/capacity guards are feasible directly without giant fixtures. */
    reset_fixture();
    ACPI_GPE_BLOCK_INFO block = {.GpeCount = 2, .EventInfo = events};
    struct gpe_snapshot snapshot = {.count = SIZE_MAX - 1};
    assert(snapshot_gpes(NULL,&block,&snapshot) == AE_LIMIT);
    void *contexts[1] = {NULL};
    snapshot = (struct gpe_snapshot){.contexts = contexts,.capacity = 1};
    assert(snapshot_gpes(NULL,&block,&snapshot) == AE_LIMIT && !contexts[0]);
    block.GpeCount = 1;
    assert(snapshot_gpes(NULL,&block,&snapshot) == AE_OK && contexts[0] == &events[0]);
    block.EventInfo = NULL;
    snapshot = (struct gpe_snapshot){0};
    assert(snapshot_gpes(NULL,&block,&snapshot) == AE_BAD_PARAMETER);
}

static void public_runtime_tests(void) {
    reset_fixture();
    assert(!acpi_os_prepare_runtime() && !namespace_walks && !maps);
    assert(acpi_os_prepare_gpes());
    ready = true;
    assert(!acpi_os_prepare_runtime() && !namespace_walks && !maps);
    ready = false;
    AcpiGbl_FADT.XPm1aControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x404,16,0,0);
    AcpiGbl_FADT.XPm1aEventBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x400,32,0,0);
    new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, true, 0x1000, 4096);
    new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, true, 0x1000, 0); /* Optional bad object. */
    new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, false, 0, 0); /* Optional unused object. */
    new_region(ACPI_ADR_SPACE_SYSTEM_IO, false, 0, 0);
    assert(acpi_os_prepare_runtime());
    struct acpi_runtime_status measured;
    acpi_os_get_runtime_status(&measured);
    assert(measured.prepared && measured.sleep_mapping_ready && namespace_walks == 1);
    assert(measured.memory_regions == 3 && measured.memory_regions_prepared == 1);
    assert(measured.memory_region_failures == 1 && measured.deferred_regions == 1);
    assert(measured.prepared_pages == 1 && !measured.fixed_mapping_failures);
    measured.prepared_pages = 999;
    acpi_os_get_runtime_status(NULL);
    acpi_os_get_runtime_status(&measured);
    assert(measured.prepared_pages == 1); /* Copy-out cannot mutate status. */
    ready = true;
    assert(acpi_os_prepare_runtime() && namespace_walks == 1 && maps == 1);

    reset_fixture();
    assert(acpi_os_prepare_gpes());
    namespace_status = AE_ERROR;
    assert(!acpi_os_prepare_runtime() && !runtime_status.prepared && namespace_walks == 1);

    reset_fixture();
    assert(acpi_os_prepare_gpes());
    AcpiGbl_FADT.XPm1aControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x404,16,0,0);
    AcpiGbl_FADT.XPm1aEventBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x400,32,0,0);
    uint8_t aml[32];
    size_t length = declaration(aml, names[0], integers[0], integers[1]);
    new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, false, 0, 0);
    extras[0].Extra.AmlStart = aml;
    extras[0].Extra.AmlLength = length;
    argument_status = AE_ERROR;
    assert(acpi_os_prepare_runtime());
    assert(runtime_status.prepared && runtime_status.memory_region_failures == 1);
    assert(argument_calls == 1 && interpreter_enters == 1 && interpreter_exits == 1);
    assert(!maps && !interpreter_depth); /* Optional interpreter failure cannot abort boot. */

    reset_fixture();
    assert(acpi_os_prepare_gpes());
    mapping_fails = true;
    AcpiGbl_FADT.Flags = ACPI_FADT_RESET_REGISTER | ACPI_FADT_HW_REDUCED;
    AcpiGbl_FADT.ResetRegister = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x1000,8,0,0);
    AcpiGbl_FADT.SleepControl = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x2000,8,0,0);
    AcpiGbl_FADT.SleepStatus = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x3000,8,0,0);
    new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, true, 0x4000, 4096);
    assert(acpi_os_prepare_runtime()); /* Mapping failures do not fail optional boot. */
    assert(runtime_status.prepared && runtime_status.fixed_mapping_failures == 3);
    assert(runtime_status.memory_region_failures == 1 && !runtime_status.prepared_pages);
    assert(!runtime_status.reset_mapping_ready && !runtime_status.sleep_mapping_ready);

    reset_fixture();
    assert(acpi_os_prepare_gpes());
    AcpiGbl_FADT.XPm1aControlBlock = gas(ACPI_ADR_SPACE_SYSTEM_MEMORY,0x1000,16,0,0);
    AcpiGbl_FADT.XPm1aEventBlock = gas(ACPI_ADR_SPACE_SYSTEM_IO,0x400,32,0,0);
    for (unsigned i = 0; i < 4; i++)
        new_region(ACPI_ADR_SPACE_SYSTEM_MEMORY, true,0x10000000 + i * PREPARE_REGION_LIMIT,
                   PREPARE_REGION_LIMIT);
    assert(acpi_os_prepare_runtime());
    assert(mappings[0].address == 0x1000); /* Fixed power registers get budget priority. */
    assert(runtime_status.sleep_mapping_ready && runtime_status.memory_regions_prepared == 3);
    assert(runtime_status.memory_region_failures == 1 && runtime_status.prepared_pages == 49153);
}

int main(void) {
    literal_tests();
    reject_expression_tests();
    malformed_aml_tests();
    region_and_budget_tests();
    fixed_register_tests();
    gpe_tests();
    public_runtime_tests();
    assert(!live_allocations && !interpreter_depth && !inside_gpe_walk);
    printf("ACPICA runtime preflight: %u fixtures passed; literal-only AML, guarded bounds, "
           "optional regions, page budgets, normalized FADT readiness, "
           "1024-entry GPE snapshot/reservation and failure paths (ASan/UBSan)\n", fixture_count);
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="coast-acpi-runtime-") as temporary:
        folder = Path(temporary)
        include = folder / "include"
        include.mkdir()
        (include / "test_kernel.h").write_text(KERNEL)
        for name in ("mm/pmm.h", "printk.h", "sched.h"):
            header = include / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "test_kernel.h"\n')
        (include / "acpi").mkdir()
        (include / "acpi/osl.h").write_text((ROOT / "include/acpi/osl.h").read_text())
        source = folder / "test.c"
        source.write_text(SOURCE.replace("RUNTIME_SOURCE", str(ROOT / "drivers/acpi/runtime.c")))
        binary = folder / "test"
        compiler = shlex.split(os.environ.get("HOSTCC", "cc"))
        subprocess.run(compiler + ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                  "-D__COAST__", "-fsanitize=address,undefined",
                                  "-fno-omit-frame-pointer", "-I" + str(include),
                                  "-I" + str(ROOT / "lib/acpica/source/include"),
                                  str(source), "-o", str(binary)], check=True)
        environment = os.environ.copy()
        # LeakSanitizer can be incompatible with sandbox ptrace supervisors.
        # Explicit allocation accounting remains active, alongside ASan/UBSan.
        environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
        subprocess.run([str(binary)], check=True, timeout=30, env=environment)


if __name__ == "__main__":
    main()
