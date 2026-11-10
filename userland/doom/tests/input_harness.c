/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "coast_protocol.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "d_event.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static struct doom_input_event fixture[512];
static unsigned fixture_count, fixture_at, tick_count, downs, ups, mice;
static unsigned char held[256];

long Doom_TestSyscall(long number, ...) {
    assert(number == DOOM_SYS_INPUT);
    va_list args;
    va_start(args, number);
    struct doom_input_event *out = (void *)va_arg(args, long);
    unsigned capacity = (unsigned)va_arg(args, long);
    va_end(args);
    if (fixture_at == fixture_count) { errno = EAGAIN; return -1; }
    unsigned count = fixture_count - fixture_at;
    if (count > capacity) count = capacity;
    memcpy(out, fixture + fixture_at, count * sizeof(*out));
    fixture_at += count;
    return count;
}

void D_PostEvent(event_t *event) {
    tick_count++;
    assert(tick_count <= 32); /* Actual engine ring capacity is only 64. */
    if (event->type == ev_mouse) { mice++; return; }
    assert(event->data1 > 0 && event->data1 < 256);
    if (event->type == ev_keydown) { held[event->data1] = 1; downs++; }
    else { assert(event->type == ev_keyup); held[event->data1] = 0; ups++; }
}

void I_Quit(void) { abort(); }

static void run_tick(void) {
    tick_count = 0;
    Doom_InputBeginTic();
    int down;
    unsigned char key;
    while (DG_GetKey(&down, &key)) {
        event_t event = {.type = down ? ev_keydown : ev_keyup, .data1 = key};
        D_PostEvent(&event);
    }
}

static void key_event(unsigned usage, unsigned down) {
    assert(fixture_count < 512);
    fixture[fixture_count++] = (struct doom_input_event){.type = DOOM_INPUT_HID_KEY,
                                                       .code = usage, .buttons = down};
}

static void fresh(void) { fixture_at = fixture_count = 0; }

int main(void) {
    /* A burst much larger than the engine ring must retain every transition. */
    for (unsigned i = 0; i < 200; i++) {
        key_event(0x1a, !(i & 1));
        if (i % 3 == 0)
            fixture[fixture_count++] = (struct doom_input_event){.type = DOOM_INPUT_POINTER, .dx = 1};
    }
    for (unsigned tick = 0; tick < 20; tick++) run_tick();
    assert(downs == 100 && ups == 100 && mice > 0 && !held['w']);

    /* Review regression: 63 mouse packets followed by the held key's release. */
    fresh(); key_event(0x1a, 1); run_tick(); assert(held['w']);
    fresh();
    for (unsigned i = 0; i < 63; i++)
        fixture[fixture_count++] = (struct doom_input_event){.type = DOOM_INPUT_POINTER,
                                                           .dx = 1, .buttons = i & 1};
    key_event(0x1a, 0);
    for (unsigned tick = 0; tick < 4; tick++) run_tick();
    assert(!held['w']);

    /* Menu text/save descriptions retain all movement letters and Space. */
    fresh();
    const unsigned usages[] = {0x1a, 0x04, 0x16, 0x07, 0x08, 0x2c};
    const unsigned char expected[] = {'w', 'a', 's', 'd', 'e', ' '};
    for (unsigned i = 0; i < sizeof(usages) / sizeof(*usages); i++) {
        key_event(usages[i], 1); key_event(usages[i], 0);
    }
    Doom_InputBeginTic();
    for (unsigned i = 0; i < sizeof(expected); i++) {
        int down; unsigned char key;
        assert(DG_GetKey(&down, &key) && down && key == expected[i]);
        assert(DG_GetKey(&down, &key) && !down && key == expected[i]);
    }

    /* Left/right Ctrl share one action without an early release. */
    fresh();
    key_event(0xe0, 1); key_event(0xe4, 1); key_event(0xe0, 0); key_event(0xe4, 0);
    unsigned before_down = downs, before_up = ups;
    run_tick();
    assert(downs == before_down + 1 && ups == before_up + 1 && !held[KEY_FIRE]);

    fresh(); key_event(0x1a, 1); run_tick(); assert(held['w']);
    fresh();
    fixture[fixture_count++] = (struct doom_input_event){.type = DOOM_INPUT_OVERFLOW, .code = 1};
    run_tick(); assert(!held['w']);
    /* Fresh-parent startup and occupied-file errors use the real filesystem. */
    char directory[] = "/tmp/coast-doom-directories-XXXXXX";
    assert(mkdtemp(directory));
    char home[256], user[256], save[256], occupied[256], missing[256];
    snprintf(home, sizeof(home), "%s/home", directory);
    snprintf(user, sizeof(user), "%s/home/user", directory);
    snprintf(save, sizeof(save), "%s/home/user/.doom", directory);
    snprintf(occupied, sizeof(occupied), "%s/occupied", directory);
    snprintf(missing, sizeof(missing), "%s/missing/child", directory);
    assert(!Doom_EnsureDirectory(home) && !Doom_EnsureDirectory(user) && !Doom_EnsureDirectory(save));
    assert(!Doom_EnsureDirectory(home) && !Doom_EnsureDirectory(user) && !Doom_EnsureDirectory(save));
    int fd = open(occupied, O_CREAT | O_WRONLY | O_EXCL, 0600);
    assert(fd >= 0); close(fd);
    assert(Doom_EnsureDirectory(occupied) == -1 && errno == ENOTDIR);
    assert(Doom_EnsureDirectory(missing) == -1 && errno == ENOENT);
    assert(!unlink(occupied) && !rmdir(save) && !rmdir(user) && !rmdir(home) && !rmdir(directory));
    puts("Doom input burst/ASCII/alias/overflow and fresh save-directory host harness passed.");
    return 0;
}
