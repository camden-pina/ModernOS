#include "desktop.h"
#include "pixel_damage.h"
#include <uapi/window.h>

typedef struct {
    struct os_window_info info;
    uint32_t *pixels;
    size_t pixel_bytes;
    unsigned requested_width,requested_height,requested_states;
    uint64_t configured_at;
    int pid, output;
    bool painted;
    uint64_t frames, changed_pixels, unchanged_frames, copied_pixels;
    Rect view;
    int pointer_x,pointer_y;
    unsigned pointer_buttons;
    bool pointer_entered,pointer_grab;
    struct os_window_cursor cursor;
    uint32_t cursor_pixels[OS_WINDOW_CURSOR_MAX*OS_WINDOW_CURSOR_MAX];
} Game;
static struct os_window_info game_pending;
static bool ready;
static unsigned focused;
static Window *capture;
static struct os_keyboard_snapshot keyboard_state;
static uint64_t keyboard_skip, keyboard_current;
static bool keyboard_reset_pending;
static bool keyboard_published;
static uint64_t keyboard_published_sequence;
static struct {
    int pid, output;
    uint64_t at;
    unsigned grace_ms;
} retired[64];
static unsigned retired_count;
static long window_op(unsigned op, void *p, size_t n) { return call3(SYS_window, op, p, n); }
static void send(Window *,struct os_window_event);
static bool native_pixels(Game *g) { return g->info.version == OS_WINDOW_PIXEL_CLIENT_VERSION; }
bool game_window_loading(Window *w) {
    return native_window_app(w->app) && w->state && !((Game *)w->state)->painted;
}
static bool full_keyboard(Game *g) {
    return g->info.version == OS_WINDOW_KEYBOARD_CLIENT_VERSION || native_pixels(g);
}
static size_t pixel_capacity(Game *g) {
    return native_pixels(g)?(size_t)OS_WINDOW_PIXEL_MAX_WIDTH*OS_WINDOW_PIXEL_MAX_HEIGHT*4:
                           (size_t)g->info.width*g->info.height*4;
}
static void configure_native_window(Window *w,Game *g) {
    if(!native_pixels(g) || !g->info.id)return;
    /* The border follows input independently from asynchronous client paints.
       Retained content stays at 1:1 until Electron supplies its next layout. */
    Rect body=window_body(w);
    unsigned width=MAX(2,MIN(body.w,OS_WINDOW_PIXEL_MAX_WIDTH)&~1);
    unsigned height=MAX(2,MIN(body.h,OS_WINDOW_PIXEL_MAX_HEIGHT)&~1);
    unsigned states=(w->maximized?OS_WINDOW_STATE_MAXIMIZED:0)|
                    (w->minimized?OS_WINDOW_STATE_MINIMIZED:0)|
                    (w->fullscreen?OS_WINDOW_STATE_FULLSCREEN:0);
    if(width==g->requested_width && height==g->requested_height && states==g->requested_states)return;
    uint64_t now=now_ms();
    if(desk.drag==w && desk.drag_mode!=16 && states==g->requested_states) {
        bool pending=g->info.width!=g->requested_width || g->info.height!=g->requested_height;
        /* One relayout in flight, with the newest pointer target coalesced.
           Timeout allows recovery from a client that skips a configure. */
        if (now-g->configured_at<16 || (pending && now-g->configured_at<160)) return;
    }
    struct os_window_configure request={g->info.id,width,height,states};
    if(!window_op(OS_WINDOW_SERVER_CONFIGURE,&request,sizeof(request))) {
        g->requested_width=width;g->requested_height=height;g->requested_states=states;
        g->configured_at=now;
        if(desk.trace)printf("DESKTOP NATIVE_CONFIGURE window=%d width=%u height=%u states=%u ms=%lu\n",w->id,width,height,states,now);
    }
}
static void fit_native_window(Window *w, Game *g) {
    if (!native_pixels(g) || w->maximized || w->snapped || w->fullscreen)return;
    int width=MIN((int)g->info.width+2,desk.width);
    int height=MIN((int)g->info.height+S(WINDOW_HEADER)+1,
                   desk.height-S(DESKTOP_TOP)-desktop_bottom());
    w->box=(Rect){(desk.width-width)/2,
                  S(DESKTOP_TOP)+(desk.height-S(DESKTOP_TOP)-desktop_bottom()-height)/2,
                  width,height};
    w->restore=w->box;
    wm_trace_geometry(w);
}
static void pixel_pointer(Window *w,int wheel) {
    Game *g=w->state;
    if(!g || !native_pixels(g) || !g->painted)return;
    bool eligible=wm_focused()==w && !desk.dialog.open && !desk.launcher &&
                  !desk.menu.open && !desk.snap_open && !desk.snap_assist && !desk.snap_pointer_blocked && !desk.drag && !w->minimized;
    Rect body=window_body(w), view=body;
    bool inside=eligible && wm_pointer_active(w) && contains(body,desk.mouse_x,desk.mouse_y);
    if(inside && desk.buttons)g->pointer_grab=true;
    bool active=inside || (eligible && g->pointer_grab);
    int x=desk.mouse_x-view.x;
    int y=desk.mouse_y-view.y;
    unsigned buttons=active?desk.buttons:0;
    if(desk.trace && active!=g->pointer_entered)
        printf("DESKTOP NATIVE_POINTER window=%d active=%d x=%d y=%d\n",w->id,active,x,y);
    if(active!=g->pointer_entered || (active && (x!=g->pointer_x || y!=g->pointer_y || buttons!=g->pointer_buttons || wheel)))
        send(w,(struct os_window_event){.type=OS_WINDOW_POINTER,.code=active,.dx=x,.dy=y,.buttons=buttons,.wheel=active?wheel:0});
    g->pointer_entered=active;g->pointer_x=x;g->pointer_y=y;g->pointer_buttons=buttons;
    if(!buttons || !eligible)g->pointer_grab=false;
}
static void release_capture(void) {
    if (capture) {
        trace("GAME_CAPTURE", capture, "released");
        capture = NULL;
        invalidate_all();
    }
}
static void drain(int fd) {
    if (fd < 0)
        return;
    char bytes[1024];
    for (unsigned i = 0; i < 16; i++) {
        long n = read(fd, bytes, sizeof(bytes));
        if (n <= 0)
            break;
        write_all(1, bytes, n);
    }
}
static unsigned identifier(Window *w) {
    return w && w->alive && native_window_app(w->app) && w->state ? ((Game *)w->state)->info.id : 0;
}
static void keyboard_resync(void) {
    struct os_keyboard_snapshot s={.version=OS_KEYBOARD_STATE_VERSION,.size=sizeof(s)};
    if(call3(SYS_display,DISPLAY_KEYBOARD_SNAPSHOT,&s,sizeof(s))) {
        if(keyboard_state.flags&OS_KEYBOARD_VALID)keyboard_reset_pending=true;
        keyboard_state.flags=0;
        return;
    }
    if(!keyboard_state.version || s.source_generation!=keyboard_state.source_generation ||
       ((s.flags^keyboard_state.flags)&OS_KEYBOARD_VALID)) {
        keyboard_state=s;
        keyboard_skip=s.sequence;
        keyboard_reset_pending=true;
    }
}
void game_keyboard_source(const struct os_input_event *e) {
    keyboard_current=e->sequence;
    if(e->sequence<=keyboard_skip || !(keyboard_state.flags&OS_KEYBOARD_VALID))return;
    if(e->code<4 || e->code>255 || e->dy<0 || e->dy>255) {
        keyboard_state.flags=0;keyboard_reset_pending=true;return;
    }
    unsigned key=e->code,mask=1u<<(key%8);
    bool held=(keyboard_state.pressed[key/8]&mask)!=0;
    if(e->buttons && !held) {
        if(key==0x39)keyboard_state.locks^=OS_KEYBOARD_LOCK_CAPS;
        if(key==0x53)keyboard_state.locks^=OS_KEYBOARD_LOCK_NUM;
        if(key==0x47)keyboard_state.locks^=OS_KEYBOARD_LOCK_SCROLL;
    }
    if(e->buttons)keyboard_state.pressed[key/8]|=mask;
    else keyboard_state.pressed[key/8]&=~mask;
    keyboard_state.modifiers=e->dy;keyboard_state.pressed[28]=e->dy;
    keyboard_state.sequence=e->sequence;
}
void game_keyboard_overflow(void) {
    keyboard_state.flags=0;keyboard_reset_pending=true;
    keyboard_resync();
}
void game_keyboard_publish(void) {
    Window *w=wm_focused();
    if(!ready || !focused || identifier(w)!=focused)return;
    Game *g=w->state;
    if(!full_keyboard(g))return;
    if(keyboard_published && !keyboard_reset_pending &&
       keyboard_published_sequence==keyboard_state.sequence)return;
    struct os_window_keyboard q={.version=OS_WINDOW_KEYBOARD_VERSION,.size=sizeof(q),.id=focused};
    if(window_op(OS_WINDOW_SERVER_KEYBOARD_GET,&q,sizeof(q)))return;
    if(keyboard_reset_pending) {
        q.state=keyboard_state;q.state.flags=0;
        if(window_op(OS_WINDOW_SERVER_KEYBOARD_SET,&q,sizeof(q)))return;
    }
    q.state=keyboard_state;
    if(!window_op(OS_WINDOW_SERVER_KEYBOARD_SET,&q,sizeof(q))) {
        keyboard_reset_pending=false;keyboard_published=true;
        keyboard_published_sequence=keyboard_state.sequence;
    }
}
void game_focus_changed(void) {
    if (!ready)
        return;
    Window *w = wm_focused();
    unsigned id = (!desk.dialog.open && !desk.launcher && !desk.menu.open && w && !w->minimized &&
                   w->workspace == desk.workspace)
                      ? identifier(w)
                      : 0;
    if (capture && capture != w)
        release_capture();
    if (id != focused) {
        keyboard_published=false;
        for(unsigned i=0;i<MAX_WINDOWS;i++) {
            Window *other=&desk.windows[i];
            if(other->alive && native_window_app(other->app) && other->state) {
                Game *g=other->state;g->pointer_entered=g->pointer_grab=false;g->pointer_buttons=0;
            }
        }
        struct os_window_focus f = {id, id != 0};
        window_op(OS_WINDOW_SERVER_FOCUS, &f, sizeof(f));
        focused = id;
    }
    game_keyboard_publish();
}
void game_service_start(void) {
    ready = window_op(OS_WINDOW_SERVER_START, NULL, 0) == 0;
    focused = 0;
    capture = NULL;
    memset(&keyboard_state,0,sizeof(keyboard_state));
    keyboard_skip=keyboard_current=0;keyboard_reset_pending=true;
    keyboard_published=false;
    keyboard_resync();
}
static void reap(void) {
    for (unsigned i = 0; i < retired_count;) {
        drain(retired[i].output);
        int status;
        long r = call3(SYS_wait4, retired[i].pid, &status, 1);
        if (r == retired[i].pid || r == -E_CHILD) {
            drain(retired[i].output);
            if (desk.trace)
                printf("DESKTOP GAME_EXIT pid=%d status=%d reaped=%d\n", retired[i].pid,
                       r == -E_CHILD ? -1 : status, r == retired[i].pid);
            if (retired[i].output >= 0)
                close(retired[i].output);
            retired[i] = retired[--retired_count];
            continue;
        }
        if (now_ms() - retired[i].at > retired[i].grace_ms + 2000)
            call2(SYS_kill, retired[i].pid, 9);
        else if (now_ms() - retired[i].at > retired[i].grace_ms)
            call2(SYS_kill, retired[i].pid, 15);
        i++;
    }
}
void game_service_tick(void) {
    if (!ready)
        return;
    reap();
    keyboard_resync();
    game_focus_changed();
    for (unsigned n = 0; n < OS_WINDOW_SLOTS; n++) {
        struct os_window_info info;
        long e = window_op(OS_WINDOW_SERVER_NEXT, &info, sizeof(info));
        if (e)
            break;
        /* A launcher can publish its first surface after its loading window
           was closed. Close that owned surface instead of reopening the app. */
        bool cancelled = false;
        for (unsigned i = 0; i < retired_count; i++) {
            if (retired[i].pid != (int)info.client)
                continue;
            struct os_window_token token = {info.id, 0};
            window_op(OS_WINDOW_SERVER_CLOSE, &token, sizeof(token));
            if (desk.trace)
                printf("DESKTOP NATIVE_START_CANCELLED client=%u id=%u\n", info.client, info.id);
            cancelled = true;
            break;
        }
        if (cancelled)
            continue;
        Window *found = NULL;
        for (unsigned i = 0; i < MAX_WINDOWS; i++) {
            Window *w = &desk.windows[i];
            if (w->alive && native_window_app(w->app) && w->state &&
                ((Game *)w->state)->pid == (int)info.client && !((Game *)w->state)->info.id) {
                found = w;
                break;
            }
        }
        if (found) {
            Game *g = found->state;
            g->info = info;
            g->info.sequence = 0; /* Claiming metadata does not consume pixels. */
            g->pixel_bytes=pixel_capacity(g);
            g->pixels = allocate(g->pixel_bytes);
            if (!g->pixels) {
                wm_close(found);
                continue;
            }
            copy_text(found->title, sizeof(found->title), info.title);
            /* Claiming the client replaces the launch placeholder's geometry.
               Repaint its old frame and shadow before fitting the real
               surface, or the old outline remains behind Electron windows. */
            damage_window(found);
            fit_native_window(found,g);
            invalidate_window(found);
            game_focus_changed();
        } else {
            game_pending = info;
            char id[32];
            format_uint(id, info.id);
            App app = info.version == OS_WINDOW_PIXEL_CLIENT_VERSION &&
                      !strcmp(info.title, "Coast - Code OSS") ? APP_WORKBENCH :
                      !strcmp(info.title,"Chromium Browser")?APP_CHROMIUM:
                      !strcmp(info.title,"Qt 6 Demo")?APP_QT:APP_GAME;
            if (!wm_open(app, id)) {
                struct os_window_token t = {info.id, 0};
                window_op(OS_WINDOW_SERVER_CLOSE, &t, sizeof(t));
            }
        }
    }
    for(unsigned n=0;n<OS_WINDOW_SLOTS;n++) {
        struct os_window_request request;
        if(window_op(OS_WINDOW_SERVER_REQUEST,&request,sizeof(request)))break;
        for(unsigned i=0;i<MAX_WINDOWS;i++) {
            Window *w=&desk.windows[i];Game *g=w->state;
            if(!w->alive || !native_window_app(w->app) || !g || !native_pixels(g) || g->info.id!=request.id)continue;
            if(request.action==OS_WINDOW_REQUEST_MINIMIZE)wm_minimize(w);
            else if(request.action==OS_WINDOW_REQUEST_MAXIMIZE) {
                if(w->minimized)wm_focus(w);
                /* Ozone sends unset_fullscreen then set_maximized. The
                   coalesced final request must also leave fullscreen. */
                if(w->fullscreen)wm_fullscreen(w,false);
                if(!w->maximized)wm_maximize(w);
            } else if(request.action==OS_WINDOW_REQUEST_RESTORE) {
                if(w->minimized)wm_focus(w);
                if(w->fullscreen)wm_fullscreen(w,false);
                if(w->maximized)wm_maximize(w);
            } else if(request.action==OS_WINDOW_REQUEST_FULLSCREEN) {
                if(w->minimized)wm_focus(w);
                wm_fullscreen(w,true);
            } else if(request.action==OS_WINDOW_REQUEST_UNFULLSCREEN) {
                wm_fullscreen(w,false);
            }
            break;
        }
    }
    for(unsigned i=0;i<MAX_WINDOWS;i++) {
        Window *w=&desk.windows[i];
        if(w->alive && native_window_app(w->app) && w->state) {
            Game *g=w->state;configure_native_window(w,g);
            if(native_pixels(g)) {
                struct os_window_cursor cursor={.id=g->info.id,.pixels=(uintptr_t)g->cursor_pixels,.sequence=g->cursor.sequence};
                if(!window_op(OS_WINDOW_SERVER_CURSOR,&cursor,sizeof(cursor)) && cursor.sequence!=g->cursor.sequence) {
                    g->cursor=cursor;
                    desk.cursor_dirty=true;
                    if(desk.trace)printf("DESKTOP NATIVE_CURSOR window=%d mode=%u width=%u height=%u hotspot=%d,%d mouse=%d,%d sequence=%lu\n",
                        w->id,cursor.mode,cursor.width,cursor.height,cursor.hotspot_x,cursor.hotspot_y,desk.mouse_x,desk.mouse_y,cursor.sequence);
                }
            }
        }
    }
    Window *current=wm_focused();
    if(current && native_window_app(current->app))pixel_pointer(current,0);
}
static Game *pointer_cursor(void) {
    Window *w=wm_focused();
    if(!w || !native_window_app(w->app) || !w->state || desk.dialog.open || desk.menu.open || desk.snap_open || desk.snap_assist || desk.snap_pointer_blocked || desk.launcher || desk.drag)return NULL;
    Game *g=w->state;
    if(!native_pixels(g) || !g->painted || !g->pointer_entered || (!g->pointer_grab && !wm_pointer_active(w)))return NULL;
    return g;
}
bool game_cursor_bounds(Rect *bounds) {
    Game *g=pointer_cursor();
    if(!g || g->cursor.mode==OS_WINDOW_CURSOR_DEFAULT)return false;
    *bounds=g->cursor.mode==OS_WINDOW_CURSOR_HIDDEN?(Rect){0}:
        (Rect){desk.mouse_x-g->cursor.hotspot_x,desk.mouse_y-g->cursor.hotspot_y,
               g->cursor.width,g->cursor.height};
    return true;
}
bool game_cursor_draw(void) {
    Game *g=pointer_cursor();
    if(!g)return false;
    if(g->cursor.mode==OS_WINDOW_CURSOR_HIDDEN)return true;
    if(g->cursor.mode!=OS_WINDOW_CURSOR_BITMAP)return false;
    for(unsigned y=0;y<g->cursor.height;y++)for(unsigned x=0;x<g->cursor.width;x++) {
        int px=desk.mouse_x-g->cursor.hotspot_x+x,py=desk.mouse_y-g->cursor.hotspot_y+y;
        if(px<0 || py<0 || px>=desk.width || py>=desk.height)continue;
        uint32_t source=g->cursor_pixels[(size_t)y*g->cursor.width+x];
        uint32_t *target=desk.output+(size_t)py*desk.width+px;
        unsigned back=255-(source>>24),red=((source>>16)&255)+(((*target>>16)&255)*back+127)/255;
        unsigned green=((source>>8)&255)+(((*target>>8)&255)*back+127)/255;
        unsigned blue=(source&255)+((*target&255)*back+127)/255;
        *target=(red<<16)|(green<<8)|blue;
    }
    return true;
}
int game_open(Window *w, const char *path) {
    /* The current Electron user namespace is pinned to the owner's UID.
       Refuse other profiles rather than exposing the owner's private home. */
    if((w->app==APP_WORKBENCH || w->app==APP_CHROMIUM) && profiles_uid(profiles_active())!=OS_ACCOUNT_FIRST)return -E_PERM;
    if (!ready)
        return -E_NOTSUP;
    Game *g = allocate(sizeof(*g));
    if (!g)
        return -E_NOMEM;
    memset(g, 0, sizeof(*g));
    g->output = -1;
    w->state = g;
    if (path) {
        unsigned id = number(path, 10);
        g->info = (struct os_window_info){.id = id}; // next-info was claimed already; dimensions
                                                     // arrive through bind below
        // The service passes the claimed descriptor via this process-local
        // snapshot.
        if (game_pending.id != id) {
            deallocate(g, sizeof(*g));
            w->state = NULL;
            return -E_INVAL;
        }
        g->info = game_pending;
        g->info.sequence = 0;
        g->pixel_bytes=pixel_capacity(g);
        g->pixels = allocate(g->pixel_bytes);
        if (!g->pixels) {
            deallocate(g->pixels,g->pixel_bytes);
            deallocate(g, sizeof(*g));
            w->state = NULL;
            return -E_NOMEM;
        }
        copy_text(w->title, sizeof(w->title), g->info.title);
        fit_native_window(w,g);
        return 0;
    }
    if (retired_count >= 64 - MAX_WINDOWS) {
        deallocate(g, sizeof(*g));
        w->state = NULL;
        return -E_AGAIN;
    }
    int fds[2] = {-1, -1};
    int e = call2(SYS_pipe2, fds, O_CLOEXEC);
    int null = open("/dev/null", O_RDWR, 0);
    if (e || null < 0) {
        if (fds[0] >= 0)
            close(fds[0]);
        if (fds[1] >= 0)
            close(fds[1]);
        if (null >= 0)
            close(null);
        deallocate(g, sizeof(*g));
        w->state = NULL;
        return e ? e : null;
    }
    bool workbench = w->app == APP_WORKBENCH;
    bool browser = w->app == APP_CHROMIUM;
    char uid[24],scale[24];format_uint(uid,profiles_uid(profiles_active()));format_uint(scale,desk.scale);
    char *qt_args[]={"/bin/profile-session",uid,"qt6",desk.home,scale,NULL};
    char *args[] = {browser ? "/bin/chromium-wayland" : workbench ? "/bin/codeoss-wayland" : "/bin/doom",
                    browser ? "--browser" : workbench ? "--codeoss" : "--windowed", NULL};
    struct os_spawn_io spec = {.fd = {null, fds[1], fds[1]}, .cwd = (uintptr_t)desk.home};
    unsigned file_scope=0;
    if(workbench || browser)file_scope=desktop_file_scope(0);
    g->pid = call3(SYS_spawn_io,w->app==APP_QT?qt_args[0]:args[0],w->app==APP_QT?qt_args:args,&spec);
    if(workbench || browser)desktop_file_scope(file_scope);
    close(null);
    close(fds[1]);
    if (g->pid < 0) {
        e = g->pid;
        close(fds[0]);
        deallocate(g, sizeof(*g));
        w->state = NULL;
        return e;
    }
    g->output = fds[0];
    call3(SYS_fcntl, g->output, F_SETFL, O_NONBLOCK);
    trace("GAME_PROCESS", w, w->app==APP_QT?"/usr/bin/qt6-demo":browser ? "/bin/chromium-wayland --browser" : workbench ? "/bin/codeoss-wayland --codeoss" : "/bin/doom --windowed");
    return 0;
}
void game_draw(Window *w) {
    Game *g = w->state;
    Rect b = window_body(w);
    if (!g || !g->painted) {
        fill(b, 0x11161b);
        label(b.x + S(24), b.y + S(28),
              w->app==APP_QT?"Starting Qt 6...":w->app == APP_CHROMIUM ? "Starting Chromium Browser..." : w->app == APP_WORKBENCH ? "Starting Code OSS..." : "Starting Doom...",
              FONT_TITLE, 0xe0e6ea, b.w - S(48));
        return;
    }
    if (native_pixels(g)) {
        /* Copy client pixels at 1:1. No workbench text, icons or cursor
           coordinates are scaled, even while a configure is outstanding. */
        Rect view={b.x,b.y,(int)g->info.width,(int)g->info.height};
        if(memcmp(&g->view,&view,sizeof(view))) {
            g->view=view;
            if(desk.trace)printf("DESKTOP GAME_VIEW window=%d x=%d y=%d width=%d height=%d source=%ux%u native=1 unscaled=1\n",
                                w->id,view.x,view.y,view.w,view.h,g->info.width,g->info.height);
        }
        Rect area=intersect(b,desk.clip);
        for(int y=area.y;y<area.y+area.h;y++) {
            unsigned sy=MIN((unsigned)(y-b.y),g->info.height-1);
            const uint32_t *row=g->pixels+(size_t)sy*g->info.width;
            uint32_t *out=paint_pixel(area.x,y);
            int x=area.x-b.x,n=clampi((int)g->info.width-x,0,area.w);
            if(n)memcpy(out,row+x,(size_t)n*4);
            /* Newly exposed slivers inherit the client's own edge color,
               rather than a foreign blank canvas. Existing client pixels
               never move or scale. The next frame replaces these slivers. */
            uint32_t edge=row[g->info.width-1];
            for(int i=n;i<area.w;i++)out[i]=edge;
        }
        return;
    }
    fill(b, 0x11161b);
    int height = b.h - S(27), width = height * 4 / 3;
    if (width > b.w) {
        width = b.w;
        height = width * 3 / 4;
    }
    Rect view = {b.x + (b.w - width) / 2, b.y + (b.h - S(27) - height) / 2, width, height};
    if (memcmp(&g->view, &view, sizeof(view))) {
        g->view = view;
        if (desk.trace)
            printf("DESKTOP GAME_VIEW window=%d x=%d y=%d width=%d height=%d body_x=%d body_y=%d "
                   "body_w=%d body_h=%d source=320x200\n",
                   w->id, view.x, view.y, view.w, view.h, b.x, b.y, b.w, b.h);
    }

    Rect area = intersect(view, desk.clip);
    /* Exact nearest-neighbor positions with no division in the pixel loop.
       Large software viewports otherwise spend most repaint time dividing. */
    unsigned step = g->info.width / view.w, delta = g->info.width % view.w;
    uint64_t start = (uint64_t)(area.x - view.x) * g->info.width;
    unsigned first = start / view.w, fraction = start % view.w;
    for (int y = area.y; y < area.y + area.h; y++) {
        uint32_t *out = paint_pixel(area.x, y);
        unsigned sy = (uint64_t)(y - view.y) * g->info.height / view.h;
        const uint32_t *row = g->pixels + (size_t)sy * g->info.width;
        unsigned sx = first, remainder = fraction;
        for (int x = area.x; x < area.x + area.w; x++) {
            *out++ = row[sx];
            sx += step;
            remainder += delta;
            if (remainder >= (unsigned)view.w) {
                remainder -= view.w;
                sx++;
            }
        }
    }
    label(b.x + S(10), b.y + b.h - S(21),
          capture == w ? "Pointer captured · F10 releases · Alt+F4 closes"
                       : "Click game to capture pointer · F10 releases · Alt+F4 closes",
          FONT_SMALL, 0xc6cfd6, b.w - S(20));
}
bool game_thumbnail(Window *w,Rect r) {
    Game *g=w->state;
    if((w->app!=APP_WORKBENCH && w->app!=APP_QT && w->app!=APP_CHROMIUM) || !g || !g->painted || !native_pixels(g))return false;
    if(empty(r))return true;
    int width=r.w,height=(int64_t)width*g->info.height/g->info.width;
    if(height>r.h){height=r.h;width=(int64_t)height*g->info.width/g->info.height;}
    if(width<=0 || height<=0)return true;
    Rect dst={r.x+(r.w-width)/2,r.y+(r.h-height)/2,width,height};
    Rect area=intersect(dst,desk.clip);
    for(int y=area.y;y<area.y+area.h;y++) {
        unsigned sy=(uint64_t)(y-dst.y)*g->info.height/height;
        uint32_t *out=paint_pixel(area.x,y);
        for(int x=area.x;x<area.x+area.w;x++)
            *out++=g->pixels[(size_t)sy*g->info.width+(uint64_t)(x-dst.x)*g->info.width/width];
    }
    return true;
}
static void send(Window *w, struct os_window_event event) {
    Game *g = w->state;
    if (!g || !g->info.id)
        return;
    struct os_window_events request = {g->info.id, 1, (uintptr_t)&event};
    window_op(OS_WINDOW_SERVER_SEND, &request, sizeof(request));
}
bool game_mouse(Window *w, int x, int y, unsigned pressed, int wheel) {
    Game *g=w->state;
    if(g && native_pixels(g)){pixel_pointer(w,wheel);return true;}
    (void)x;
    (void)y;
    (void)wheel;
    if (pressed & 1) {
        capture = w;
        invalidate_all();
        trace("GAME_CAPTURE", w, "active");
        send(w, (struct os_window_event){.type = OS_WINDOW_POINTER, .buttons = desk.buttons});
    }
    return true;
}
bool game_pointer_captured(void) {
    if (capture && (!capture->alive || capture->minimized || capture->workspace != desk.workspace ||
                    desk.dialog.open || desk.launcher || desk.menu.open || wm_focused() != capture))
        release_capture();
    return capture != NULL;
}
bool game_pointer_event(const struct os_input_event *event) {
    if (!game_pointer_captured())
        return false;
    desk.buttons = event->buttons;
    send(capture, (struct os_window_event){.type = OS_WINDOW_POINTER,
                                           .dx = event->dx,
                                           .dy = event->dy,
                                           .buttons = event->buttons,
                                           .wheel = event->wheel});
    return true;
}
void game_key(Window *w, Key k) {
    Game *g=w->state;
    if ((!g || !native_pixels(g)) && k.down && (k.code == 0x43 || ((k.mods & MOD_CTRL) && (k.mods & MOD_ALT)))) {
        release_capture();
        struct os_window_focus off = {0, 0};
        window_op(OS_WINDOW_SERVER_FOCUS, &off, sizeof(off));
        focused = 0;
        game_focus_changed();
        return;
    }
    if (k.repeat)
        return;
    if(g && full_keyboard(g)) {
        if(!k.code || keyboard_current<=keyboard_skip)return;
        game_focus_changed();
    }
    send(w, (struct os_window_event){.type = k.code ? OS_WINDOW_HID_KEY : OS_WINDOW_KEY,
                                     .code = k.code ? k.code : k.ascii,
                                     .dx = k.ascii,
                                     .dy = k.mods,
                                     .buttons = k.code ? k.down : 0});
}
void game_tick(Window *w) {
    Game *g = w->state;
    drain(g->output);
    if (g->pid > 0) {
        int status;
        long r = call3(SYS_wait4, g->pid, &status, 1);
        if (r == g->pid) {
            drain(g->output);
            if (desk.trace)
                printf("DESKTOP GAME_EXIT pid=%d status=%d reaped=1\n", g->pid, status);
            g->pid = 0;
            wm_close(w);
            return;
        }
    }
    if (!g->info.id)
        return;
    struct os_window_geometry geometry={.id=g->info.id};
    unsigned width=g->info.width,height=g->info.height;
    if(native_pixels(g) && !window_op(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))) {
        width=geometry.width;height=geometry.height;
    }
    bool changed=width!=g->info.width || height!=g->info.height;
    bool incremental=native_pixels(g) && g->painted && !changed;
    struct os_window_damage_frame request={.frame={g->info.id,width,height,width,
        (uintptr_t)g->pixels,changed?0:g->info.sequence}};
    struct os_window_frame *frame=&request.frame;
    long e = native_pixels(g)?window_op(OS_WINDOW_SERVER_READ_DAMAGE,&request,sizeof(request)):
                              window_op(OS_WINDOW_SERVER_READ,frame,sizeof(*frame));
    if (!e) {
        PixelDamage damage=native_pixels(g)?(PixelDamage){request.x,request.y,request.width,request.height}:
                                             (PixelDamage){0,0,width,height};
        g->copied_pixels+=(uint64_t)damage.width*damage.height;
        if(native_pixels(g) && changed && desk.trace)
            printf("DESKTOP NATIVE_LAYOUT window=%d width=%u height=%u latency_ms=%lu ms=%lu\n",
                w->id,width,height,now_ms()-g->configured_at,now_ms());
        g->info.width=width;g->info.height=height;
        g->info.sequence = frame->sequence;
        g->painted = true;
        if (native_pixels(g) && changed) configure_native_window(w,g);
        g->frames++;
        g->changed_pixels+=(uint64_t)damage.width*damage.height;
        if (!damage.width || !damage.height) g->unchanged_frames++;
        else if (!incremental) invalidate_window(w);
        else if (!w->minimized && w->workspace==desk.workspace) {
            /* Cached pixels move without repainting. Damage and pointer
               coordinates must follow current geometry even before draw has
               updated the diagnostic view rectangle. */
            Rect body=window_body(w);
            int right=damage.x+damage.width>=width?body.w:(int)(damage.x+damage.width);
            int bottom=damage.y+damage.height>=height?body.h:(int)(damage.y+damage.height);
            invalidate_window_region(w,intersect(body,(Rect){body.x+(int)damage.x,
                body.y+(int)damage.y,right-(int)damage.x,bottom-(int)damage.y}));
        }
        if (desk.trace && (frame->sequence == 1 || frame->sequence % 35 == 0)) {
            printf("DESKTOP GAME_FRAME window=%d client=%u sequence=%lu width=%u "
                   "height=%u\n",
                   w->id, g->info.client, frame->sequence, frame->width, frame->height);
        }
    } else if (e == -E_NOENT || e == -E_PIPE || e == -E_NODEV)
        wm_close(w);
}
bool game_close(Window *w) {
    Game *g = w->state;
    if (!g)
        return true;
    if (capture == w)
        release_capture();
    if (g->info.id) {
        struct os_window_token token = {g->info.id, 0};
        window_op(OS_WINDOW_SERVER_CLOSE, &token, sizeof(token));
    }
    if (g->pid > 0 && retired_count < 64) {
        retired[retired_count].pid = g->pid;
        retired[retired_count].output = g->output;
        /* The bridge allows Electron ten seconds to close its workbench and
           reap helpers. Keep draining its pipe throughout that grace period. */
        retired[retired_count].grace_ms = (w->app == APP_WORKBENCH || w->app == APP_CHROMIUM) ? 12000 : 2000;
        retired[retired_count++].at = now_ms();
        g->output = -1;
    }
    if (g->output >= 0)
        close(g->output);
    if (g->pixels)
        deallocate(g->pixels,g->pixel_bytes);
    if (desk.trace && native_pixels(g))
        printf("DESKTOP NATIVE_DAMAGE window=%d frames=%lu changed_pixels=%lu unchanged_frames=%lu copied_pixels=%lu\n",
               w->id,g->frames,g->changed_pixels,g->unchanged_frames,g->copied_pixels);
    deallocate(g, sizeof(*g));
    w->state = NULL;
    return true;
}
void game_service_shutdown(void) {
    release_capture();
    uint64_t until = now_ms() + 14500;
    while (retired_count && now_ms() < until) {
        reap();
        struct os_timespec delay = {0, 10000000};
        call2(SYS_nanosleep, &delay, 0);
    }
    ready = false;
}
