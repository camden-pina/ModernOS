#!/usr/bin/env python3
"""Host checks for the real shell/System & Power UI with mocked OS boundaries.

No VM, firmware, real power syscall, native ports or browser build is invoked.
The harness includes the actual UI, editor, terminal and shell translation units;
linker section GC excludes unrelated paths. This is not a rendering/guest proof.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include "userland/desktop/power.c"
#include "userland/desktop/editor.c"
#include "userland/desktop/terminal.c"
#define main coast_shell_main
#include "userland/sh.c"
#undef main

extern void *malloc(size_t);
extern void free(void *);
const char *shell_search_path(void) {return "/bin:/usr/bin";}
Desktop desk;
const AppOps apps[APP_COUNT] = {
    [APP_EDITOR] = {.can_close = editor_confirm_close},
    [APP_TERMINAL] = {.can_close = terminal_can_close}
};
static char model_status[9000], output[16000];
static long read_error, write_error, action_error;
static unsigned last_kind, last_action, power_calls, close_calls, notices, flags;
static unsigned foreground = 42;
static int ioctl_error;
static uint64_t tick = 1000;
static bool truncated, large_snapshots;
static long audio_error;
static unsigned audio_version=1;
static Editor editor;
static Terminal terminal;
static Window *owner;
bool terminal_is_preferences(Window *w) {(void)w; return false;}

void *allocate(size_t n) {return malloc(n);}
void deallocate(void *p, size_t n) {(void)n; free(p);}
void copy_text(char *out, size_t size, const char *in) {
    if (!size) return;
    size_t n = strlen(in); if (n >= size) n = size - 1;
    memmove(out, in, n); out[n] = 0;
}
void append_text(char *out, size_t size, const char *in) {
    size_t n = strlen(out); if (n < size) copy_text(out + n, size - n, in);
}
void format_uint(char *out, uint64_t n) {
    char b[32]; int i = 0; do {b[i++] = '0' + n % 10; n /= 10;} while (n);
    int j = 0; while (i) out[j++] = b[--i]; out[j] = 0;
}
int S(int n) {return n * desk.scale / 100;}
int clampi(int64_t n, int low, int high) {return n < low ? low : n > high ? high : n;}
bool contains(Rect r, int x, int y) {return x >= r.x && y >= r.y && x < r.x+r.w && y < r.y+r.h;}
Rect intersect(Rect a, Rect b) {(void)b; return a;}
Rect window_body(Window *w) {return (Rect){w->box.x, w->box.y+S(WINDOW_HEADER), w->box.w, w->box.h-S(WINDOW_HEADER)};}
uint64_t now_ms(void) {return tick;}
void invalidate_window(Window *w) {(void)w;}
void trace(const char *event,Window *w,const char *detail) {(void)w;(void)detail;assert(event);}
void invalidate(Rect r) {(void)r;}
void clip_set(Rect r) {(void)r;}
void fill(Rect r, uint32_t c) {(void)r;(void)c;}
uint32_t theme_color(uint32_t c) {return c;}
void label(int x,int y,const char *s,Font f,uint32_t c,int w) {(void)x;(void)y;(void)s;(void)f;(void)c;(void)w;}
int text_width(const char *s, Font f) {(void)f;return (int)strlen(s)*8;}
void button(Rect r,const char*s,Icon i,bool p,bool e,bool f) {(void)r;(void)s;(void)i;(void)p;(void)e;(void)f;}
void scrollbar(Rect r,int height,int scroll) {(void)r;assert(height>=scroll);}
void menu_close(void) {}
void launcher_close(void) {}
Window *wm_focused(void) {return &desk.windows[desk.focused];}
void wm_focus(Window *w) {desk.focused = (int)(w-desk.windows);}
Window *wm_open(App app,const char *path) {(void)path; assert(app==APP_POWER); return owner;}
void wm_close(Window *w) {close_calls++; w->alive=false;}
void dialog_notice(Window*w,const char*t,const char*m) {
    notices++; desk.dialog.open=true;desk.dialog.owner=w;desk.dialog.action=NULL;
    copy_text(desk.dialog.title,sizeof(desk.dialog.title),t);
    copy_text(desk.dialog.message,sizeof(desk.dialog.message),m);
}
void dialog_confirm(Window*w,const char*t,const char*m,DialogAction a) {
    dialog_notice(w,t,m);desk.dialog.action=a;
}
static void accept_dialog(void) {
    Dialog d=desk.dialog;assert(d.open&&d.action);desk.dialog.open=false;d.action(d.owner,"");
}
static void cancel_dialog(void) {desk.dialog.open=false;power_review_cancel();}
const char *error_text(long e) {return e == -E_BUSY ? "busy" : e == -E_NOTSUP ? "unsupported" : "mock I/O error";}
void puts(const char *s) {append_text(output,sizeof(output),s);append_text(output,sizeof(output),"\n");}
void printf(const char *fmt,...) {(void)fmt;}
int write_all(int fd,const void*p,size_t n) {(void)fd;(void)p;(void)n;return (int)write_error;}
long number(const char *s,unsigned b) {(void)s;(void)b;return 0;}
int spawn(const char*p,char *const a[],int b) {(void)p;(void)a;(void)b;return -E_NOENT;}
int read_line(int f,char*b,size_t n) {(void)f;(void)b;(void)n;return 0;}
int ioctl(int fd,unsigned request,void *p) {
    (void)fd;assert(request==OS_PTY_GET_FOREGROUND);*(unsigned*)p=foreground;return ioctl_error;
}
static void set_status(const char *state,bool off,bool reset,unsigned events) {
    copy_text(model_status,sizeof(model_status),"power_state=");append_text(model_status,sizeof(model_status),state);
    append_text(model_status,sizeof(model_status),off?"\npoweroff_ready=1":"\npoweroff_ready=0");
    append_text(model_status,sizeof(model_status),reset?"\nreset_ready=1":"\nreset_ready=0");
    append_text(model_status,sizeof(model_status),"\nrequest_id=7\nlast_error=0\nfailure_stage=none\nfixed_button_events=");
    char n[32];format_uint(n,events);append_text(model_status,sizeof(model_status),n);
    append_text(model_status,sizeof(model_status),"\nfirmware_prep_attempted=0\n");
}
long syscall6(long n,long a,long b,long c,long d,long e,long f) {
    (void)d;(void)e;(void)f;
    if(n==SYS_sysinfo) {
        last_kind=(unsigned)a;if(read_error)return read_error;
        if(large_snapshots&&a!=INFO_POWER){memset((void*)b,'S',(size_t)c);return c;}
        const char *s=a==INFO_POWER?model_status:a==INFO_ACPI?"battery=unavailable stale=1\n":a==INFO_CPUS?"cpu=0 online=1\n":"/ ramfs rw\n/mnt/data fat32 rw\n";
        size_t used=strlen(s);if(used>(size_t)c)used=(size_t)c;
        memcpy((void*)b,s,used);
        if(truncated&&a==INFO_POWER){memset((void*)b,'x',(size_t)c);return c;}
        return (long)used;
    }
    if(n==SYS_audio) {
        assert(a==OS_AUDIO_INFO&&c==sizeof(struct os_audio_info));
        if(audio_error)return audio_error;
        *(struct os_audio_info*)b=(struct os_audio_info){.version=audio_version,.rate=48000,.channels=2,.sample_bits=16,.queued_frames=0,.played_frames=12345};return 0;
    }
    if(n==SYS_power) {
        power_calls++;last_action=(unsigned)a;flags=(unsigned)b;
        if(action_error)return action_error;
        set_status(a==OS_POWER_CANCEL?"idle":"pending",true,true,0);return 0;
    }
    if(n==SYS_uname) {
        struct os_utsname *u=(void*)a;memset(u,0,sizeof(*u));
        copy_text(u->sysname,sizeof(u->sysname),"Coast");copy_text(u->machine,sizeof(u->machine),"x86_64");return 0;
    }
    if(n==SYS_clock_gettime) {struct os_timespec *t=(void*)b;t->sec=20;t->nsec=0;return 0;}
    return -E_NOTSUP;
}
static void check_review(void) {
    power_command(owner,0);assert(review.active&&desk.dialog.owner==owner&&!power_calls);
    accept_dialog();assert(power_review_pending(&desk.windows[1])&&editor.dirty&&desk.windows[1].alive);
    accept_dialog();assert(power_review_pending(&desk.windows[2])&&editor.dirty&&!terminal.allow_close);
}
int main(void) {
    uint64_t value;
    assert(!power_status_flag("S5_advertised=1\n","poweroff_ready"));
    assert(!power_status_flag("poweroff_ready=1","poweroff_ready"));
    assert(!power_status_flag("prefix_poweroff_ready=1\n","poweroff_ready"));
    assert(!power_status_flag("poweroff_ready=10\n","poweroff_ready"));
    assert(!power_status_idle("power_state=unknown\n"));
    assert(power_status_cancellable("power_state=freezing\n"));
    assert(!power_status_cancellable("power_state=preparing\n"));
    assert(power_status_uint("n=18446744073709551615\n","n",&value)&&value==UINT64_MAX);
    assert(!power_status_uint("n=18446744073709551616\n","n",&value));
    assert(!power_status_uint("n=-1\n","n",&value));
    desk.width=1024;desk.height=768;desk.scale=100;desk.focused=0;
    owner=&desk.windows[0];*owner=(Window){.alive=true,.id=1,.app=APP_POWER,.box={0,0,600,480}};
    desk.windows[1]=(Window){.alive=true,.id=2,.app=APP_EDITOR,.state=&editor};
    desk.windows[2]=(Window){.alive=true,.id=3,.app=APP_TERMINAL,.state=&terminal};
    editor.dirty=true;terminal.pid=9;terminal.read_fd=10;
    set_status("idle",true,true,0);assert(!power_open(owner,NULL));PowerView*p=owner->state;
    assert(enabled(p,0)&&enabled(p,1)&&!enabled(p,2));
    assert(strstr(p->report,"battery=unavailable stale=1"));
    unsigned diagnostic_calls=power_calls;
    for(int section=0;section<=SYSTEM_DIAGNOSTICS;section++) {
        power_command(owner,10+section);
        assert(power_calls==diagnostic_calls&&control_visible(p,3));
        assert(control_visible(p,0)==(section==SYSTEM_OVERVIEW||section==SYSTEM_DIAGNOSTICS));
    }
    assert(strstr(p->report,"ramfs rw")&&strstr(p->report,"cpu=0 online=1")&&strstr(p->report,"Sample rate Hz: 48000"));
    large_snapshots=true;refresh(owner);
    assert(strlen(p->report)>49000&&strstr(p->report,"Played frames: 12345")&&strstr(p->report,"ABI limit"));
    large_snapshots=false;
    p->section=SYSTEM_AUDIO;audio_error=-E_IO;refresh(owner);assert(strstr(p->report,"Unavailable:"));
    audio_error=0;audio_version=99;refresh(owner);assert(strstr(p->report,"unknown audio ABI version"));audio_version=1;
    p->section=SYSTEM_OVERVIEW;refresh(owner);assert(!power_calls);
    check_review();power_review_cancel();assert(!desk.dialog.open);
    assert(!review.active&&!power_calls&&editor.dirty&&!close_calls);
    assert(!power_review_approved(&desk.windows[1]));
    check_review();accept_dialog();assert(!review.active&&power_calls==1&&last_action==OS_POWER_OFF&&!flags);
    assert(editor.dirty&&!terminal.allow_close&&!close_calls&&desk.windows[1].alive&&desk.windows[2].alive);
    assert(!enabled(p,0)&&!enabled(p,1)&&enabled(p,2));
    activate(owner,0);assert(power_calls==1);activate(owner,2);assert(power_calls==2&&last_action==OS_POWER_CANCEL);
    assert(editor.dirty&&!terminal.allow_close&&!close_calls&&enabled(p,0));
    set_status("freezing",true,true,0);refresh(owner);action_error=-E_BUSY;
    activate(owner,2);assert(power_calls==3&&desk.dialog.open&&strstr(p->notice,"Cancellation rejected"));
    action_error=0;cancel_dialog();
    set_status("idle",false,false,0);refresh(owner);activate(owner,0);activate(owner,1);assert(power_calls==3);
    set_status("new-state",true,true,0);refresh(owner);assert(!enabled(p,0)&&!enabled(p,2));
    copy_text(model_status,sizeof(model_status),"power_state=failed\npoweroff_ready=1\nreset_ready=1\nlast_error=-5\nfailure_stage=flush\nfirmware_prep_attempted=1\n");
    refresh(owner);assert(strstr(p->notice,"failed at flush")&&strstr(p->notice,"effects may remain"));
    set_status("idle",true,true,0);truncated=true;refresh(owner);assert(!p->status_valid&&!enabled(p,0));truncated=false;
    read_error=-E_IO;refresh(owner);assert(!p->status_valid&&!enabled(p,0));read_error=0;
    set_status("idle",true,true,0);refresh(owner);action_error=-E_IO;
    prompt(owner,OS_POWER_RESET);accept_dialog();accept_dialog();accept_dialog();
    assert(power_calls==4&&last_action==OS_POWER_RESET&&strstr(p->notice,"Request rejected"));
    assert(editor.dirty&&!terminal.allow_close&&!close_calls);action_error=0;cancel_dialog();
    ioctl_error=-E_IO;assert(!terminal_can_close(&desk.windows[2])&&desk.dialog.open);cancel_dialog();ioctl_error=0;
    char long_line[4000],line_text[256];memset(long_line,'Z',sizeof(long_line)-1);long_line[sizeof(long_line)-1]=0;
    const char *cursor=long_line;unsigned lines=0,characters=0;
    while(*cursor){cursor=report_line(cursor,line_text,sizeof(line_text),32);characters+=(unsigned)strlen(line_text);lines++;}
    assert(characters==3999&&lines==1000);
    copy_text(p->report,sizeof(p->report),long_line);p->layout_dirty=true;
    assert(scroll_max(owner)>0);power_key(owner,(Key){.down=true,.code=KEY_END});assert(p->scroll==scroll_max(owner));
    power_key(owner,(Key){.down=true,.code=KEY_HOME});assert(!p->scroll);power_draw(owner);
    unsigned before=power_calls;buttons_initialized=false;buttons_at=0;tick+=500;
    set_status("idle",true,true,0);power_review_tick();assert(!review.active);
    set_status("idle",true,true,1);tick+=250;power_review_tick();assert(review.active&&power_calls==before);
    set_status("idle",true,true,2);tick+=250;power_review_tick();assert(review.active&&power_calls==before);
    cancel_dialog();tick+=250;power_review_tick();assert(!review.active&&power_calls==before);
    set_status("idle",true,true,3);tick+=250;power_review_tick();assert(review.active&&power_calls==before);cancel_dialog();
    char *acpi[]={"acpi",NULL},*battery[]={"battery",NULL},*power[]={"power",NULL};
    assert(!command(1,acpi)&&last_kind==INFO_ACPI);assert(!command(1,battery)&&last_kind==INFO_ACPI);
    assert(!command(1,power)&&last_kind==INFO_POWER);
    read_error=-E_IO;assert(command(1,acpi)==1&&command(1,power)==1);read_error=0;
    write_error=-E_IO;assert(command(1,battery)==1);write_error=0;
    char *off[]={"poweroff",NULL},*reset[]={"reboot",NULL},*cancel[]={"power","cancel",NULL};
    assert(!command(1,off)&&last_action==OS_POWER_OFF&&!flags);
    assert(!command(1,reset)&&last_action==OS_POWER_RESET&&!flags);
    assert(!command(2,cancel)&&last_action==OS_POWER_CANCEL&&!flags);
    before=power_calls;char *bad[]={"power","garbage",NULL},*extra[]={"reboot","extra",NULL};
    assert(command(2,bad)==2&&command(2,extra)==2&&power_calls==before);
    action_error=-E_NOTSUP;assert(command(1,off)==1);action_error=0;
    assert(strstr(output,"asynchronous")&&strstr(output,"Cancellation accepted"));
    assert(editor.dirty&&!close_calls);discard(&desk.windows[1],"");assert(!editor.dirty&&close_calls==1);
    confirm_close(&desk.windows[2],"");assert(terminal.allow_close&&close_calls==2);
    power_close(owner);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--sanitize', action='store_true', help='also enable host ASan/UBSan')
    args = parser.parse_args()
    output = ROOT / 'build' / 'power-ui-checks'
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='run-', dir=output) as temporary:
        tmp = Path(temporary)
        # strstr is host-only in this harness; Coast libc does not declare it.
        source = SOURCE.replace('#include <assert.h>', '#include <assert.h>\nextern char *strstr(const char *, const char *);')
        (tmp / 'check.c').write_text(source)
        sanitize = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run([args.cc, *sanitize, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-ffreestanding', '-fno-builtin', '-ffunction-sections', '-fdata-sections',
                        '-I', str(ROOT), '-I', str(ROOT / 'userland/include'), '-I', str(ROOT / 'include'),
                        str(tmp / 'check.c'), '-Wl,--gc-sections', '-o', str(tmp / 'check')], check=True)
        environment = os.environ.copy()
        if args.sanitize:
            # LeakSanitizer cannot enumerate threads under this sandbox/ptrace.
            # Address/undefined-behavior checks remain active.
            environment['ASAN_OPTIONS'] = environment.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
        subprocess.run([str(tmp / 'check')], check=True, env=environment)
    print('PASS actual shell and System & Power UI: bounded/fail-closed snapshots, wrap/scroll, unsupported states, staged editor/terminal consent, cancellation, async/coalescing/error semantics, fixed-button confirmation, all diagnostic views/audio errors and full bounded-snapshot capacity; mocked kernel/rendering only')


if __name__ == '__main__':
    main()
