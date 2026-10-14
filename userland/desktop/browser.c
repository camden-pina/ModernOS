#include "desktop.h"
#include <browser_protocol.h>

#define INPUT_QUEUE 64

typedef struct {
    struct browser_packet packet;
    char text[BROWSER_MAX_TEXT];
} BrowserInput;

typedef struct {
    int pid, input, output, errors;
    uint32_t *canvas;
    struct browser_paint paint;
    int width, height, wanted_width, wanted_height;
    struct browser_packet geometry;
    struct browser_packet packet;
    size_t header_read, payload_read, payload_capacity;
    uint8_t *payload;
    BrowserInput queue[INPUT_QUEUE];
    unsigned first, queued;
    size_t sent;
    int pointer_x, pointer_y;
    int pointer_screen_x, pointer_screen_y;
    unsigned pointer_buttons, modifiers;
    unsigned navigation;
    bool address_focus, replace_address;
    int cursor;
    bool ended, painted, failed, input_error, closing;
    bool web_engine, paint_started;
    uint64_t close_at;
    char url[BROWSER_MAX_TEXT + 1], address[BROWSER_MAX_TEXT + 1], error_line[512];
    unsigned error_used;
} Browser;

static Rect address_box(Window *w) {
    return (Rect){w->box.x + S(196), w->box.y + S(7), MAX(1, w->box.w - S(289)), S(26)};
}

static void toolbar_dirty(Window *w) {
    if (w->workspace == desk.workspace && !w->minimized) {
        invalidate((Rect){w->box.x, w->box.y, w->box.w, S(40)});
    } else {
        w->surface_damage = (Rect){0, 0, w->box.w, w->box.h};
    }
}

static void focus_address(Window *w) {
    Browser *b = w->state;
    b->address_focus = true;
    b->replace_address = true;
    copy_text(b->address, sizeof(b->address), b->url);
    b->cursor = strlen(b->address);
    toolbar_dirty(w);
}

void browser_toolbar(Window *w) {
    Browser *b = w->state;
    fill((Rect){w->box.x + S(102), w->box.y + S(10), 1, S(19)}, theme_color(0xDFE5E4));
    for (int i = 0; i < 3; i++) {
        Rect r = {w->box.x + S(112 + 25 * i), w->box.y + S(8), S(24), S(25)};
        bool enabled =
            !b->ended && (i == 2 || (b->navigation & (i ? BROWSER_CAN_FORWARD : BROWSER_CAN_BACK)));
        if (enabled && contains(r, desk.mouse_x, desk.mouse_y)) {
            rounded(r, S(4), theme_color(0xEAF0ED));
        }
        Icon kind = i == 0                              ? I_BACK
                    : i == 1                            ? I_FORWARD
                    : (b->navigation & BROWSER_LOADING) ? I_CLOSE
                                                        : I_RELOAD;
        icon(r.x + S(5), r.y + S(6), kind, S(14),
             enabled ? THEME_SECONDARY_TEXT : THEME_MUTED_TEXT);
    }

    input_draw(address_box(w), b->address_focus ? b->address : b->url, "Enter an address",
               b->address_focus ? (b->replace_address ? -1 : b->cursor) : 0,
               b->address_focus && wm_focused() == w);
}

Rect browser_toolbar_hover(Window *w, int x, int y) {
    for (int i = 0; i < 3; i++) {
        Rect r = {w->box.x + S(112 + 25 * i), w->box.y + S(8), S(24), S(25)};
        if (contains(r, x, y)) {
            return r;
        }
    }

    return (Rect){0};
}

static void browser_failed(Window *w, const char *message) {
    Browser *b = w->state;
    if (!b->failed) {
        b->failed = true;
        trace("BROWSER_ERROR", w, message);
        dialog_notice(w, "Browser", message);
    }
}

static void flush_input(Window *w) {
    Browser *b = w->state;
    while (b->queued && !b->ended) {
        BrowserInput *event = &b->queue[b->first];
        size_t bytes = sizeof(event->packet) + event->packet.bytes;
        long count = write(b->input, (uint8_t *)event + b->sent, bytes - b->sent);
        if (count == -E_AGAIN || count == -E_INTR) {
            return;
        }
        if (count <= 0) {
            b->ended = true;
            browser_failed(w, "The browser process stopped accepting input.");
            return;
        }
        b->sent += count;
        if (b->sent == bytes) {
            b->sent = 0;
            b->first = (b->first + 1) % INPUT_QUEUE;
            b->queued--;
        }
    }
}

static bool send_event(Window *w, struct browser_packet packet, const char *text) {
    Browser *b = w->state;
    if (b->ended) {
        return false;
    }

    packet.magic = BROWSER_MAGIC;
    packet.bytes = text ? MIN(strlen(text), (size_t)BROWSER_MAX_TEXT) : 0;
    flush_input(w);

    /* Pure motion can replace unsent pure motion; button/key transitions stay
       in order and are never silently discarded to improve frame rate. */
    if (packet.type == BROWSER_POINTER && !packet.width && !packet.height && b->queued) {
        unsigned last = (b->first + b->queued - 1) % INPUT_QUEUE;
        struct browser_packet *previous = &b->queue[last].packet;
        if (!(last == b->first && b->sent) && previous->type == BROWSER_POINTER &&
            !previous->width && !previous->height && previous->flags == packet.flags) {
            *previous = packet;
            return true;
        }
    }

    if (b->queued == INPUT_QUEUE) {
        if (!b->input_error) {
            b->input_error = true;
            dialog_notice(w, "Browser input",
                          "The browser is busy and its input queue is full. Let it finish loading "
                          "before continuing.");
        }

        return false;
    }

    BrowserInput *event = &b->queue[(b->first + b->queued) % INPUT_QUEUE];
    event->packet = packet;
    if (packet.bytes) {
        memcpy(event->text, text, packet.bytes);
    }
    b->queued++;
    flush_input(w);
    return true;
}

bool browser_toolbar_mouse(Window *w, int x, int y, unsigned pressed) {
    Browser *b = w->state;
    if (b->closing)
        return true;
    if (!(pressed & 1)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        Rect r = {w->box.x + S(112 + 25 * i), w->box.y + S(8), S(24), S(25)};
        if (!contains(r, x, y)) {
            continue;
        }
        unsigned action = i == 0                              ? BROWSER_BACK
                          : i == 1                            ? BROWSER_FORWARD
                          : (b->navigation & BROWSER_LOADING) ? BROWSER_STOP
                                                              : BROWSER_RELOAD;
        if (!b->ended &&
            (i == 2 || (b->navigation & (i ? BROWSER_CAN_FORWARD : BROWSER_CAN_BACK)))) {
            send_event(w, (struct browser_packet){.type = action}, NULL);
        }
        b->address_focus = false;
        toolbar_dirty(w);
        return true;
    }

    if (contains(address_box(w), x, y)) {
        focus_address(w);
        return true;
    }
    return false;
}

static void resize_browser(Window *w) {
    Browser *b = w->state;
    Rect body = window_body(w);
    if (b->web_engine) {
        struct browser_packet geometry = {.type = BROWSER_SCREEN,
                                          .x = w->box.x,
                                          .y = w->box.y,
                                          .width = desk.width,
                                          .height = desk.height,
                                          .flags = desk.height - S(42),
                                          .canvas_width = w->box.w,
                                          .canvas_height = w->box.h};
        if (memcmp(&geometry, &b->geometry, sizeof(geometry)) && send_event(w, geometry, NULL))
            b->geometry = geometry;
    }

    if (body.w == b->wanted_width && body.h == b->wanted_height) {
        return;
    }
    if (send_event(
            w, (struct browser_packet){.type = BROWSER_RESIZE, .width = body.w, .height = body.h},
            NULL)) {
        b->wanted_width = body.w;
        b->wanted_height = body.h;
    }
}

static void window_number(char *out, int value) {
    if (value < 0) {
        *out++ = '-';
        format_uint(out, -(int64_t)value);
    } else
        format_uint(out, value);
}

static void path_url(char *url, const char *path) {
    static const char hex[] = "0123456789ABCDEF";
    copy_text(url, BROWSER_MAX_TEXT + 1, "file://");
    size_t used = strlen(url);
    for (const unsigned char *p = (const unsigned char *)path; *p && used + 3 < BROWSER_MAX_TEXT;
         p++) {
        if (*p == ' ' || *p == '#' || *p == '%' || *p == '?' || *p < 32 || *p >= 127) {
            url[used++] = '%';
            url[used++] = hex[*p >> 4];
            url[used++] = hex[*p & 15];
        } else {
            url[used++] = *p;
        }
    }

    url[used] = 0;
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int local_document(const char *url, char path[OS_PATH_MAX]) {
    if (strncmp(url, "file://", 7))
        return 0;
    const char *p = url + 7;
    if (!strncmp(p, "localhost/", 10))
        p += 9;
    if (*p != '/')
        return -E_INVAL;
    size_t used = 0;
    while (*p && *p != '#' && *p != '?') {
        unsigned char c = *p++;
        if (c == '%') {
            if (!p[0] || !p[1])
                return -E_INVAL;
            int hi = hex_digit(p[0]), lo = hex_digit(p[1]);
            if (hi < 0 || lo < 0)
                return -E_INVAL;
            c = (hi << 4) | lo;
            p += 2;
        }

        if (!c || used + 1 >= OS_PATH_MAX)
            return -E_NAMETOOLONG;
        path[used++] = c;
    }

    path[used] = 0;
    return 1;
}

int browser_open(Window *w, const char *path) {
    Browser *b = allocate(sizeof(*b));
    if (!b) {
        return -E_NOMEM;
    }

    memset(b, 0, sizeof(*b));
    b->input = b->output = b->errors = -1;
    b->pointer_x = b->pointer_y = -1;
    w->state = b;

    if (path && path[0] == '/') {
        path_url(b->url, path);
    } else {
        copy_text(b->url, sizeof(b->url), path && path[0] ? path : "about:blank");
    }

    b->web_engine = strncmp(b->url, "file://", 7) != 0;
    int in[2] = {-1, -1}, out[2] = {-1, -1}, errors[2] = {-1, -1};
    /* spawn_web's control endpoint is duplex. The trusted desktop keeps the
       same bounded packets and pipe pixel validation for both engines. */
    int error = b->web_engine ? syscall6(SYS_socketpair, 1, 1 | O_CLOEXEC, 0, (long)in, 0, 0)
                              : call2(SYS_pipe2, in, O_CLOEXEC);
    if (!error) {
        error = call2(SYS_pipe2, out, O_CLOEXEC);
    }
    if (!error) {
        error = call2(SYS_pipe2, errors, O_CLOEXEC);
    }
    if (!error) {
        if (b->web_engine && call3(SYS_fcntl, out[0], F_SETPIPE_SZ, 256 * 1024) < 0)
            error = -E_NOMEM;
    }
    if (!error) {
        /* Only the read end presented to the engine is nonblocking. Output
           backpressure sleeps the engine; the GUI never blocks on a pipe. */
        call3(SYS_fcntl, in[0], F_SETFL, O_NONBLOCK);

        Rect body = window_body(w);
        char width[24], height[24];
        format_uint(width, body.w);
        format_uint(height, body.h);
        char *args[] = {"/bin/netsurf", "-v",   "-f", "modernos", "-w",   width,
                        "-h",           height, "-b", "32",       b->url, NULL};
        if (!desk.trace) {
            for (unsigned i = 1; i + 1 < sizeof(args) / sizeof(*args); i++)
                args[i] = args[i + 1];
        }

        char local[OS_PATH_MAX];
        int is_local = local_document(b->url, local);
        struct os_spawn_browser spec = {
            .io = {.fd = {in[0], out[1], errors[1]}, .cwd = (uintptr_t)desk.home},
            .local_file = is_local > 0 ? (uintptr_t)local : 0};
        if (b->web_engine) {
            char screen_width[24], screen_height[24], available_height[24];
            format_uint(screen_width, desk.width);
            format_uint(screen_height, desk.height);
            format_uint(available_height, desk.height - S(42));

            char window_x[24], window_y[24], window_width[24], window_height[24];
            window_number(window_x, w->box.x);
            window_number(window_y, w->box.y);
            format_uint(window_width, w->box.w);
            format_uint(window_height, w->box.h);

            char *web_args[] = {"web-broker", "--native",   b->url,        width,
                                height,       screen_width, screen_height, available_height,
                                window_x,     window_y,     window_width,  window_height,
                                NULL,         NULL,         NULL};
            unsigned web_argc = 12;
            if (desk.browser_network_trace)
                web_args[web_argc++] = "--network-trace";
            if (desk.browser_exception_trace)
                web_args[web_argc++] = "--exception-trace";

            struct os_spawn_web web = {.role = OS_WEB_BROKER,
                                       .ipc_fd = in[0],
                                       .stdout_fd = out[1],
                                       .stderr_fd = errors[1],
                                       .home = (uintptr_t)desk.home};
            error = call3(SYS_spawn_web, "/bin/web-broker", web_args, &web);
        } else
            error = is_local < 0 ? is_local : call3(SYS_spawn_browser, "/bin/netsurf", args, &spec);
        if (error > 0) {
            b->pid = error;
            b->wanted_width = body.w;
            b->wanted_height = body.h;
            error = 0;
        }
    }

    if (in[0] >= 0) {
        close(in[0]);
    }
    if (out[1] >= 0) {
        close(out[1]);
    }
    if (errors[1] >= 0) {
        close(errors[1]);
    }
    if (error) {
        if (in[1] >= 0) {
            close(in[1]);
        }
        if (out[0] >= 0) {
            close(out[0]);
        }
        if (errors[0] >= 0) {
            close(errors[0]);
        }
        deallocate(b, sizeof(*b));
        w->state = NULL;
        return error;
    }

    b->input = in[1];
    b->output = out[0];
    b->errors = errors[0];
    call3(SYS_fcntl, b->input, F_SETFL, O_WRONLY | O_NONBLOCK);
    call3(SYS_fcntl, b->output, F_SETFL, O_NONBLOCK);
    call3(SYS_fcntl, b->errors, F_SETFL, O_NONBLOCK);
    trace("BROWSER_PROCESS", w, b->url);
    return 0;
}

void browser_draw(Window *w) {
    Browser *b = w->state;
    Rect body = window_body(w);
    fill(body, THEME_WINDOW);
    if (b->canvas && b->painted) {
        Rect area = intersect(
            desk.clip, (Rect){body.x, body.y, MIN(body.w, b->width), MIN(body.h, b->height)});
        for (int y = area.y; y < area.y + area.h; y++) {
            memcpy(paint_pixel(area.x, y),
                   b->canvas + (size_t)(y - body.y) * b->width + area.x - body.x,
                   (size_t)area.w * 4);
        }
    } else {
        icon(body.x + S(24), body.y + S(25), I_GLOBE, S(22), THEME_SECONDARY_TEXT);
        label(body.x + S(24), body.y + S(64),
              b->ended ? "Browser process exited" : "Starting browser…", FONT_TITLE,
              THEME_PRIMARY_TEXT, body.w - S(48));
        label(body.x + S(24), body.y + S(98), b->url, FONT_NORMAL, THEME_SECONDARY_TEXT,
              body.w - S(48));
    }
}

static unsigned pointer_buttons(void) {
    return (desk.buttons & 1) | ((desk.buttons & 4) >> 1) | ((desk.buttons & 2) << 1);
}

static void send_pointer(Window *w, int wheel) {
    Browser *b = w->state;
    Rect body = window_body(w);
    int x = clampi(desk.mouse_x - body.x, 0, MAX(0, body.w - 1));
    int y = clampi(desk.mouse_y - body.y, 0, MAX(0, body.h - 1));
    unsigned buttons = pointer_buttons();
    if (x == b->pointer_x && y == b->pointer_y && buttons == b->pointer_buttons && !wheel &&
        desk.mouse_x == b->pointer_screen_x && desk.mouse_y == b->pointer_screen_y) {
        return;
    }
    if (send_event(w,
                   (struct browser_packet){.type = BROWSER_POINTER,
                                           .x = x,
                                           .y = y,
                                           .width = wheel,
                                           .height = buttons != b->pointer_buttons,
                                           .canvas_width = b->web_engine ? desk.mouse_x : 0,
                                           .canvas_height = b->web_engine ? desk.mouse_y : 0,
                                           .flags = buttons},
                   NULL)) {
        b->pointer_x = x;
        b->pointer_y = y;
        b->pointer_screen_x = desk.mouse_x;
        b->pointer_screen_y = desk.mouse_y;
        b->pointer_buttons = buttons;
    }
}

bool browser_mouse(Window *w, int x, int y, unsigned pressed, int wheel) {
    (void)x;
    (void)y;
    (void)pressed;
    Browser *b = w->state;
    if (b->closing)
        return true;
    if (pressed && b->address_focus) {
        b->address_focus = false;
        toolbar_dirty(w);
    }
    send_pointer(w, wheel);
    return true;
}

static void navigate(Window *w, const char *value) {
    char url[BROWSER_MAX_TEXT + 1];
    if (!value[0]) {
        return;
    }
    if (value[0] == '/') {
        path_url(url, value);
    } else if (!strncmp(value, "http://", 7) || !strncmp(value, "https://", 8) ||
               !strncmp(value, "file://", 7) || !strncmp(value, "about:", 6)) {
        copy_text(url, sizeof(url), value);
    } else {
        bool search = false, host = false;
        for (const char *p = value; *p; p++) {
            if (*p == ' ' || *p == '\t')
                search = true;
            if (*p == '.' || *p == ':')
                host = true;
        }

        if (search || (!host && strncmp(value, "localhost", 9))) {
            static const char hex[] = "0123456789ABCDEF";
            copy_text(url, sizeof(url), "https://www.google.com/search?q=");
            size_t used = strlen(url);
            for (const unsigned char *p = (const unsigned char *)value;
                 *p && used + 3 < sizeof(url); p++) {
                if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~')
                    url[used++] = *p;
                else {
                    url[used++] = '%';
                    url[used++] = hex[*p >> 4];
                    url[used++] = hex[*p & 15];
                }
            }

            url[used] = 0;
        } else {
            copy_text(url, sizeof(url), "https://");
            append_text(url, sizeof(url), value);
        }
    }

    Browser *b = w->state;
    char local[OS_PATH_MAX];
    int is_local = local_document(url, local);
    if (is_local < 0) {
        dialog_notice(w, "Browser", "This local document could not be opened.");
        return;
    }
    /* File associations keep their established local-document engine/history
       and grants. Normal Browser windows use WebKit for web navigation. */
    if ((b->web_engine && is_local) || b->ended) {
        b->closing = true;
        b->close_at = 0;
        if (browser_close(w)) {
            int error = browser_open(w, url);
            if (error < 0)
                dialog_notice(w, "Browser", error_text(error));
        }
        return;
    }

    if (is_local && call2(SYS_browser_grant, b->pid, local) < 0) {
        dialog_notice(w, "Browser", "This local document could not be opened.");
        return;
    }
    send_event(w, (struct browser_packet){.type = BROWSER_NAVIGATE}, url);
}

void browser_key(Window *w, Key key) {
    Browser *b = w->state;
    if (b->closing)
        return;
    /* The page must receive modifier releases even when the toolbar consumes
       the associated shortcut or text. Keep this state current for navigation. */
    const unsigned masks[] = {MOD_SHIFT, MOD_CTRL, MOD_ALT};
    const int codes[] = {304, 306, 308};
    for (unsigned i = 0; i < 3; i++) {
        if (!!(key.mods & masks[i]) != !!(b->modifiers & masks[i])) {
            send_event(w,
                       (struct browser_packet){
                           .type = BROWSER_KEY, .x = codes[i], .flags = !!(key.mods & masks[i])},
                       NULL);
        }
    }

    b->modifiers = key.mods;
    if (key.down && (key.mods & MOD_CTRL) && key.code == 0x0f) {
        focus_address(w);
        return;
    }
    if (key.down && (key.mods & MOD_CTRL) && key.code == 0x11) {
        wm_open(APP_BROWSER, NULL);
        return;
    }
    if (b->address_focus) {
        if (!key.down) {
            return;
        }
        if (key.code == KEY_ENTER) {
            navigate(w, b->address);
            b = w->state;
            if (!b)
                return;
            b->address_focus = false;
        } else if (key.code == KEY_ESCAPE || key.code == KEY_TAB) {
            b->address_focus = false;
        } else if ((key.mods & MOD_CTRL) && key.code == 0x04) {
            b->replace_address = true;
        } else {
            bool edits = key.code == KEY_BACKSPACE || key.code == KEY_DELETE ||
                         (key.ascii >= 32 && key.ascii < 127 &&
                          !(key.mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)));
            if (b->replace_address && edits) {
                b->address[0] = 0;
                b->cursor = 0;
            }
            if (edits || key.code == KEY_LEFT || key.code == KEY_RIGHT || key.code == KEY_HOME ||
                key.code == KEY_END) {
                b->replace_address = false;
            }
            input_key(b->address, sizeof(b->address), &b->cursor, key);
        }

        toolbar_dirty(w);
        return;
    }

    if (key.down && ((key.mods & MOD_ALT) && (key.code == KEY_LEFT || key.code == KEY_RIGHT))) {
        send_event(
            w,
            (struct browser_packet){.type = key.code == KEY_LEFT ? BROWSER_BACK : BROWSER_FORWARD},
            NULL);
        return;
    }

    if (key.down && (key.code == KEY_F5 || ((key.mods & MOD_CTRL) && key.code == 0x15))) {
        send_event(w, (struct browser_packet){.type = BROWSER_RELOAD}, NULL);
        return;
    }
    if (key.down && key.code == KEY_ESCAPE && (b->navigation & BROWSER_LOADING)) {
        send_event(w, (struct browser_packet){.type = BROWSER_STOP}, NULL);
        return;
    }
    int code = key.ascii;
    if (key.code >= 4 && key.code <= 29) {
        code = 'a' + key.code - 4;
    }
    switch (key.code) {
    case KEY_ENTER:
        code = 13;
        break;
    case KEY_ESCAPE:
        code = 27;
        break;
    case KEY_BACKSPACE:
        code = 8;
        break;
    case KEY_TAB:
        code = 9;
        break;
    case KEY_DELETE:
        code = 127;
        break;
    case KEY_UP:
        code = 273;
        break;
    case KEY_DOWN:
        code = 274;
        break;
    case KEY_RIGHT:
        code = 275;
        break;
    case KEY_LEFT:
        code = 276;
        break;
    case KEY_HOME:
        code = 278;
        break;
    case KEY_END:
        code = 279;
        break;
    case KEY_PAGEUP:
        code = 280;
        break;
    case KEY_PAGEDOWN:
        code = 281;
        break;
    default:
        break;
    }
    if (key.ascii >= 32 && key.ascii <= 126 && !(key.mods & (MOD_CTRL | MOD_ALT))) {
        code = BROWSER_TEXT_KEY_BASE + key.ascii;
    }
    if (code > 0) {
        send_event(w, (struct browser_packet){.type = BROWSER_KEY, .x = code, .flags = key.down},
                   NULL);
    }
}

static bool valid_packet(const struct browser_packet *packet) {
    if (packet->magic != BROWSER_MAGIC) {
        return false;
    }
    if (packet->type == BROWSER_STATE) {
        return !packet->bytes &&
               !(packet->flags & ~(BROWSER_CAN_BACK | BROWSER_CAN_FORWARD | BROWSER_LOADING));
    }
    if (packet->type == BROWSER_FRAME)
        return browser_surface_rect_valid(packet) &&
               (uint64_t)packet->width * packet->height * 4 == packet->bytes;
    if (packet->type == BROWSER_COPY)
        return browser_surface_rect_valid(packet) && !packet->flags &&
               packet->bytes == sizeof(struct browser_copy_source);
    if (packet->type == BROWSER_PRESENT)
        return browser_surface_present_valid(packet);
    if (packet->type == BROWSER_CANCEL_PAINT)
        return browser_surface_cancel_valid(packet);
    return (packet->type == BROWSER_TITLE || packet->type == BROWSER_URL ||
            packet->type == BROWSER_NOTICE || packet->type == BROWSER_STATUS) &&
           packet->bytes <= BROWSER_MAX_TEXT;
}

static bool apply_packet(Window *w) {
    Browser *b = w->state;
    struct browser_packet *packet = &b->packet;
    if (packet->type == BROWSER_CANCEL_PAINT) {
        if (!b->web_engine)
            return false;
        browser_surface_paint_cancel(&b->paint, deallocate);
        b->paint_started = false;
        trace("BROWSER_PAINT_CANCEL", w, "discarded incomplete engine paint");
        return true;
    }

    if (b->web_engine && (packet->type == BROWSER_FRAME || packet->type == BROWSER_COPY)) {
        if (!browser_surface_paint_queue(&b->paint, b->width, b->height, b->painted, packet,
                                         b->payload, allocate)) {
            browser_failed(w, "The browser sent an invalid or oversized paint.");
            return false;
        }
        return true;
    }

    if (packet->type == BROWSER_PRESENT) {
        if (!b->web_engine || !browser_surface_paint_ready(&b->paint, packet))
            return false;

        bool resized =
            b->width != (int)packet->canvas_width || b->height != (int)packet->canvas_height;
        uint32_t *canvas = b->canvas;
        size_t bytes = (size_t)packet->canvas_width * packet->canvas_height * 4;
        if (resized) {
            canvas = allocate(bytes);
            if (!canvas) {
                browser_failed(w, "Not enough memory for the browser surface.");
                return false;
            }
        }

        if (!browser_surface_paint_apply(canvas, packet->canvas_width, packet->canvas_height,
                                         &b->paint, packet)) {
            if (resized)
                deallocate(canvas, bytes);
            return false;
        }

        int left = packet->canvas_width, top = packet->canvas_height, right = 0, bottom = 0;
        bool copied = false;
        for (struct browser_paint_packet *item = b->paint.first; item; item = item->next) {
            struct browser_packet *p = &item->header;
            left = MIN(left, p->x);
            top = MIN(top, p->y);
            right = MAX(right, p->x + p->width);
            bottom = MAX(bottom, p->y + p->height);
            copied |= p->type == BROWSER_COPY;
        }

        if (resized) {
            deallocate(b->canvas, (size_t)b->width * b->height * 4);
            b->canvas = canvas;
            b->width = packet->canvas_width;
            b->height = packet->canvas_height;
            invalidate_window(w);
        } else {
            Rect body = window_body(w);
            if (w->workspace == desk.workspace && !w->minimized)
                invalidate(intersect(
                    body, (Rect){body.x + left, body.y + top, right - left, bottom - top}));
            else
                w->surface_damage = (Rect){0, 0, w->box.w, w->box.h};
        }

        if (!b->painted)
            trace("BROWSER_FRAME", w, "complete engine frame");
        b->painted = true;
        if (copied)
            trace("BROWSER_SCROLL_COPY", w, "retained engine pixels");
        browser_surface_paint_cancel(&b->paint, deallocate);
        b->paint_started = false;
        trace("BROWSER_PRESENT", w, "complete engine paint");
        return true;
    }

    if (packet->type == BROWSER_STATE) {
        bool was_loading = b->navigation & BROWSER_LOADING;
        b->navigation = packet->flags;
        toolbar_dirty(w);
        bool loading = b->navigation & BROWSER_LOADING;
        if (loading != was_loading) {
            trace(loading ? "BROWSER_LOADING" : "BROWSER_READY", w, w->title);
        }
        return true;
    }

    if (packet->type == BROWSER_FRAME || packet->type == BROWSER_COPY) {
        if (packet->type == BROWSER_COPY) {
            struct browser_copy_source source;
            memcpy(&source, b->payload, sizeof(source));
            if (!b->painted ||
                !browser_surface_copy(b->canvas, b->width, b->height, packet, &source)) {
                browser_failed(w, "The browser sent an invalid surface copy.");
                return false;
            }
            trace("BROWSER_SCROLL_COPY", w, "retained engine pixels");
        } else {
            if (b->width != (int)packet->canvas_width || b->height != (int)packet->canvas_height) {
                size_t bytes = (size_t)packet->canvas_width * packet->canvas_height * 4;
                uint32_t *canvas = allocate(bytes);
                if (!canvas) {
                    browser_failed(w, "Not enough memory for the browser surface.");
                    return false;
                }
                memset(canvas, 0xff, bytes);
                deallocate(b->canvas, (size_t)b->width * b->height * 4);
                b->canvas = canvas;
                b->width = packet->canvas_width;
                b->height = packet->canvas_height;
                invalidate_window(w);
            }

            for (int y = 0; y < packet->height; y++) {
                memcpy(b->canvas + (size_t)(packet->y + y) * b->width + packet->x,
                       b->payload + (size_t)y * packet->width * 4, (size_t)packet->width * 4);
            }
        }

        Rect body = window_body(w);
        if (w->workspace == desk.workspace && !w->minimized) {
            invalidate(intersect(body, (Rect){body.x + packet->x, body.y + packet->y, packet->width,
                                              packet->height}));
        } else {
            w->surface_damage = (Rect){0, 0, w->box.w, w->box.h};
        }
        if (!b->painted) {
            trace("BROWSER_FRAME", w, "complete engine frame");
        }
        b->painted = true;
    } else {
        b->payload[packet->bytes] = 0;
        if (packet->type == BROWSER_TITLE) {
            copy_text(w->title, sizeof(w->title), (char *)b->payload);
            invalidate((Rect){w->box.x, w->box.y, w->box.w, S(40)});
            trace("BROWSER_TITLE", w, w->title);
            /* Async scripts can change the title after the load completes. */
            if (!(b->navigation & BROWSER_LOADING))
                trace("BROWSER_READY", w, w->title);
        } else if (packet->type == BROWSER_URL) {
            copy_text(b->url, sizeof(b->url), (char *)b->payload);
            b->failed = false;
            toolbar_dirty(w);
            trace("BROWSER_URL", w, b->url);
        } else if (packet->type == BROWSER_STATUS) {
            trace("BROWSER_STATUS", w, (char *)b->payload);
            dialog_notice(w, "Browser", (char *)b->payload);
        } else {
            browser_failed(w, (char *)b->payload);
        }
    }

    return true;
}

static void read_frames(Window *w) {
    Browser *b = w->state;
    size_t budget = 4 * 1024 * 1024;
    uint64_t deadline = now_ms() + 4;
    while (budget && !b->ended) {
        if (now_ms() >= deadline)
            break;
        if (b->header_read < sizeof(b->packet)) {
            long count = read(b->output, (uint8_t *)&b->packet + b->header_read,
                              sizeof(b->packet) - b->header_read);
            if (count == -E_AGAIN || count == -E_INTR) {
                break;
            }
            if (count <= 0) {
                b->ended = true;
                break;
            }
            b->header_read += count;
            budget -= MIN(budget, (size_t)count);
            if (b->header_read != sizeof(b->packet)) {
                continue;
            }
            if (!valid_packet(&b->packet)) {
                b->ended = true;
                browser_failed(w, "The browser sent an invalid surface update.");
                break;
            }
            if (b->web_engine && !b->paint_started &&
                (b->packet.type == BROWSER_FRAME || b->packet.type == BROWSER_COPY)) {
                b->paint_started = true;
                trace("BROWSER_PAINT_BEGIN", w, "receiving engine paint");
            }
            size_t needed = (size_t)b->packet.bytes + 1;
            if (needed > b->payload_capacity) {
                uint8_t *payload = allocate(needed);
                if (!payload) {
                    b->ended = true;
                    browser_failed(w, "Not enough memory to receive the browser frame.");
                    break;
                }
                deallocate(b->payload, b->payload_capacity);
                b->payload = payload;
                b->payload_capacity = needed;
            }
        }

        if (b->payload_read < b->packet.bytes) {
            size_t bytes = MIN(budget, (size_t)b->packet.bytes - b->payload_read);
            if (!bytes) {
                break;
            }
            long count = read(b->output, b->payload + b->payload_read, bytes);
            if (count == -E_AGAIN || count == -E_INTR) {
                break;
            }
            if (count <= 0) {
                b->ended = true;
                break;
            }
            b->payload_read += count;
            budget -= count;
            if (b->payload_read != b->packet.bytes) {
                continue;
            }
        }

        if (!apply_packet(w)) {
            b->ended = true;
            break;
        }
        b->header_read = b->payload_read = 0;
    }
}

void browser_tick(Window *w) {
    Browser *b = w->state;
    if (!b->ended) {
        if (!b->closing)
            resize_browser(w);
        flush_input(w);
        read_frames(w);
        if (!b->closing && ((wm_focused() == w && !desk.drag &&
                             contains(window_body(w), desk.mouse_x, desk.mouse_y)) ||
                            (b->pointer_buttons && !desk.buttons))) {
            send_pointer(w, 0);
        }
    }

    /* stderr is always drained separately from the binary render stream. */
    char bytes[1024];
    for (unsigned i = 0; i < 4; i++) {
        long count = read(b->errors, bytes, sizeof(bytes));
        if (count <= 0) {
            break;
        }
        for (long j = 0; j < count; j++) {
            if (bytes[j] == '\n') {
                b->error_line[b->error_used] = 0;
                if (b->error_used) {
                    trace("BROWSER_LOG", w, b->error_line);
                }
                b->error_used = 0;
            } else if ((unsigned char)bytes[j] >= 32 && b->error_used < sizeof(b->error_line) - 1) {
                b->error_line[b->error_used++] = bytes[j];
            }
        }
    }

    if (b->pid) {
        int status = 0;
        int exited = waitpid(b->pid, &status, 1);
        if (exited == b->pid) {
            b->pid = 0;
            b->ended = true;
            invalidate_window(w);
            if (status) {
                browser_failed(
                    w, "The browser exited with an error. See the browser log in desktop tracing.");
            }
        }
    }

    if (b->closing) {
        wm_close(w);
    }
}

bool browser_close(Window *w) {
    Browser *b = w->state;
    if (!b) {
        return true;
    }
    if (b->pid) {
        if (!b->closing) {
            b->closing = true;
            b->close_at = now_ms();
            /* Finish a partially written packet, then prioritize Quit over
               input the closing window will never use. */
            b->queued = b->sent ? 1 : 0;
            send_event(w, (struct browser_packet){.type = BROWSER_QUIT}, NULL);
            return false;
        }

        /* Let Quit cancel downloads and save cookies. Draining in tick keeps
           the GUI responsive while the engine flushes its remaining output.
           Large software frames can take longer than 250 ms to drain under
           emulation. Keep a bounded grace period without blocking the GUI. */
        if (now_ms() - b->close_at < 2000) {
            return false;
        }
        trace("BROWSER_FORCED_CLOSE", w, "engine did not finish shutdown within two seconds");
        call2(SYS_kill, b->pid, 9);
        /* waitpid may yield to this child, but no browser network wait is held. */
        waitpid(b->pid, NULL, 0);
    }

    if (b->input >= 0) {
        close(b->input);
    }
    if (b->output >= 0) {
        close(b->output);
    }
    if (b->errors >= 0) {
        close(b->errors);
    }
    deallocate(b->payload, b->payload_capacity);
    browser_surface_paint_cancel(&b->paint, deallocate);
    deallocate(b->canvas, (size_t)b->width * b->height * 4);
    deallocate(b, sizeof(*b));
    w->state = NULL;
    return true;
}
