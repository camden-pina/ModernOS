#include "desktop.h"
#include <power_status.h>
#include <uapi/audio.h>

/* Snapshot-only diagnostics: no arbitrary AML and no polling firmware in
 * ring 3. */
enum { SNAPSHOT_BYTES = 8192, REPORT_BYTES = SNAPSHOT_BYTES * 9, POWER_CONTROLS = 4 };
typedef struct {
    char status[SNAPSHOT_BYTES + 1], report[REPORT_BYTES], notice[256];
    int scroll, content_height, focus, layout_width, layout_scale, section;
    bool status_valid, layout_dirty;
    uint64_t refresh_at;
    SmoothScroll animation;
} PowerView;

/* Consent belongs to this immediate modal review and these exact window IDs.
   It must never set editor dirty=false, close a window, or signal a process. */
static struct {
    bool active;
    unsigned action;
    Window *owner, *waiting;
    int owner_id, waiting_id, approved[MAX_WINDOWS];
    Window *dialog_owner;
    DialogAction dialog_action;
} review;
enum {
    SYSTEM_OVERVIEW,
    SYSTEM_CPU,
    SYSTEM_STORAGE,
    SYSTEM_PROCESSES,
    SYSTEM_NETWORK,
    SYSTEM_AUDIO,
    SYSTEM_DIAGNOSTICS
};
static const char *section_names[] = {"Power and battery", "Processor inventory", "Storage mounts",
                                      "Processes",         "Network diagnostics", "Audio device",
                                      "Full diagnostics"};
static uint64_t buttons_at, buttons_seen;
static bool buttons_initialized, button_pending;

static long snapshot(unsigned kind, char *out) {
    long n = call3(SYS_sysinfo, kind, out, SNAPSHOT_BYTES);
    if (n < 0 || n > SNAPSHOT_BYTES) {
        out[0] = 0;
        return n < 0 ? n : -E_IO;
    }
    out[n] = 0;
    return n;
}

static void report_append(PowerView *p, const char *text) {
    append_text(p->report, sizeof(p->report), text);
}

static void report_section(PowerView *p, const char *title, unsigned kind) {
    char buffer[SNAPSHOT_BYTES + 1];
    report_append(p, title);
    long n = snapshot(kind, buffer);
    if (n < 0) {
        report_append(p, "Unavailable: ");
        report_append(p, error_text(n));
    } else {
        /* Preserve newlines; keep control bytes from affecting text layout. */
        for (long i = 0; i < n; i++)
            if ((unsigned char)buffer[i] < 32 && buffer[i] != '\n')
                buffer[i] = ' ';
        report_append(p, buffer);
        if (n >= SNAPSHOT_BYTES - 1)
            report_append(p, "\n[Snapshot reached the 8192-byte ABI limit; it may be truncated.]");
    }
    report_append(p, "\n\n");
}

static void refresh(Window *w) {
    PowerView *p = w->state;
    long n = snapshot(INFO_POWER, p->status);
    p->status_valid = n >= 0 && n < SNAPSHOT_BYTES - 1;
    char state[24];
    if (p->status_valid && power_status_value(p->status, "power_state", state, sizeof(state)) &&
        !strcmp(state, "failed")) {
        char value[64];
        copy_text(p->notice, sizeof(p->notice), "Power request failed");
        if (power_status_value(p->status, "failure_stage", value, sizeof(value))) {
            append_text(p->notice, sizeof(p->notice), " at ");
            append_text(p->notice, sizeof(p->notice), value);
        }
        if (power_status_value(p->status, "last_error", value, sizeof(value))) {
            append_text(p->notice, sizeof(p->notice), "; error ");
            append_text(p->notice, sizeof(p->notice), value);
        }
        if (power_status_flag(p->status, "firmware_prep_attempted"))
            append_text(p->notice, sizeof(p->notice),
                        ". Firmware preparation was attempted; its effects may remain.");
    }
    p->report[0] = 0;
    struct os_utsname os;
    if (!call1(SYS_uname, &os)) {
        /* A malformed/older uname provider must not overrun its fixed fields. */
        os.sysname[64] = os.release[64] = os.version[64] = os.machine[64] = 0;
        report_append(p, os.sysname);
        report_append(p, " ");
        report_append(p, os.release);
        report_append(p, " ");
        report_append(p, os.machine);
        report_append(p, "\n");
        report_append(p, os.version);
        report_append(p, "\n");
    } else {
        report_append(p, "OS identity unavailable\n");
    }
    struct os_timespec uptime;
    if (!call2(SYS_clock_gettime, 1, &uptime) && uptime.sec >= 0) {
        char number[32];
        format_uint(number, (uint64_t)uptime.sec);
        report_append(p, "Uptime seconds: ");
        report_append(p, number);
        report_append(p, "\n");
    } else {
        report_append(p, "Uptime unavailable\n");
    }
    report_append(p, "\n");
    if (p->section == SYSTEM_OVERVIEW || p->section == SYSTEM_DIAGNOSTICS) {
        report_append(p, "Coordinated power (cached)\n");
        if (n < 0) {
            report_append(p, "Unavailable: ");
            report_append(p, error_text(n));
        } else {
            report_append(p, p->status);
            if (!p->status_valid)
                report_append(p, "\n[Power snapshot may be truncated; actions disabled.]");
        }
        report_append(p, "\n\n");
        report_section(p, "ACPI, AC adapters and batteries (cached)\n", INFO_ACPI);
        report_append(p, "RAM files are volatile. A writable disk mount is "
                         "required for persistence. "
                         "A flush does not make FAT32 power-loss atomic.\n\n"
                         "Accepted power requests are asynchronous. Cancellation "
                         "is limited to pending/freezing; "
                         "firmware preparation can have effects that software "
                         "cancellation cannot undo.\n");
    }
    if (p->section == SYSTEM_CPU || p->section == SYSTEM_DIAGNOSTICS)
        report_section(p, "CPU inventory\n", INFO_CPUS);
    if (p->section == SYSTEM_STORAGE || p->section == SYSTEM_DIAGNOSTICS) {
        report_section(p, "Storage mounts\n", INFO_MOUNTS);
        report_section(p, "Block devices\n", INFO_DISKS);
    }
    if (p->section == SYSTEM_PROCESSES || p->section == SYSTEM_DIAGNOSTICS)
        report_section(p, "Process snapshot\n", INFO_PROCESSES);
    if (p->section == SYSTEM_NETWORK || p->section == SYSTEM_DIAGNOSTICS) {
        report_section(p, "Network snapshot\n", INFO_NETWORK);
        report_append(p, "This view reports the kernel's implemented network "
                         "devices and counters. "
                         "It does not provide Wi-Fi configuration.\n");
    }
    if (p->section == SYSTEM_AUDIO || p->section == SYSTEM_DIAGNOSTICS) {
        struct os_audio_info audio = {0};
        long error = call3(SYS_audio, OS_AUDIO_INFO, &audio, sizeof(audio));
        report_append(p, "Native PCM device\n");
        if (error < 0 || audio.version != 1) {
            report_append(p, "Unavailable: ");
            report_append(p, error < 0 ? error_text(error) : "unknown audio ABI version");
        } else {
            char number[32];
            const char *keys[] = {"Sample rate Hz: ", "Channels: ", "Sample bits: ",
                                  "Queued frames: ", "Played frames: "};
            uint64_t values[] = {audio.rate, audio.channels, audio.sample_bits, audio.queued_frames,
                                 audio.played_frames};
            for (unsigned i = 0; i < sizeof(values) / sizeof(*values); i++) {
                format_uint(number, values[i]);
                report_append(p, keys[i]);
                report_append(p, number);
                report_append(p, "\n");
            }
        }
        report_append(p, "\nCounters describe the actual PCM device. A desktop "
                         "master-volume service is not available.\n");
    }
    p->refresh_at = now_ms() + 500;
    p->layout_dirty = true;
    invalidate_window(w);
}

static bool enabled(PowerView *p, int control) {
    if (control == 3)
        return !review.active;
    if (review.active || !p->status_valid)
        return false;
    if (control == 2)
        return power_status_cancellable(p->status);
    return power_status_idle(p->status) &&
           power_status_flag(p->status, control == 0 ? "poweroff_ready" : "reset_ready");
}

static bool power_controls(PowerView *p) {
    return p->section == SYSTEM_OVERVIEW || p->section == SYSTEM_DIAGNOSTICS;
}
static bool control_visible(PowerView *p, int control) { return power_controls(p) || control == 3; }
static Rect control_rect(Window *w, int control) {
    Rect b = window_body(w);
    PowerView *p = w->state;
    if (!power_controls(p) && control == 3)
        return (Rect){b.x + b.w - S(126), b.y + S(10), S(108), S(29)};
    int gap = S(8), width = (b.w - S(36) - 3 * gap) / POWER_CONTROLS;
    return (Rect){b.x + S(18) + control * (width + gap), b.y + S(44), width, S(29)};
}

static Rect report_view(Window *w) {
    Rect b = window_body(w);
    int top = power_controls(w->state) ? 143 : 78;
    return (Rect){b.x + S(18), b.y + S(top), b.w - S(36), MAX(S(12), b.h - S(top + 11))};
}

/* Hard wrapping is intentional: even a long firmware identifier stays readable.
   One character always advances, including in an extremely narrow viewport. */
static const char *report_line(const char *cursor, char *line_text, size_t cap, int width) {
    size_t used = 0;
    while (*cursor && *cursor != '\n' && used + 1 < cap) {
        line_text[used] = *cursor;
        line_text[used + 1] = 0;
        if (used && text_width(line_text, FONT_MONO) > width)
            break;
        cursor++;
        used++;
    }
    line_text[used] = 0;
    if (*cursor == '\n')
        cursor++;
    return cursor;
}

static int report_height(Window *w, bool paint) {
    PowerView *p = w->state;
    Rect v = report_view(w);
    int height = 0, row = S(17), width = MAX(1, v.w - S(15));
    const char *cursor = p->report;
    while (*cursor) {
        char text[256];
        cursor = report_line(cursor, text, sizeof(text), width);
        int y = v.y + height - p->scroll;
        if (paint && y + row > v.y && y < v.y + v.h)
            label(v.x, y, text, FONT_MONO, THEME_TEXT, width);
        height += row;
    }
    return height;
}

static int scroll_max(Window *w) {
    PowerView *p = w->state;
    int width = report_view(w).w;
    if (p->layout_dirty || p->layout_width != width || p->layout_scale != desk.scale) {
        p->content_height = report_height(w, false);
        p->layout_dirty = false;
        p->layout_width = width;
        p->layout_scale = desk.scale;
    }
    return MAX(0, p->content_height - report_view(w).h);
}

void power_review_cancel(void) {
    /* Do not leave a former review callback armed as an ordinary Discard/Close
       after consent has been cleared (for example, when a window is closed). */
    if (review.active && desk.dialog.open && desk.dialog.owner == review.dialog_owner &&
        desk.dialog.action == review.dialog_action) {
        Rect b = desk.dialog.box;
        desk.dialog.open = false;
        invalidate((Rect){b.x - S(8), b.y - S(8), b.w + S(16), b.h + S(16)});
    }
    memset(&review, 0, sizeof(review));
}

bool power_review_pending(Window *w) {
    return review.active && w && w->alive && review.waiting == w && review.waiting_id == w->id;
}

bool power_review_approved(Window *w) {
    if (!review.active || !w || !w->alive)
        return false;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (&desk.windows[i] == w)
            return review.approved[i] == w->id;
    return false;
}

static void remember_dialog(void) {
    review.dialog_owner = desk.dialog.owner;
    review.dialog_action = desk.dialog.action;
}

static void continue_review(void) {
    Window *owner = review.owner;
    if (!review.active || !owner || !owner->alive || owner->id != review.owner_id) {
        power_review_cancel();
        return;
    }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window *w = &desk.windows[i];
        if (!w->alive || !apps[w->app].can_close)
            continue;
        review.waiting = w;
        review.waiting_id = w->id;
        if (!apps[w->app].can_close(w)) {
            if (!desk.dialog.open) {
                power_review_cancel();
                dialog_notice(owner, "Power request canceled",
                              "An application did not approve the request.");
            } else {
                remember_dialog();
                trace("POWER_WAIT", w, apps[w->app].name);
            }
            return;
        }
        review.waiting = NULL;
    }
    unsigned action = review.action;
    power_review_cancel();
    /* Recheck after all human decisions. Another shell/client may have acted. */
    refresh(owner);
    PowerView *p = owner->state;
    int control = action == OS_POWER_OFF ? 0 : 1;
    if (!enabled(p, control)) {
        copy_text(p->notice, sizeof(p->notice),
                  "Request not sent: backend unavailable or another transition is "
                  "active.");
        dialog_notice(owner, "Power request unavailable", p->notice);
        return;
    }
    long result = call2(SYS_power, action, 0);
    trace("POWER_REQUEST", owner, result < 0 ? "rejected" : "accepted/coalesced");
    copy_text(p->notice, sizeof(p->notice),
              result < 0 ? "Request rejected: "
                         : "Request accepted/coalesced. Completion is "
                           "asynchronous. Cancel quickly if needed.");
    if (result < 0) {
        append_text(p->notice, sizeof(p->notice), error_text(result));
        dialog_notice(owner, "Power request failed", p->notice);
    }
    refresh(owner);
}

bool power_review_accept(Window *w) {
    if (!power_review_pending(w))
        return false;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (&desk.windows[i] == w)
            review.approved[i] = w->id;
    review.waiting = NULL;
    trace("POWER_CONSENT", w, apps[w->app].name);
    continue_review();
    return true;
}

static void begin_review(Window *w, const char *unused) {
    (void)unused;
    if (review.active && review.owner == w && w && w->alive && w->id == review.owner_id)
        continue_review();
    else
        power_review_cancel();
}

static void prompt(Window *w, unsigned action) {
    PowerView *p = w->state;
    refresh(w);
    if (!enabled(p, action == OS_POWER_OFF ? 0 : 1))
        return;
    desk.quit_pending = false;
    menu_close();
    launcher_close();
    memset(&review, 0, sizeof(review));
    review.active = true;
    review.action = action;
    review.owner = w;
    review.owner_id = w->id;
    dialog_confirm(w, action == OS_POWER_OFF ? "Shut down the system?" : "Restart the system?",
                   "Save your work first. Unsaved editor documents and running terminal "
                   "programs "
                   "will each need your approval. Other apps may also have unsaved work. "
                   "Cancellation is available briefly before preparation and is not "
                   "guaranteed once it starts.",
                   begin_review);
    remember_dialog();
    trace("POWER_REVIEW", w, action == OS_POWER_OFF ? "shutdown" : "restart");
}

static void activate(Window *w, int control) {
    PowerView *p = w->state;
    if (!enabled(p, control))
        return;
    if (control < 2) {
        prompt(w, control == 0 ? OS_POWER_OFF : OS_POWER_RESET);
    } else if (control == 2) {
        long result = call2(SYS_power, OS_POWER_CANCEL, 0);
        trace("POWER_CANCEL", w, result < 0 ? "rejected" : "accepted");
        copy_text(p->notice, sizeof(p->notice),
                  result < 0 ? "Cancellation rejected: "
                             : "Cancellation accepted. Check the state below; all "
                               "windows are preserved.");
        if (result < 0) {
            append_text(p->notice, sizeof(p->notice), error_text(result));
            append_text(p->notice, sizeof(p->notice), ". Preparation may already have started.");
            dialog_notice(w, "Cancellation not accepted", p->notice);
        }
        refresh(w);
    } else {
        refresh(w);
    }
}

int power_open(Window *w, const char *path) {
    PowerView *p = allocate(sizeof(*p));
    if (!p)
        return -E_NOMEM;
    memset(p, 0, sizeof(*p));
    w->state = p;
    const char *paths[] = {"overview", "cpu",   "storage",    "processes",
                           "network",  "audio", "diagnostics"};
    for (unsigned i = 0; path && i < sizeof(paths) / sizeof(*paths); i++)
        if (!strcmp(path, paths[i]))
            p->section = i;
    p->focus = 3;
    copy_text(p->notice, sizeof(p->notice),
              "Unavailable or missing readiness data disables power controls.");
    refresh(w);
    return 0;
}

void power_draw(Window *w) {
    PowerView *p = w->state;
    Rect b = window_body(w), saved = desk.clip;
    clip_set(intersect(saved, b));
    fill(b, THEME_WINDOW);
    label(b.x + S(18), b.y + S(15), section_names[p->section], FONT_TITLE, THEME_PRIMARY_TEXT,
          b.w - S(power_controls(p) ? 36 : 158));
    const char *names[] = {"Shut down", "Restart", "Cancel", "Refresh"};
    for (int i = 0; i < POWER_CONTROLS; i++)
        if (control_visible(p, i))
            button(control_rect(w, i), names[i], I_COUNT, i < 2, enabled(p, i), p->focus == i);
    if (power_controls(p)) {
        char state[32], summary[160];
        bool known =
            p->status_valid && power_status_value(p->status, "power_state", state, sizeof(state));
        copy_text(summary, sizeof(summary), "State: ");
        append_text(summary, sizeof(summary), known ? state : "unavailable");
        append_text(summary, sizeof(summary), ". Details, request ID and failures below.");
        label(b.x + S(18), b.y + S(83), summary, FONT_BODY, THEME_PRIMARY_TEXT, b.w - S(36));
        const char *cursor = p->notice;
        for (int row = 0; *cursor && row < 2; row++) {
            char text[256];
            cursor = report_line(cursor, text, sizeof(text), b.w - S(36));
            label(b.x + S(18), b.y + S(104 + row * 17), text, FONT_MONO, THEME_SECONDARY_TEXT,
                  b.w - S(36));
        }
    } else {
        label(b.x + S(18), b.y + S(48),
              "Kernel snapshot  |  F5 refresh  |  System menu for power actions", FONT_SMALL,
              THEME_SECONDARY_TEXT, b.w - S(36));
    }
    Rect v = report_view(w);
    p->scroll = clampi(p->scroll, 0, scroll_max(w));
    clip_set(intersect(saved, v));
    report_height(w, true);
    scrollbar(v, p->content_height, p->scroll);
    clip_set(saved);
}

Rect power_hover(Window *w, int x, int y) {
    PowerView *p = w->state;
    for (int i = 0; i < POWER_CONTROLS; i++) {
        Rect r = control_rect(w, i);
        if (control_visible(p, i) && enabled(p, i) && contains(r, x, y))
            return r;
    }
    return (Rect){0};
}

bool power_mouse(Window *w, int x, int y, unsigned pressed, int wheel) {
    PowerView *p = w->state;
    if (wheel) {
        smooth_by(&p->animation, p->scroll, -(int64_t)wheel * S(51), scroll_max(w), now_ms());
        return true;
    }
    if (pressed & 1)
        for (int i = 0; i < POWER_CONTROLS; i++)
            if (control_visible(p, i) && contains(control_rect(w, i), x, y)) {
                p->focus = i;
                activate(w, i);
                invalidate_window(w);
                return true;
            }
    return false;
}

void power_key(Window *w, Key key) {
    if (!key.down ||
        (key.repeat && (key.code == KEY_ENTER || key.code == KEY_SPACE || key.code == KEY_F5)))
        return;
    PowerView *p = w->state;
    int maximum = scroll_max(w), page = report_view(w).h;
    if (key.code == KEY_TAB) {
        int step = key.mods & MOD_SHIFT ? POWER_CONTROLS - 1 : 1;
        for (int i = 0; i < POWER_CONTROLS; i++) {
            p->focus = (p->focus + step) % POWER_CONTROLS;
            if (control_visible(p, p->focus) && enabled(p, p->focus))
                break;
        }
    } else if (key.code == KEY_ENTER || key.code == KEY_SPACE) {
        if (control_visible(p, p->focus))
            activate(w, p->focus);
    } else if (key.code == KEY_F5 ||
               ((key.mods & MOD_CTRL) && (key.ascii == 'r' || key.ascii == 'R'))) {
        if (enabled(p, 3))
            refresh(w);
    } else if (key.code == KEY_HOME) {
        p->scroll = 0;
    } else if (key.code == KEY_END) {
        p->scroll = maximum;
    } else if (key.code == KEY_UP || key.code == KEY_DOWN || key.code == KEY_PAGEUP ||
               key.code == KEY_PAGEDOWN) {
        int delta = key.code == KEY_UP       ? -S(17)
                    : key.code == KEY_DOWN   ? S(17)
                    : key.code == KEY_PAGEUP ? -page
                                             : page;
        p->scroll = clampi(p->scroll + delta, 0, maximum);
    }
    smooth_sync(&p->animation, p->scroll, maximum, now_ms());
    invalidate_window(w);
}

void power_tick(Window *w) {
    PowerView *p = w->state;
    uint64_t now = now_ms();
    if (now >= p->refresh_at)
        refresh(w);
    int position = smooth_tick(&p->animation, p->scroll, scroll_max(w), now);
    if (position != p->scroll) {
        p->scroll = position;
        invalidate_window(w);
    }
}

bool power_close(Window *w) {
    if (review.owner == w)
        power_review_cancel();
    deallocate(w->state, sizeof(PowerView));
    w->state = NULL;
    return true;
}

void power_review_tick(void) {
    if (review.active && (!desk.dialog.open || desk.dialog.owner != review.dialog_owner ||
                          desk.dialog.action != review.dialog_action))
        power_review_cancel();
    uint64_t now = now_ms();
    if (now < buttons_at)
        return;
    buttons_at = now + 250;
    char status[SNAPSHOT_BYTES + 1];
    uint64_t sequence;
    if (snapshot(INFO_POWER, status) < 0 ||
        !power_status_uint(status, "fixed_button_events", &sequence))
        return;
    if (!buttons_initialized) {
        buttons_initialized = true;
        buttons_seen = sequence;
        return;
    }
    if (sequence != buttons_seen) {
        buttons_seen = sequence;
        if (!review.active && power_status_idle(status))
            button_pending = true;
    }
    if (!button_pending || desk.dialog.open || review.active || desk.quit_pending)
        return;
    button_pending = false;
    if (!power_status_idle(status))
        return;
    Window *w = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (desk.windows[i].alive && desk.windows[i].app == APP_POWER) {
            w = &desk.windows[i];
            break;
        }
    if (w)
        wm_focus(w);
    else
        w = wm_open(APP_POWER, NULL);
    if (!w)
        return;
    PowerView *p = w->state;
    refresh(w);
    if (enabled(p, 0))
        prompt(w, OS_POWER_OFF);
    else
        dialog_notice(w, "Power button pressed",
                      "Coordinated shutdown is unavailable. See the cached report "
                      "for the reason. No power request was sent.");
}

static void power_command(Window *w, int command) {
    PowerView *p = w->state;
    if (command < 4) {
        if (control_visible(p, command))
            p->focus = command;
        activate(w, command);
    } else if (command == 4)
        wm_close(w);
    else if (command >= 10 && command <= 16) {
        p->section = command - 10;
        if (!power_controls(p))
            p->focus = 3;
        p->scroll = 0;
        smooth_jump(&p->animation, 0, now_ms());
        refresh(w);
        trace("SYSTEM_VIEW", w, section_names[p->section]);
    }
}
void power_menu(Window *w, int group, int x, int y) {
    PowerView *p = w->state;
    if (group == MENU_FILE) {
        MenuItem items[] = {{"Refresh snapshots\tF5", I_RELOAD, 3, enabled(p, 3)},
                            {"Shut down...", I_COUNT, 0, enabled(p, 0)},
                            {"Restart...", I_RELOAD, 1, enabled(p, 1)},
                            {"Cancel pending request", I_CLOSE, 2, enabled(p, 2)},
                            {"Close window\tAlt+F4", I_CLOSE, 4, true}};
        menu_open(w, x, y, items, 5, power_command);
    } else if (group == MENU_VIEW) {
        MenuItem items[7];
        for (int i = 0; i < 7; i++)
            items[i] =
                (MenuItem){section_names[i], p->section == i ? I_CHECK : I_INFO, 10 + i, true};
        menu_open(w, x, y, items, 7, power_command);
    }
}
