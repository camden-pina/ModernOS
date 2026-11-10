/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOOM_COAST_PROTOCOL_H
#define DOOM_COAST_PROTOCOL_H
#include <stdint.h>

/* Musl-facing declarations for the public Coast ABI. tools/test_doom_host.py
 * checks their sizes and operations against include/uapi/abi.h. */
enum { DOOM_SYS_DISPLAY = 402, DOOM_SYS_INPUT = 403, DOOM_SYS_AUDIO = 412 };
enum {
    DOOM_DISPLAY_INFO, DOOM_DISPLAY_ACQUIRE, DOOM_DISPLAY_PRESENT,
    DOOM_DISPLAY_RELEASE, DOOM_DISPLAY_PRESENT_BATCH, DOOM_DISPLAY_CAPS,
    DOOM_DISPLAY_PRESENT_SWAP
};
enum { DOOM_INPUT_KEY = 1, DOOM_INPUT_POINTER, DOOM_INPUT_OVERFLOW, DOOM_INPUT_HID_KEY };
#define DOOM_DISPLAY_MAX_RECTS 128
struct doom_display_info {
    uint32_t version, width, height, format, max_pixels, reserved[3];
};
struct doom_damage_rect { uint32_t x, y, width, height; };
struct doom_display_batch {
    uint64_t pixels;
    uint32_t stride, count;
    struct doom_damage_rect rects[DOOM_DISPLAY_MAX_RECTS];
};
struct doom_input_event {
    uint64_t sequence;
    uint32_t type, code;
    int32_t dx, dy, wheel;
    uint32_t buttons;
};

long Doom_OSCall(long number, long a, long b, long c);
int Doom_TraceEnabled(void);
void Doom_InputBeginTic(void);
int Doom_EnsureDirectory(const char *path);
void Doom_AudioStats(void);
void Doom_AudioShutdown(void);

#endif
