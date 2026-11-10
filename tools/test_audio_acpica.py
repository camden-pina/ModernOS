#!/usr/bin/env python3
"""Test the real AW88399 ACPICA consumer with synthetic firmware and mocked OSL.

Uses the vendored ACPICA public headers and compiles the actual driver. No
firmware installation, guest, physical MMIO or audible verification is involved.
"""
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def serial_bus(address):
    source = b"\\_SB.PC02.I2C2\0"
    data = bytearray(18 + len(source))
    data[0] = 0x8e
    struct.pack_into("<H", data, 1, len(data) - 3)
    data[3], data[5] = 2, 1
    struct.pack_into("<H", data, 10, 6)
    struct.pack_into("<IH", data, 12, 400000, address)
    data[18:] = source
    return data


def gpio_resource(pin, output):
    source = b"\\_SB.PGPI\0"
    data = bytearray(25 + len(source))
    data[0] = 0x8c
    struct.pack_into("<H", data, 1, len(data) - 3)
    data[3], data[4], data[5], data[7], data[9] = 1, int(output), 1, 1, 1
    struct.pack_into("<H", data, 14, 23)
    struct.pack_into("<H", data, 17, 25)
    struct.pack_into("<H", data, 19, len(data))
    struct.pack_into("<H", data, 23, pin)
    data[25:] = source
    return data


def memory_resource(base):
    data = bytearray(46)
    data[0] = 0x8a
    struct.pack_into("<H", data, 1, 43)
    data[4] = 0x0d
    struct.pack_into("<QQ", data, 14, base, base + 65535)
    struct.pack_into("<Q", data, 38, 65536)
    return data


SOURCE = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <acpica.h>
#include <drivers/aw88399_power.h>

static unsigned char speaker_data[] = {SPEAKER_BYTES};
static unsigned char gpio_data[] = {GPIO_BYTES};
static uint32_t registers[65536 / 4];
static int speaker_handle, gpio_handle;
static unsigned handles, evaluations, allocations, frees, live, maps, ticks, warnings;
static unsigned levels[9];
static unsigned missing_handle, fail_eval, fault, resource_fault, map_fault, gpio_fault;
enum { FAIL_NO_OBJECT = 1, FAIL_WITH_OBJECT, WRONG_TYPE, NO_OBJECT, SHORT_OBJECT,
       NULL_DATA, WRONG_STRING, SHORT_STRING, LONG_STRING };

ACPI_STATUS AcpiGetHandle(ACPI_HANDLE parent, const char *path, ACPI_HANDLE *out) {
    assert(!parent && out);
    handles++;
    if (handles == 1) {
        assert(!strcmp(path, "\\_SB.PC02.I2C2.SPKR"));
        *out = &speaker_handle;
    } else {
        assert(handles == 2 && !strcmp(path, "\\_SB.PGPI"));
        *out = &gpio_handle;
    }
    return handles == missing_handle ? AE_NOT_FOUND : AE_OK;
}

ACPI_STATUS AcpiEvaluateObject(ACPI_HANDLE handle, ACPI_STRING name,
                              ACPI_OBJECT_LIST *args, ACPI_BUFFER *result) {
    assert(!args && result && result->Length == ACPI_ALLOCATE_BUFFER && !result->Pointer);
    unsigned at = ++evaluations;
    const void *data;
    size_t size;
    ACPI_OBJECT_TYPE type;
    assert(at <= 5 && handle == (at == 3 || at == 5 ? &gpio_handle : &speaker_handle));
    assert(!strcmp(name, at == 2 ? "_SUB" : at >= 4 ? "_CRS" : "_HID"));
    if (at < 4) {
        data = at == 1 ? "AWDZ8399" : at == 2 ? "17AA3907" : "INTC1084";
        size = strlen(data);
        type = ACPI_TYPE_STRING;
    } else {
        data = at == 4 ? speaker_data : gpio_data;
        size = at == 4 ? sizeof(speaker_data) : sizeof(gpio_data);
        type = ACPI_TYPE_BUFFER;
    }
    if (at == fail_eval && (fault == FAIL_NO_OBJECT || fault == NO_OBJECT))
        return fault == FAIL_NO_OBJECT ? AE_ERROR : AE_OK;
    ACPI_OBJECT *obj = calloc(1, sizeof(*obj) + size + 1);
    assert(obj);
    allocations++;
    live++;
    unsigned char *bytes = (unsigned char *)(obj + 1);
    memcpy(bytes, data, size);
    obj->Type = type;
    if (type == ACPI_TYPE_STRING) {
        obj->String.Length = size;
        obj->String.Pointer = (char *)bytes;
    } else {
        obj->Buffer.Length = size;
        obj->Buffer.Pointer = bytes;
        if ((at == 4 && resource_fault == 1) || (at == 5 && resource_fault == 2))
            bytes[0] = 0;
        if (at == 4 && resource_fault == 3)
            bytes[RESET_OFFSET] = 171;
        if (at == 4 && resource_fault == 4)
            bytes[PULL_OFFSET] = 2;
    }
    result->Pointer = obj;
    result->Length = sizeof(*obj) + size + 1;
    if (at == fail_eval) {
        if (fault == FAIL_WITH_OBJECT)
            return AE_ERROR;
        if (fault == WRONG_TYPE)
            obj->Type = ACPI_TYPE_INTEGER;
        if (fault == SHORT_OBJECT)
            result->Length = sizeof(*obj) - 1;
        if (fault == NULL_DATA) {
            if (type == ACPI_TYPE_STRING)
                obj->String.Pointer = NULL;
            else
                obj->Buffer.Pointer = NULL;
        }
        if (fault == WRONG_STRING)
            bytes[0] ^= 1;
        if (fault == SHORT_STRING)
            obj->String.Length--;
        if (fault == LONG_STRING)
            obj->String.Length++;
    }
    return AE_OK;
}

void AcpiOsFree(void *ptr) {
    assert(ptr && live);
    live--;
    frees++;
    free(ptr);
}

void *vmem_map_mmio(uint64_t physical, size_t length) {
    assert(physical == 0x10010000 && length == sizeof(registers));
    maps++;
    return map_fault ? NULL : registers;
}

uint64_t tsc_uptime_us(void) {
    assert(ticks < 9);
    levels[ticks] = registers[0x6c0 / 4] & 1;
    return 500ULL * ++ticks;
}

int printk(const char *fmt, ...) {
    assert(fmt);
    if (strstr(fmt, "remain muted"))
        warnings++;
    return 0;
}

static void run(bool expected, unsigned expected_maps) {
    handles = evaluations = allocations = frees = live = maps = ticks = warnings = 0;
    memset(registers, 0, sizeof(registers));
    registers[0] = 0x940000;
    registers[0xc / 4] = 0x600;
    registers[0x6c0 / 4] = 0x81000400;
    registers[0x6c4 / 4] = 0x80000001;
    if (gpio_fault == 1)
        registers[0xb4 / 4] = 1U << 16; /* Pad is not host-owned. */
    if (gpio_fault == 2)
        registers[0x114 / 4] = 1U << 12; /* TX locked. */
    if (gpio_fault == 3)
        registers[0x110 / 4] = 1U << 12; /* Locked in non-GPIO mode. */
    assert(aw88399_power_init() == expected);
    assert(!live && allocations == frees && maps == expected_maps);
    assert(warnings == !expected);
    if (expected) {
        assert(handles == 2 && evaluations == 5 && allocations == 5 && ticks == 9);
        assert(levels[0] == 1 && levels[3] == 0 && levels[6] == 1);
        assert((registers[0x6c0 / 4] & 0x3f01) == 0x201);
        assert((registers[0x6c0 / 4] & 0x81000000) == 0x81000000);
        assert((registers[0x6c4 / 4] & 0x3c00) == 0x3000);
        assert((registers[0x6c4 / 4] & 0x80000001) == 0x80000001);
    } else {
        assert(!ticks);
        assert(registers[0x6c0 / 4] == 0x81000400);
        assert(registers[0x6c4 / 4] == 0x80000001);
    }
}

int main(void) {
    run(true, 1);
    for (missing_handle = 1; missing_handle <= 2; missing_handle++)
        run(false, 0);
    missing_handle = 0;
    for (fail_eval = 1; fail_eval <= 5; fail_eval++) {
        for (fault = FAIL_NO_OBJECT; fault <= NULL_DATA; fault++)
            run(false, 0);
        if (fail_eval <= 3)
            for (fault = WRONG_STRING; fault <= LONG_STRING; fault++)
                run(false, 0);
    }
    fail_eval = fault = 0;
    for (resource_fault = 1; resource_fault <= 4; resource_fault++)
        run(false, 0);
    resource_fault = 0;
    map_fault = 1;
    run(false, 1);
    map_fault = 0;
    for (gpio_fault = 1; gpio_fault <= 3; gpio_fault++)
        run(false, 1);
    gpio_fault = 0;
    run(true, 1);
    puts("PASS AW88399 ACPICA consumer: fixed device identities, raw _CRS parsing, reset, all evaluation/type/data failures, partial allocation cleanup and GPIO safety gates; no physical hardware claim");
}
'''


def main():
    buses = serial_bus(0x34) + serial_bus(0x35)
    speaker = buses + gpio_resource(140, True) + gpio_resource(171, False) + b"\x79\0"
    gpio = b"".join(memory_resource(0x10000000 + i * 65536) for i in range(5)) + b"\x79\0"
    source = SOURCE.replace("SPEAKER_BYTES", ",".join(map(str, speaker)))
    source = source.replace("GPIO_BYTES", ",".join(map(str, gpio)))
    source = source.replace("RESET_OFFSET", str(len(buses) + 23))
    source = source.replace("PULL_OFFSET", str(len(buses) + 9))
    with tempfile.TemporaryDirectory(prefix="coast-audio-acpica-") as temp:
        path = Path(temp)
        (path / "check.c").write_text(source)
        subprocess.run([
            "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-D__COAST__",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-idirafter", str(ROOT / "include"),
            "-idirafter", str(ROOT / "arch/x86/include/asm"),
            "-I", str(ROOT / "lib/acpica/source/include"),
            str(path / "check.c"), str(ROOT / "drivers/audio/aw88399_power.c"),
            "-o", str(path / "check"),
        ], check=True)
        subprocess.run([str(path / "check")], check=True)


if __name__ == "__main__":
    main()
