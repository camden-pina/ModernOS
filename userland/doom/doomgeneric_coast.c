/* SPDX-License-Identifier: GPL-2.0-or-later
 * Native Coast framebuffer, USB HID/mouse and monotonic-clock backend. */
#define _GNU_SOURCE
#include "coast_protocol.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "d_event.h"
#include "i_system.h"
#include "m_config.h"
#include "m_argv.h"
#include "m_controls.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "../../include/uapi/audio.h"
#include "../../include/uapi/window.h"

#define DEFAULT_IWAD "/usr/share/games/doom/freedoom2.wad"
#define DEFAULT_HOME "/home/user/.doom"
#define KEY_QUEUE 512
struct key_event { unsigned char key, down, mouse; int dx; unsigned buttons; };
static struct key_event keys[KEY_QUEUE];
static unsigned key_head, key_tail;
static unsigned input_budget;
static unsigned char physical[256], references[256];
static struct doom_display_info display;
static struct termios saved_termios;
static uint32_t *canvas;
static unsigned *source_x, *source_y;
static unsigned view_x, view_y, view_width, view_height;
static bool acquired, termios_saved, trace, request_quit;
static unsigned frame_count, frame_limit;
static bool windowed, force_window, force_console;
static unsigned window_id;
static uint64_t epoch_ms;

long Doom_OSCall(long number, long a, long b, long c) {
    return syscall(number, a, b, c);
}

int Doom_TraceEnabled(void) { return trace; }

static uint64_t clock_ms(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) {
        perror("doom: monotonic clock");
        exit(1);
    }
    return (uint64_t)time.tv_sec * 1000 + (unsigned long)time.tv_nsec / 1000000;
}

static void cleanup(void) {
    Doom_AudioShutdown();
    if (windowed && window_id) {
        struct os_window_token token = {window_id, 0};
        Doom_OSCall(OS_WINDOW_SYSCALL, OS_WINDOW_CLIENT_CLOSE, (long)&token, sizeof(token));
        window_id = 0;
    }
    if (acquired) {
        Doom_OSCall(DOOM_SYS_DISPLAY, DOOM_DISPLAY_RELEASE, 0, 0);
        acquired = false;
    }
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
        termios_saved = false;
    }
    free(canvas);
    free(source_x);
    free(source_y);
    canvas = NULL;
    if (frame_count)
        printf("DOOM_EXIT frames=%u; returned to %s\n", frame_count, windowed ? "desktop" : "console");
}

static void fatal(const char *what) {
    perror(what);
    exit(1);
}

void DG_Init(void) {
    /* Preserve ASCII for menu shortcuts/save descriptions; gameplay bindings
       are independent of the keyboard's text representation. */
    key_up = 'w'; key_down = 's'; key_strafeleft = 'a'; key_straferight = 'd'; key_use = 'e';
    if (!force_console) {
        struct os_window_info window = {.version=OS_WINDOW_VERSION,
            .width=DOOMGENERIC_RESX,.height=DOOMGENERIC_RESY};
        strcpy(window.title, "Doom - Freedoom");
        if (!Doom_OSCall(OS_WINDOW_SYSCALL, OS_WINDOW_CREATE, (long)&window, sizeof(window))) {
            windowed = true;
            window_id = window.id;
            epoch_ms = clock_ms();
            printf("DOOM_READY window=%u engine=320x200 aspect=4:3 software compositor\n",window_id);
            puts("Click game for pointer capture; F10 releases; Alt+F4 closes. WASD moves; Escape menu.");
            fflush(stdout);
            return;
        }
        if (force_window || (errno != ENODEV && errno != ENOSYS && errno != ENOTSUP))
            fatal("doom: create desktop window");
    }
    if (Doom_OSCall(DOOM_SYS_DISPLAY, DOOM_DISPLAY_INFO, (long)&display, sizeof(display)))
        fatal("doom: display info");
    if (display.version < 2 || display.format != 1 || display.width < 320 ||
        display.height < 240 || display.width > 8192 ||
        (uint64_t)display.width * display.height * 4 > 64 * 1024 * 1024) {
        fputs("doom: display ABI 2+, XRGB8888, and a 320x240 or larger display required\n", stderr);
        exit(1);
    }
    canvas = calloc((size_t)display.width * display.height, sizeof(*canvas));
    source_x = malloc(display.width * sizeof(*source_x));
    source_y = malloc(display.height * sizeof(*source_y));
    if (!canvas || !source_x || !source_y)
        fatal("doom: screen allocation");
    /* Classic Doom's 320x200 pixels were shown with a 4:3 display aspect. */
    view_width = display.width;
    view_height = view_width * 3 / 4;
    if (view_height > display.height) {
        view_height = display.height;
        view_width = view_height * 4 / 3;
    }
    view_x = (display.width - view_width) / 2;
    view_y = (display.height - view_height) / 2;
    for (unsigned x = 0; x < view_width; x++)
        source_x[x] = x * DOOMGENERIC_RESX / view_width;
    for (unsigned y = 0; y < view_height; y++)
        source_y[y] = y * DOOMGENERIC_RESY / view_height;

    if (tcgetattr(STDIN_FILENO, &saved_termios))
        fatal("doom: console termios (launch from the Coast console)");
    termios_saved = true;
    struct termios raw = saved_termios;
    raw.c_iflag = raw.c_oflag = raw.c_lflag = 0;
    raw.c_cc[VMIN] = raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw))
        fatal("doom: raw console");
    if (Doom_OSCall(DOOM_SYS_DISPLAY, DOOM_DISPLAY_ACQUIRE, 0, 0))
        fatal("doom: acquire display (return from the desktop to the console first)");
    acquired = true;
    epoch_ms = clock_ms();
    printf("DOOM_READY display=%ux%u viewport=%ux%u engine=320x200 aspect=4:3\n",
           display.width, display.height, view_width, view_height);
    puts("Doom controls: WASD move/strafe; arrows turn/move; Ctrl or mouse1 fire; E/Space use; Shift run; Escape menu; Ctrl+Alt+Escape quit.");
    fflush(stdout);
}

static uint64_t frame_hash(void) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; i++) {
        hash ^= DG_ScreenBuffer[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void DG_DrawFrame(void) {
    if (windowed) {
        struct os_window_frame frame = {.id=window_id,.width=DOOMGENERIC_RESX,
            .height=DOOMGENERIC_RESY,.stride=DOOMGENERIC_RESX,.pixels=(uintptr_t)DG_ScreenBuffer};
        if (Doom_OSCall(OS_WINDOW_SYSCALL,OS_WINDOW_PRESENT,(long)&frame,sizeof(frame))) {
            if (errno==EPIPE || errno==ENOENT || errno==ENODEV) {request_quit=true;return;}
            fatal("doom: present window frame");
        }
    } else {
    for (unsigned y = 0; y < view_height; y++) {
        uint32_t *out = canvas + (size_t)(view_y + y) * display.width + view_x;
        const pixel_t *in = DG_ScreenBuffer + source_y[y] * DOOMGENERIC_RESX;
        for (unsigned x = 0; x < view_width; x++)
            out[x] = in[source_x[x]];
    }
    struct doom_display_batch batch = {
        .pixels = (uintptr_t)canvas, .stride = display.width, .count = 1,
        .rects = {{0, 0, display.width, display.height}}
    };
    if (Doom_OSCall(DOOM_SYS_DISPLAY,
                   display.version >= 3 ? DOOM_DISPLAY_PRESENT_SWAP : DOOM_DISPLAY_PRESENT_BATCH,
                   (long)&batch, sizeof(batch)))
        fatal("doom: present frame");
    }
    frame_count++;
    if (trace && (frame_count == 1 || frame_count % 35 == 0)) {
        const player_t *player = &players[consoleplayer];
        printf("DOOM_FRAME frame=%u hash=%016" PRIx64 " state=%d menu=%d map=%d tic=%d x=%d y=%d angle=%u ammo=%d forward=%d side=%d momx=%d momy=%d\n",
               frame_count, frame_hash(), gamestate, menuactive, gamemap, leveltime,
               player->mo ? player->mo->x : 0, player->mo ? player->mo->y : 0,
               player->mo ? player->mo->angle : 0, player->ammo[0],
               player->cmd.forwardmove, player->cmd.sidemove,
               player->mo ? player->mo->momx : 0, player->mo ? player->mo->momy : 0);
        Doom_AudioStats();
        fflush(stdout);
    }
    if (frame_limit && frame_count >= frame_limit)
        request_quit = true;
}

uint32_t DG_GetTicksMs(void) { return (uint32_t)(clock_ms() - epoch_ms); }

void DG_SleepMs(uint32_t ms) {
    struct timespec delay = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}

void DG_SetWindowTitle(const char *title) {
    printf("DOOM_TITLE %s\n", title);
    fflush(stdout);
}

static void enqueue(unsigned key, bool down) {
    unsigned next = (key_head + 1) % KEY_QUEUE;
    if (next == key_tail) {
        fputs("doom: local input queue overflow\n", stderr);
        exit(1);
    }
    keys[key_head] = (struct key_event){.key = (unsigned char)key, .down = down};
    key_head = next;
}

static unsigned ascii_key(unsigned c) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    switch (c) {
    case 10: return KEY_ENTER;
    case 8: case 127: return KEY_BACKSPACE;
    default: return c < 128 ? c : 0;
    }
}

static unsigned hid_key(unsigned usage) {
    if (usage >= 0x04 && usage <= 0x1d) return ascii_key('a' + usage - 0x04);
    if (usage >= 0x1e && usage <= 0x26) return '1' + usage - 0x1e;
    if (usage >= 0x3a && usage <= 0x43) return KEY_F1 + usage - 0x3a;
    switch (usage) {
    case 0x27: return '0';
    case 0x28: case 0x58: return KEY_ENTER;
    case 0x29: return KEY_ESCAPE;
    case 0x2a: return KEY_BACKSPACE;
    case 0x2b: return KEY_TAB;
    case 0x2c: return ' ';
    case 0x2d: return '-';
    case 0x2e: return '=';
    case 0x2f: return '[';
    case 0x30: return ']';
    case 0x31: return '\\';
    case 0x33: return ';';
    case 0x34: return '\'';
    case 0x35: return '`';
    case 0x36: return ',';
    case 0x37: return '.';
    case 0x38: return '/';
    case 0x44: return KEY_F11;
    case 0x45: return KEY_F12;
    case 0x48: return KEY_PAUSE;
    case 0x4f: return KEY_RIGHTARROW;
    case 0x50: return KEY_LEFTARROW;
    case 0x51: return KEY_DOWNARROW;
    case 0x52: return KEY_UPARROW;
    case 0xe0: case 0xe4: return KEY_FIRE;
    case 0xe1: case 0xe5: return KEY_RSHIFT;
    case 0xe2: case 0xe6: return KEY_RALT;
    default: return 0;
    }
}

static int clamp_motion(int value) {
    return value < -2048 ? -2048 : value > 2048 ? 2048 : value;
}

static void enqueue_mouse(int dx, unsigned buttons) {
    unsigned last = (key_head + KEY_QUEUE - 1) % KEY_QUEUE;
    if (key_head != key_tail && keys[last].mouse && keys[last].buttons == buttons) {
        keys[last].dx = clamp_motion(keys[last].dx + clamp_motion(dx));
        return;
    }
    unsigned next = (key_head + 1) % KEY_QUEUE;
    if (next == key_tail) {
        fputs("doom: local input queue overflow\n", stderr);
        exit(1);
    }
    keys[key_head] = (struct key_event){.mouse = 1, .dx = clamp_motion(dx), .buttons = buttons};
    key_head = next;
}

static void release_keys(void) {
    for (unsigned key = 0; key < 256; key++)
        if (references[key]) enqueue(key, false);
    memset(physical, 0, sizeof(physical));
    memset(references, 0, sizeof(references));
    enqueue_mouse(0, 0);
}

static void input_poll(void) {
    struct doom_input_event events[64];
    /* A private queue retains bursts; the engine's own ring is only 64 slots. */
    for (unsigned batch = 0; batch < 2; batch++) {
        struct os_window_events request = {window_id,64,(uintptr_t)events};
        long count = windowed ? Doom_OSCall(OS_WINDOW_SYSCALL,OS_WINDOW_INPUT,(long)&request,sizeof(request))
                              : Doom_OSCall(DOOM_SYS_INPUT, (long)events, 64, 0);
        if (count < 0) {
            if (errno == EAGAIN) return;
            if (windowed && (errno==EPIPE || errno==ENOENT || errno==ENODEV)) {request_quit=true;release_keys();return;}
            fatal("doom: input");
        }
        for (long i = 0; i < count; i++) {
            struct doom_input_event *event = &events[i];
            if (windowed && event->type==OS_WINDOW_CLOSE) {request_quit=true;release_keys();}
            else if (windowed && event->type==OS_WINDOW_FOCUS) {
                if (!event->code) release_keys();
                if (trace) printf("DOOM_INPUT focus=%u released-held-keys\n",event->code);
            } else if (event->type == DOOM_INPUT_OVERFLOW) {
                release_keys();
                if (trace) printf("DOOM_INPUT overflow=%u released-held-keys\n", event->code);
            } else if (event->type == DOOM_INPUT_HID_KEY && event->code < 256) {
                unsigned usage = event->code, key = hid_key(usage);
                bool down = event->buttons != 0;
                if (down && usage == 0x29 && (event->dy & 0x11) && (event->dy & 0x44)) {
                    request_quit = true;
                    continue;
                }
                if (!key || physical[usage] == down) continue;
                physical[usage] = down;
                if (down) {
                    if (!references[key]++) enqueue(key, true);
                } else if (references[key] && !--references[key]) enqueue(key, false);
                if (trace) printf("DOOM_INPUT hid=%u down=%u key=%u\n", usage, down, key);
            } else if (event->type == DOOM_INPUT_KEY && !event->buttons) {
                unsigned key = ascii_key(event->code);
                if (key) { enqueue(key, true); enqueue(key, false); }
            } else if (event->type == DOOM_INPUT_POINTER) {
                /* Mouse Y never moves the player: WASD supplies deliberate motion. */
                enqueue_mouse(event->dx, event->buttons & 7);
                if (trace && (event->dx || event->buttons || event->code))
                    printf("DOOM_INPUT mouse dx=%d buttons=%u\n", event->dx, event->buttons);
            }
        }
        if (count < 64) return;
    }
}

void Doom_InputBeginTic(void) { input_budget = 32; }

int DG_GetKey(int *pressed, unsigned char *key) {
    while (input_budget) {
        if (key_tail == key_head) input_poll();
        if (request_quit) { I_Quit(); exit(0); }
        if (key_tail == key_head) return 0;
        struct key_event input = keys[key_tail];
        key_tail = (key_tail + 1) % KEY_QUEUE;
        input_budget--;
        if (input.mouse) {
            event_t mouse = {.type = ev_mouse, .data1 = input.buttons, .data2 = input.dx * 8};
            D_PostEvent(&mouse);
            continue;
        }
        *pressed = input.down;
        *key = input.key;
        return 1;
    }
    return 0;
}

int Doom_EnsureDirectory(const char *path) {
    if (!mkdir(path, 0755)) return 0;
    if (errno != EEXIST) return -1;
    struct stat status;
    if (stat(path, &status)) return -1;
    if (!S_ISDIR(status.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}

static void usage(void) {
    puts("Usage: doom [--info] [--trace] [--windowed|--console] [--frames N] [engine options]");
    puts("Default game: Freedoom Phase 2. Example: doom -warp 1 -skill 2");
    puts("WASD move/strafe; arrows turn/move; mouse turns; Ctrl/mouse1 fire;");
    puts("E/Space use; Shift run; Tab map; 1-7 weapons; Escape menu; Ctrl+Alt+Escape quit.");
    puts("Desktop Terminal/launcher: managed software window; click to capture pointer, F10 release, Alt+F4 close. --console retains exclusive display mode.");
    puts("Save/config files: /home/user/.doom (RAM until copied to a persistent mount).");
    puts("Sound effects and procedural MUS/MIDI music use native 48kHz stereo PCM; multiplayer is unavailable.");
}

static int info(void) {
    struct doom_display_info screen;
    struct os_audio_info pcm;
    puts("Doomgeneric e257a09527062bfb986b6e5e0780839b6cbd5b71; GPL-2.0-or-later");
    puts("Default data: Freedoom 0.13.0 Phase 2; BSD-3-Clause");
    printf("IWAD: %s (%s)\n", DEFAULT_IWAD, access(DEFAULT_IWAD, R_OK) ? "missing" : "available");
    if (!Doom_OSCall(DOOM_SYS_DISPLAY, DOOM_DISPLAY_INFO, (long)&screen, sizeof(screen)))
        printf("Display: ABI %u, %ux%u\n", screen.version, screen.width, screen.height);
    else perror("doom: display info");
    if (!Doom_OSCall(DOOM_SYS_AUDIO, OS_AUDIO_INFO, (long)&pcm, 0))
        printf("PCM: %uHz %uch %ubit queued=%" PRIu64 " played=%" PRIu64 "\n",
               pcm.rate, pcm.channels, pcm.sample_bits, pcm.queued_frames, pcm.played_frames);
    else perror("doom: audio info");
    usage();
    return access(DEFAULT_IWAD, R_OK) ? 1 : 0;
}

int main(int argc, char **argv) {
    char **args = calloc((size_t)argc + 10, sizeof(*args));
    if (!args) fatal("doom: argument allocation");
    int count = 0;
    args[count++] = argv[0];
    bool have_iwad = false, have_config = false, have_extra = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        if (!strcmp(argv[i], "--info")) return info();
        if (!strcmp(argv[i], "--windowed")) {force_window=true;continue;}
        if (!strcmp(argv[i], "--console")) {force_console=true;continue;}
        if (!strcmp(argv[i], "--trace")) { trace = true; continue; }
        if (!strcmp(argv[i], "--frames")) {
            char *end = "";
            unsigned long value = i + 1 < argc ? strtoul(argv[++i], &end, 10) : 0;
            if (!value || value > 1000000 || *end) {
                fputs("doom: --frames needs an integer from 1 to 1000000\n", stderr);
                return 2;
            }
            frame_limit = (unsigned)value;
            continue;
        }
        if (!strcasecmp(argv[i], "-scaling") || !strcasecmp(argv[i], "-gfxmode") || argv[i][0] == '@') {
            fputs("doom: scaling, pixel format and response files are fixed by the native backend\n", stderr);
            return 2;
        }
        if (!strcasecmp(argv[i], "-iwad")) have_iwad = true;
        if (!strcasecmp(argv[i], "-config")) have_config = true;
        if (!strcasecmp(argv[i], "-extraconfig")) have_extra = true;
        args[count++] = argv[i];
    }
    if (force_window && force_console) {fputs("doom: choose --windowed or --console\n",stderr);return 2;}
    if (!have_iwad) {
        if (access(DEFAULT_IWAD, R_OK)) fatal("doom: packaged Freedoom IWAD missing");
        args[count++] = "-iwad";
        args[count++] = DEFAULT_IWAD;
    }
    const char *save_dirs[] = {"/home", "/home/user", DEFAULT_HOME};
    for (unsigned i = 0; i < sizeof(save_dirs) / sizeof(*save_dirs); i++) {
        if (Doom_EnsureDirectory(save_dirs[i])) {
            fprintf(stderr, "doom: save directory %s: %s\n", save_dirs[i], strerror(errno));
            return 1;
        }
    }
    if (!have_config) { args[count++] = "-config"; args[count++] = DEFAULT_HOME "/default.cfg"; }
    if (!have_extra) { args[count++] = "-extraconfig"; args[count++] = DEFAULT_HOME "/coast.cfg"; }
    atexit(cleanup);
    doomgeneric_Create(count, args);
    while (!request_quit) doomgeneric_Tick();
    I_Quit();
    return 0;
}
