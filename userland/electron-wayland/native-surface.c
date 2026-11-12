#define _GNU_SOURCE
#include "config.h"
#include <wayland-server.h>
#include "fullscreen-shell-unstable-v1-server-protocol.h"
#include "xdg-shell-server-protocol.h"
#include "xdg-state.h"
#include <uapi/window.h>
#include <uapi/abi.h>
#include <uapi/clipboard.h>
#include <uapi/account.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "pixel-convert.h"
#include "surface-damage.h"

/* Bounded private Wayland output with one application surface and one cursor.
   Keyboard/pointer input belongs to its owned native window. No named display
   socket or global input/device authority is exposed to the client. */
#ifdef CODEOSS_WORKBENCH
enum { WIDTH=2400,HEIGHT=1300,LOGICAL_WIDTH=1200,LOGICAL_HEIGHT=650 };
#else
enum { WIDTH=800,HEIGHT=600,LOGICAL_WIDTH=400,LOGICAL_HEIGHT=300 };
#endif
enum { MAX_WIDTH=OS_WINDOW_PIXEL_MAX_WIDTH,MAX_HEIGHT=OS_WINDOW_PIXEL_MAX_HEIGHT,
       REGION_WIDTH=MAX_WIDTH+128,REGION_HEIGHT=MAX_HEIGHT+128,
       CALLBACK_LIMIT=8,REGION_BYTES=REGION_WIDTH*REGION_HEIGHT/8,REGION_LIMIT=8,OUTPUT_LIMIT=4 };
struct output;
struct clipboard_state;
struct frame_callback { struct wl_list link; struct wl_resource *resource; struct output *output; int committed; };
struct output {
    int width,height,scale,buffer_scale,pending_scale,display_width,display_height,pixel_width,pixel_height;
    int native_configure_dirty;
    unsigned native_states;
    unsigned native_action;
    struct wl_display *display; struct wl_client *client; struct wl_resource *surface,*cursor_surface,*pending;
    struct wl_listener pending_destroy,client_destroy; struct wl_list callbacks;
    struct os_window_info window; uint32_t pixels[MAX_WIDTH*MAX_HEIGHT];
    struct xdg_state xdg;
    struct clipboard_state *clipboard;
    struct {struct wl_resource *resource;int entered;} outputs[OUTPUT_LIMIT];
    unsigned region_count;
    uint8_t input_current[REGION_BYTES],input_pending[REGION_BYTES];
    uint8_t opaque_current[REGION_BYTES],opaque_pending[REGION_BYTES];
    int input_changed,opaque_changed;
    struct wl_resource *keyboards[4];
    struct wl_resource *pointers[4];
    uint32_t pointer_enter_serial[4];
    struct wl_resource *cursor_pending;
    struct wl_listener cursor_pending_destroy;
    uint32_t cursor_pixels[OS_WINDOW_CURSOR_MAX*OS_WINDOW_CURSOR_MAX];
    unsigned cursor_width,cursor_height,cursor_mode;
    int cursor_scale,cursor_pending_scale,cursor_hot_x,cursor_hot_y;
    wl_fixed_t pointer_x,pointer_y;
    unsigned pointer_buttons;
    int pointer_focused,surface_left,surface_top;
    struct os_keyboard_snapshot key_state;
    uint64_t key_generation,key_watermark;
    unsigned seat_count;
    int key_focused,keymap_fd,input_trace;
    unsigned stall_ms;uint64_t stall_started;int stall_ended;
    uint64_t copy_max_ms,present_max_ms,dispatch_max_ms;unsigned timing_logs;
    unsigned frames,releases,repaints,callback_count,callbacks_done,ignored_input,limit;
    struct surface_damage damage;
    int source_width,source_height,source_scale;
    uint32_t source_format;
    uint64_t scanned_pixels,presented_pixels,possible_pixels,unchanged_frames;
    int attached,lost,selected,fatal,hold,closed,role,surface_has_buffer,close_fallback,checker,electron,workbench,browser;
    uint64_t first_ms,next_frame_us;
    uint64_t display_polled_at;
};
static volatile sig_atomic_t stopping;
static int frame_limit(const char *p,unsigned *out) {
    if(!*p)return -1;
    unsigned n=0;
    while(*p){if(*p<'0' || *p>'9' || n>1000)return -1;n=n*10+(*p++-'0');}
    if(n>10000)return -1;
    *out=n;return 0;
}
static void stop_signal(int sig) { (void)sig;stopping=1; }
static uint64_t now_us(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
static uint64_t now_ms(void) { return now_us()/1000; }
static long native(unsigned op,void *p,size_t n) { return syscall(OS_WINDOW_SYSCALL,op,p,n); }
/* Chromium's ARGB surface may retain transparent, rounded client corners even
   though Coast supplies the only window frame. Flatten that small corner over
   the adjacent client color, instead of showing the bridge's dark fallback
   as a second rounded border inside the native window. */
enum { CLIENT_CORNER=24 };
static uint32_t corner_color(const uint32_t *row,int width,int sample,int direction) {
    if(sample<0)sample=0;
    if(sample>=width)sample=width-1;
    for(int i=0,x=sample;i<64 && x>=0 && x<width;i++,x+=direction)
        if((row[x]>>24)==255)return row[x]&0xffffff;
    return 0x18202a;
}
static uint32_t corner_over(uint32_t src,uint32_t bg) {
    unsigned a=src>>24,back=255-a;
    unsigned r=((src>>16)&255)+(((bg>>16)&255)*back+127)/255;
    unsigned g=((src>>8)&255)+(((bg>>8)&255)*back+127)/255;
    unsigned b=(src&255)+((bg&255)*back+127)/255;
    return (r>255?255:r)<<16 | (g>255?255:g)<<8 | (b>255?255:b);
}
static void fail(struct wl_resource *r,const char *why) { wl_resource_post_error(r,WL_DISPLAY_ERROR_IMPLEMENTATION,"P6 diagnostic output: %s",why); }
static void pending_unlink(struct output *o) {
    if(o->pending) wl_list_remove(&o->pending_destroy.link);
    wl_list_init(&o->pending_destroy.link);o->pending=NULL;
}
static void pending_destroyed(struct wl_listener *l,void *unused) {
    (void)unused;struct output *o=wl_container_of(l,o,pending_destroy);
    wl_list_remove(&l->link);wl_list_init(&l->link);o->pending=NULL;o->lost=1;
}
static void client_destroyed(struct wl_listener *l,void *unused) {
    (void)unused;struct output *o=wl_container_of(l,o,client_destroy);
    wl_list_remove(&l->link);wl_list_init(&l->link);o->client=NULL;
}
static void callback_destroy(struct wl_resource *r) {
    struct frame_callback *f=wl_resource_get_user_data(r);
    wl_list_remove(&f->link);f->output->callback_count--;free(f);
}
static void keyboard_leave(struct output *);
static void xdg_focus_configure(struct output *);
static int native_geometry(struct output *);
static void pointer_leave(struct output *);
static void clipboard_note_serial(struct output *,uint32_t);
static void clipboard_focus_lost(struct output *);
static void output_forget_surface(struct output *);
#include "cursor-surface.inc"
static void surface_destroyed(struct wl_resource *r) {
    struct output *owner=wl_resource_get_user_data(r);
    if(r==owner->cursor_surface){cursor_unlink(owner);owner->cursor_surface=NULL;owner->cursor_mode=OS_WINDOW_CURSOR_DEFAULT;cursor_apply(owner);return;}
    struct output *o=wl_resource_get_user_data(r);keyboard_leave(o);output_forget_surface(o);pending_unlink(o);o->surface=NULL;
    struct frame_callback *f,*next;
    wl_list_for_each_safe(f,next,&o->callbacks,link)wl_resource_destroy(f->resource);
    o->role=o->selected=o->surface_has_buffer=0;o->attached=o->lost=0;
    o->damage=(struct surface_damage){0};
}
static void destroy_resource(struct wl_client *c,struct wl_resource *r) { (void)c;wl_resource_destroy(r); }
#include "pointer-seat.inc"
#include "keyboard-seat.inc"
#include "data-device.inc"
#include "output-region.inc"
static void destroy_surface_request(struct wl_client *c,struct wl_resource *r) {
    (void)c;struct output *o=wl_resource_get_user_data(r);
    if(r==o->cursor_surface){wl_resource_destroy(r);return;}
    if(o->xdg.surface){wl_resource_post_error(o->xdg.surface,XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,"destroy xdg role and surface first");return;}
    wl_resource_destroy(r);
}
static void attach(struct wl_client *c,struct wl_resource *r,struct wl_resource *buffer,int32_t x,int32_t y) {
    (void)c;struct output *o=wl_resource_get_user_data(r);
    if(r==o->cursor_surface) {
        struct wl_shm_buffer *shm=buffer?wl_shm_buffer_get(buffer):NULL;
        if(x || y || (buffer && (!shm || wl_shm_buffer_get_width(shm)<1 || wl_shm_buffer_get_height(shm)<1 ||
            wl_shm_buffer_get_width(shm)>(int)OS_WINDOW_CURSOR_MAX || wl_shm_buffer_get_height(shm)>(int)OS_WINDOW_CURSOR_MAX))) {
            fail(r,"cursor buffer exceeds bounds");return;
        }
        cursor_unlink(o);o->cursor_pending=buffer;
        if(buffer)wl_resource_add_destroy_listener(buffer,&o->cursor_pending_destroy);
        else {o->cursor_width=o->cursor_height=0;}
        return;
    }
    if(buffer && o->xdg.surface && !o->xdg.initial){wl_resource_post_error(o->xdg.surface,XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,"wait for initial configure before attach");return;}
    if(x || y) {fail(r,"nonzero attach offset unsupported");return;}
    pending_unlink(o);o->pending=buffer;o->attached=1;o->lost=0;
    if(buffer)wl_resource_add_destroy_listener(buffer,&o->pending_destroy);
}
static void damage(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t width,int32_t height) {
    (void)c;struct output *o=wl_resource_get_user_data(r);
    if(r!=o->cursor_surface)surface_damage_add(&o->damage,x,y,width,height);
}
static void frame(struct wl_client *c,struct wl_resource *r,uint32_t id) {
    struct output *o=wl_resource_get_user_data(r);
    if(r==o->cursor_surface) {
        struct wl_resource *callback=wl_resource_create(c,&wl_callback_interface,1,id);
        if(!callback){wl_client_post_no_memory(c);return;}
        wl_callback_send_done(callback,(uint32_t)now_ms());wl_resource_destroy(callback);return;
    }
    if(o->callback_count==CALLBACK_LIMIT) {fail(r,"frame callback limit");return;}
    struct frame_callback *f=calloc(1,sizeof(*f));if(!f){wl_client_post_no_memory(c);return;}
    f->resource=wl_resource_create(c,&wl_callback_interface,1,id);
    if(!f->resource){free(f);wl_client_post_no_memory(c);return;}
    f->output=o;wl_list_insert(o->callbacks.prev,&f->link);o->callback_count++;
    wl_resource_set_implementation(f->resource,NULL,f,callback_destroy);
}
#include "xdg-toplevel.inc"
static void commit(struct wl_client *c,struct wl_resource *r) {
    (void)c;struct output *o=wl_resource_get_user_data(r);
    if(r==o->cursor_surface){cursor_commit(o,r);return;}
    struct surface_damage pending_damage=o->damage;
    o->damage=(struct surface_damage){0};
    if(o->lost){fail(r,"destroyed pending buffer before commit unsupported");return;}
    regions_commit(o);
    if(xdg_commit(o))return;
    o->buffer_scale=o->pending_scale;
    if(native_geometry(o)<0)return;
    if(!o->selected){fail(r,"fullscreen presentation required");return;}
    if(o->attached) {
        struct wl_resource *buffer=o->pending;struct wl_shm_buffer *shm=buffer?wl_shm_buffer_get(buffer):NULL;
        if(buffer && !shm){fail(r,"only real wl_shm buffers supported");return;}
        int width=shm?wl_shm_buffer_get_width(shm):0,height=shm?wl_shm_buffer_get_height(shm):0;
        int stride=shm?wl_shm_buffer_get_stride(shm):0;uint32_t format=shm?wl_shm_buffer_get_format(shm):0;
        if(o->electron && buffer && !o->frames)printf("ELECTRON_BUFFER width=%d height=%d stride=%d format=%u\n",width,height,stride,format);
        /* Chromium shadows are in surface coordinates, as is xdg geometry.
           Copy physical pixels, cropping only the bounded shadow margins. */
        /* Old in-flight buffers are valid, but a resize must not publish
           letterboxed old content as if it were a complete new-size frame. */
        int max_width=(o->electron?MAX_WIDTH:o->width)+(o->electron?64*o->buffer_scale:0);
        int max_height=(o->electron?MAX_HEIGHT:o->height)+(o->electron?64*o->buffer_scale:0);
        if(buffer && o->electron && o->frames && o->xdg.initial &&
           (width>max_width || height>max_height) &&
           width>0 && height>0 && width<=MAX_WIDTH*2+128 && height<=MAX_HEIGHT*2+128 &&
           (int64_t)stride>=(int64_t)width*4 &&
           (format==WL_SHM_FORMAT_XRGB8888 || format==WL_SHM_FORMAT_ARGB8888)) {
            /* A DPI change can leave an old-scale Chromium buffer in flight
               after the new configure is acknowledged. It cannot fit the
               bounded native window. Release it and retain the last valid
               frame so the client can render against the new output scale. */
            printf("NATIVE_RESIZE_RETAIN oversized=%dx%d stride=%d output=%dx%d scale=%d\n",
                   width,height,stride,o->width,o->height,o->scale);
            wl_buffer_send_release(buffer);o->releases++;
            pending_unlink(o);o->attached=0;
            struct frame_callback *callback;
            wl_list_for_each(callback,&o->callbacks,link)callback->committed=1;
            return;
        }
        if(buffer && (width<1 || width>max_width || height<1 || height>max_height || (int64_t)stride<(int64_t)width*4 || stride>(o->electron?max_width*4:4096) ||
            (format!=WL_SHM_FORMAT_XRGB8888 && format!=WL_SHM_FORMAT_ARGB8888))) {
            fprintf(stderr,"P6_BUFFER_REJECT source=%dx%d stride=%d format=%u bound=%dx%d output=%dx%d scale=%d\n",
                    width,height,stride,format,max_width,max_height,o->width,o->height,o->scale);
            fail(r,"buffer exceeds output/format bounds");return;
        }
        if(!o->window.id) {
            o->window=(struct os_window_info){.version=OS_WINDOW_PIXEL_CLIENT_VERSION,.width=o->width,.height=o->height};
            strcpy(o->window.title,o->browser?"Chromium Browser":o->workbench?"Coast - Code OSS":o->electron?"Coast - upstream Electron":o->checker?"Wayland xdg - protocol lifecycle check":"Wayland xdg - upstream Weston simple-shm");
            if(native(OS_WINDOW_CREATE,&o->window,sizeof(o->window))) {
                fprintf(stderr,"P6_NATIVE_CREATE_ERROR errno=%d\n",errno);o->fatal=1;fail(r,"native window unavailable");return;
            }
            printf("P6_NATIVE_CREATED pid=%d id=%u width=%u height=%u\n",getpid(),o->window.id,o->window.width,o->window.height);
            if(o->native_action && o->xdg.toplevel) {
                unsigned action=o->native_action;o->native_action=0;
                native_state_request(o->xdg.toplevel,action);
            }
        }
        if(o->workbench && shm && o->frames && o->xdg.geometry_set &&
           ((int64_t)o->xdg.geometry.w*o->buffer_scale!=o->width ||
            (int64_t)o->xdg.geometry.h*o->buffer_scale!=o->height ||
            o->xdg.geometry.x<0 || o->xdg.geometry.y<0 ||
            ((int64_t)o->xdg.geometry.w+o->xdg.geometry.x)*o->buffer_scale>width ||
            ((int64_t)o->xdg.geometry.h+o->xdg.geometry.y)*o->buffer_scale>height)) {
            /* Keep the last complete native frame. The desktop clips it at 1:1 within
               the moving border until Chromium supplies this configure's
               content. Shadows need not be symmetric: validate the complete
               requested content rectangle inside the bounded source instead
               of assuming twice its top/left margins. Release SHM so rendering
               can continue; never wait with a client buffer held. */
            wl_buffer_send_release(buffer);o->releases++;
            pending_unlink(o);o->attached=0;
            struct frame_callback *callback;
            wl_list_for_each(callback,&o->callbacks,link)callback->committed=1;
            if((o->input_trace || o->browser) && o->timing_logs++<64)
                printf("NATIVE_RESIZE_RETAIN content=%dx%d requested=%dx%d origin=%d,%d source=%dx%d scale=%d\n",
                       o->xdg.geometry.w*o->buffer_scale,o->xdg.geometry.h*o->buffer_scale,o->width,o->height,
                       o->xdg.geometry.x,o->xdg.geometry.y,width,height,o->buffer_scale);
            return;
        }
        uint64_t copy_started=now_ms();
        int left=0,top=0;
        if(shm) {
            left=(o->width-width)/2;top=(o->height-height)/2;
            if(o->xdg.geometry_set) {
                struct window_geometry g=o->xdg.geometry;
                int64_t sx=(int64_t)g.x*o->buffer_scale,sy=(int64_t)g.y*o->buffer_scale;
                int gx=sx<0?0:sx>width?width:sx,gy=sy<0?0:sy>height?height:sy;
                int64_t ex=((int64_t)g.x+g.w)*o->buffer_scale,ey=((int64_t)g.y+g.h)*o->buffer_scale;
                ex=ex<0?0:ex>width?width:ex;ey=ey<0?0:ey>height?height:ey;
                if(ex<=gx || ey<=gy){fail(r,"empty clamped geometry unsupported");return;}
                left=(o->width-(int)(ex-gx))/2-gx;top=(o->height-(int)(ey-gy))/2-gy;
            }
        }
        bool full=!shm || !o->surface_has_buffer || o->pixel_width!=o->width || o->pixel_height!=o->height ||
                  left!=o->surface_left || top!=o->surface_top || width!=o->source_width || height!=o->source_height ||
                  o->buffer_scale!=o->source_scale || format!=o->source_format;
        struct pixel_rect area=full?(struct pixel_rect){0,0,o->width,o->height}:
            surface_damage_pixels(pending_damage,o->buffer_scale,left,top,o->width,o->height);
        struct pixel_rect changed=full?area:(struct pixel_rect){0};
        if(!shm)for(int i=0;i<o->width*o->height;i++)o->pixels[i]=0x18202a;
        if(shm) {
            /* The client's damage describes changes against the current
               surface, even when it alternates between SHM buffers. Compare
               converted pixels once here, then transfer only actual changes
               through the kernel and desktop. Geometry changes repaint all. */
            bool covers=left<=0 && top<=0 && left+width>=o->width && top+height>=o->height;
            if(full && !covers)for(int i=0;i<o->width*o->height;i++)o->pixels[i]=0x18202a;
            int x0=(int)area.x-left,y0=(int)area.y-top;
            int x1=(int)(area.x+area.width)-left,y1=(int)(area.y+area.height)-top;
            if(x0<0)x0=0;
            if(y0<0)y0=0;
            if(x1>width)x1=width;
            if(y1>height)y1=height;
            if(x0<x1 && y0<y1) {
                wl_shm_buffer_begin_access(shm);
                const uint8_t *data=wl_shm_buffer_get_data(shm);
                o->scanned_pixels+=(uint64_t)(x1-x0)*(y1-y0);
                for(int y=y0;y<y1;y++) {
                    const uint32_t *source=(const uint32_t *)(data+(size_t)y*stride);
                    const uint32_t *restrict row=source+x0;
                    uint32_t *restrict target=o->pixels+(top+y)*o->width+left+x0;
                    struct { int x;uint32_t previous; } corners[CLIENT_CORNER*2];
                    unsigned corner_count=0;
                    int left_end=x0,right_start=x1;
                    bool client_corner=o->electron && format==WL_SHM_FORMAT_ARGB8888 &&
                        (top+y<CLIENT_CORNER || top+y>=o->height-CLIENT_CORNER);
                    if(client_corner) {
                        left_end=x1;
                        if(CLIENT_CORNER-left<left_end)left_end=CLIENT_CORNER-left;
                        right_start=o->width-CLIENT_CORNER-left;
                        if(right_start<x0)right_start=x0;
                        if(right_start<left_end)right_start=left_end;
                        if(!full) {
                            for(int x=x0;x<left_end;x++)corners[corner_count++]=(typeof(corners[0])){x,target[x-x0]};
                            for(int x=right_start;x<x1;x++)corners[corner_count++]=(typeof(corners[0])){x,target[x-x0]};
                        }
                    }
                    struct pixel_span span={0};
                    if(full)pixel_convert(target,row,x1-x0,format==WL_SHM_FORMAT_ARGB8888);
                    else span=pixel_convert_damage(target,row,x1-x0,format==WL_SHM_FORMAT_ARGB8888);
                    if(client_corner) {
                        if(x0<left_end) {
                            uint32_t bg=corner_color(source,width,CLIENT_CORNER-left,1);
                            for(int x=x0;x<left_end;x++)if(source[x]>>24!=255)
                                target[x-x0]=corner_over(source[x],bg);
                        }
                        if(right_start<x1) {
                            uint32_t bg=corner_color(source,width,o->width-1-CLIENT_CORNER-left,-1);
                            for(int x=right_start;x<x1;x++)if(source[x]>>24!=255)
                                target[x-x0]=corner_over(source[x],bg);
                        }
                        if(!full)for(unsigned i=0;i<corner_count;i++) {
                            int x=corners[i].x;
                            if(corners[i].previous==target[x-x0])continue;
                            unsigned at=x-x0;
                            if(!span.width)span=(struct pixel_span){at,1};
                            else {
                                unsigned end=span.x+span.width>at+1?span.x+span.width:at+1;
                                if(at<span.x)span.x=at;
                                span.width=end-span.x;
                            }
                        }
                    }
                    if(!full && span.width) {
                        unsigned x=left+x0+span.x,py=top+y,right=x+span.width;
                        if(!changed.width)changed=(struct pixel_rect){x,py,span.width,1};
                        else {
                            unsigned old_right=changed.x+changed.width;
                            if(x<changed.x)changed.x=x;
                            changed.width=(right>old_right?right:old_right)-changed.x;
                            changed.height=py+1-changed.y;
                        }
                    }
                }
                wl_shm_buffer_end_access(shm);
            }
        }
        uint64_t present_started=now_ms(),copy_ms=present_started-copy_started;
        if(copy_ms>o->copy_max_ms)o->copy_max_ms=copy_ms;
        struct os_window_damage_frame f={.frame={.id=o->window.id,.width=o->width,.height=o->height,
            .stride=o->width,.pixels=(uintptr_t)o->pixels},.x=changed.x,.y=changed.y,.width=changed.width,.height=changed.height};
        if(changed.width && native(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f))) {
            o->pixel_width=o->pixel_height=0; /* Force a complete retry after superseded geometry. */
            if(errno==EINVAL && native_geometry(o)>0) {
                /* A newer native request superseded this copied frame. The
                   SHM access ended above; release it and schedule new work. */
                if(buffer){wl_buffer_send_release(buffer);o->releases++;}
                pending_unlink(o);o->attached=0;
                struct frame_callback *callback;
                wl_list_for_each(callback,&o->callbacks,link)callback->committed=1;
                return;
            }
            if(errno==EPIPE || errno==ENOENT || errno==ENODEV)o->closed=1;
            else o->fatal=1;
            return;
        }
        o->pixel_width=o->width;o->pixel_height=o->height;
        o->surface_left=left;o->surface_top=top;
        o->source_width=width;o->source_height=height;o->source_scale=o->buffer_scale;o->source_format=format;
        o->possible_pixels+=(uint64_t)o->width*o->height;
        o->presented_pixels+=(uint64_t)changed.width*changed.height;
        if(!changed.width)o->unchanged_frames++;
        uint64_t present_ms=now_ms()-present_started;
        if(present_ms>o->present_max_ms)o->present_max_ms=present_ms;
        if(o->input_trace && (copy_ms>=40 || present_ms>=40) && o->timing_logs++<64)
            printf("NATIVE_FRAME_TIMING frame=%u copy_ms=%lu present_ms=%lu ms=%lu\n",o->frames,copy_ms,present_ms,now_ms());
        if(buffer){wl_buffer_send_release(buffer);o->releases++;}
        o->surface_has_buffer=buffer!=NULL;if(o->xdg.surface)o->xdg.mapped=buffer!=NULL;
        if(!o->surface_has_buffer)keyboard_leave(o);
        output_surface_state(o,o->surface_has_buffer);
        pending_unlink(o);o->attached=0;o->frames++;
        if(o->frames==1){o->first_ms=now_ms();printf("P6_UPSTREAM_PIXELS_PRESENTED frame=1 source=%dx%d format=%u output=%dx%d\n",width,height,format,o->width,o->height);}
        if(o->electron && buffer && o->frames==1)printf("ELECTRON_CLIENT_PIXELS_PRESENTED source=%dx%d format=%u output=%dx%d scale=%d\n",width,height,format,o->width,o->height,o->buffer_scale);
        if(o->limit && o->frames>=o->limit)stopping=1;
    }
    struct frame_callback *f;
    wl_list_for_each(f,&o->callbacks,link)f->committed=1;
}
static void buffer_scale(struct wl_client *c,struct wl_resource *r,int32_t scale) {
    (void)c;struct output *o=wl_resource_get_user_data(r);
    if(scale<1){wl_resource_post_error(r,WL_SURFACE_ERROR_INVALID_SCALE,"scale must be positive");return;}
    if(scale>2){fail(r,"buffer scale exceeds owned output bounds");return;}
    if(r==o->cursor_surface){o->cursor_pending_scale=scale;return;}
    o->pending_scale=scale;
}
static void buffer_transform(struct wl_client *c,struct wl_resource *r,int32_t transform) {
    (void)c;if(transform!=WL_OUTPUT_TRANSFORM_NORMAL)fail(r,"only normal buffer transform supported");
}
static const struct wl_surface_interface surface_impl={
    .destroy=destroy_surface_request,.attach=attach,.damage=damage,.frame=frame,
    .set_opaque_region=surface_opaque_region,.set_input_region=surface_input_region,.commit=commit,
    .set_buffer_transform=buffer_transform,.set_buffer_scale=buffer_scale
};
static void create_surface(struct wl_client *c,struct wl_resource *r,uint32_t id) {
    struct output *o=wl_resource_get_user_data(r);
    if(o->surface) {
        if(!o->electron || o->cursor_surface){fail(r,"one application plus one cursor surface bound");return;}
        o->cursor_surface=wl_resource_create(c,&wl_surface_interface,wl_resource_get_version(r),id);
        if(!o->cursor_surface){wl_client_post_no_memory(c);return;}
        o->cursor_scale=o->cursor_pending_scale=1;
        o->cursor_pending_destroy.notify=cursor_pending_gone;
        wl_list_init(&o->cursor_pending_destroy.link);
        wl_resource_set_implementation(o->cursor_surface,&surface_impl,o,surface_destroyed);return;
    }
    o->surface=wl_resource_create(c,&wl_surface_interface,wl_resource_get_version(r),id);
    if(!o->surface){wl_client_post_no_memory(c);return;}
    regions_reset(o);
    o->buffer_scale=o->pending_scale=1;
    wl_resource_set_implementation(o->surface,&surface_impl,o,surface_destroyed);
}
static const struct wl_compositor_interface compositor_impl={.create_surface=create_surface,.create_region=create_region};
static void bind_compositor(struct wl_client *c,void *data,uint32_t version,uint32_t id) {
    struct wl_resource *r=wl_resource_create(c,&wl_compositor_interface,version>3?3:version,id);
    if(!r){wl_client_post_no_memory(c);return;}wl_resource_set_implementation(r,&compositor_impl,data,NULL);
}
static void present_surface(struct wl_client *c,struct wl_resource *r,struct wl_resource *surface,uint32_t method,struct wl_resource *output) {
    (void)c;struct output *o=wl_resource_get_user_data(r);
    if(o->xdg.surface || o->role==2){wl_resource_post_error(r,WL_DISPLAY_ERROR_INVALID_OBJECT,"surface has an xdg role");return;}
    if(surface!=o->surface || (output && (!wl_resource_instance_of(output,&wl_output_interface,&output_impl) || wl_resource_get_user_data(output)!=o)) || (method!=ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT && method!=ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_CENTER)) {fail(r,"single default/center owned output only");return;}
    o->selected=surface!=NULL;if(surface)o->role=1;
}
static void present_mode(struct wl_client *c,struct wl_resource *r,struct wl_resource *surface,struct wl_resource *output,int32_t framerate,uint32_t id) {
    (void)surface;(void)output;(void)framerate;
    struct wl_resource *feedback=wl_resource_create(c,&zwp_fullscreen_shell_mode_feedback_v1_interface,1,id);
    if(!feedback){wl_client_post_no_memory(c);return;}
    zwp_fullscreen_shell_mode_feedback_v1_send_mode_failed(feedback);wl_resource_destroy(feedback);
    (void)r;
}
static const struct zwp_fullscreen_shell_v1_interface shell_impl={.release=destroy_resource,.present_surface=present_surface,.present_surface_for_mode=present_mode};
static void bind_shell(struct wl_client *c,void *data,uint32_t version,uint32_t id) {
    (void)version;struct wl_resource *r=wl_resource_create(c,&zwp_fullscreen_shell_v1_interface,1,id);
    if(!r){wl_client_post_no_memory(c);return;}wl_resource_set_implementation(r,&shell_impl,data,NULL);
}
int main(int argc,char **argv) {
    struct utsname os;if(uname(&os) || strcmp(os.sysname,"Coast"))return 2;
    static struct output o;o.limit=600;const char *check_case=NULL,*project="/home/user/Projects";int electron=0;wl_list_init(&o.callbacks);wl_list_init(&o.pending_destroy.link);o.pending_destroy.notify=pending_destroyed;
    int first_arg=1;
#ifdef CODEOSS_WORKBENCH
    char project_path[OS_PATH_MAX];
    const char *name=strrchr(argv[0],'/');name=name?name+1:argv[0];
    if(!strcmp(name,"code")) {
        if(argc==2 && !strcmp(argv[1],"--help")) {
            puts("Open a folder in Code OSS.\nUsage: code [folder]");return 0;
        }
        if(argc>2 || (argc==2 && argv[1][0]=='-')) {
            fputs("Usage: code [folder]\n",stderr);return 2;
        }
        if(getuid()!=0){fputs("Open Code OSS from the desktop Terminal.\n",stderr);return 2;}
        electron=1;o.workbench=1;first_arg=argc;
        if(argc==2) {
            project=argv[1];
            if(*project!='/') {
                char directory[OS_PATH_MAX];
                if(!getcwd(directory,sizeof(directory)))return 2;
                int length=snprintf(project_path,sizeof(project_path),"%s/%s",directory,project);
                if(length<0 || (size_t)length>=sizeof(project_path))return 2;
                project=project_path;
            }
        }
    }
#endif
    for(int i=first_arg;i<argc;i++) {
        if(!strcmp(argv[i],"--electron")){electron=1;}
#ifdef CODEOSS_WORKBENCH
        else if(!strcmp(argv[i],"--codeoss")){electron=1;o.workbench=1;if(i+1<argc)project=argv[++i];}
        else if(!strcmp(argv[i],"--browser")){electron=1;o.workbench=1;o.browser=1;}
#endif
        else if(!strcmp(argv[i],"--hold"))o.hold=1;
        else if(!strcmp(argv[i],"--stall-input-ms") && i+1<argc){if(frame_limit(argv[++i],&o.stall_ms))return 2;}
        else if(!strcmp(argv[i],"--frames") && i+1<argc){if(frame_limit(argv[++i],&o.limit))return 2;}
        else if(!strcmp(argv[i],"--check") && i+1<argc)check_case=argv[++i];
        else return 2;
    }
    if(electron && (check_case || o.hold || getuid()!=0))return 2;
    if(o.browser) {
        struct os_account_request q={.uid=OS_ACCOUNT_FIRST};
        if(syscall(SYS_account,OS_ACCOUNT_BROWSER_LAUNCH,&q)) {
            fputs("BROWSER_NETWORK_ADMISSION_DENIED\n",stderr);return 2;
        }
    }
    o.electron=electron;if(electron)o.limit=0;
    const char *input_trace=getenv("COAST_INPUT_TRACE");
    o.input_trace=input_trace && !strcmp(input_trace,"1");
    struct os_display_info info={0};
    o.scale=!syscall(SYS_display,DISPLAY_INFO,&info,sizeof(info)) && info.width>=2200 && info.height>=1400?2:1;
    o.width=LOGICAL_WIDTH*o.scale;o.height=LOGICAL_HEIGHT*o.scale;
    o.display_width=info.width?(int)info.width:o.width;o.display_height=info.height?(int)info.height:o.height;
    o.buffer_scale=o.pending_scale=1;
    o.checker=check_case!=NULL;
    setvbuf(stdout,NULL,_IONBF,0);struct sigaction action={.sa_handler=stop_signal};sigemptyset(&action.sa_mask);sigaction(SIGINT,&action,NULL);sigaction(SIGTERM,&action,NULL);
    int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair))return 3;
    pid_t child=fork();if(child<0)return 4;
    if(!child) {
        close(pair[0]);if(fcntl(pair[1],F_SETFD,0))_exit(112);
        char fd[32];snprintf(fd,sizeof(fd),"%d",pair[1]);if(setenv("WAYLAND_SOCKET",fd,1))_exit(113);
        if(electron) {
            if(dup2(pair[1],3)!=3 || fcntl(3,F_SETFD,0))_exit(115);
            for(int n=4;n<COAST_FD_LIMIT;n++)close(n);
            if(o.browser) {
                execl("/bin/g2-wayland-launch","g2-wayland-launch","--nnp=1","--filter","--browser",
                      "--js-flags=--jitless --sandbox-prohibit-insecure-mode",
                      "--ozone-platform=wayland","--disable-gpu","--disable-software-rasterizer",
                      "--disable-frame-rate-limit","--disable-dev-shm-usage",
                      "--enable-logging=stderr","--v=0","/usr/share/coast-browser",(char *)NULL);
                _exit(116);
            }
            if(o.workbench) {
                execl("/bin/g2-wayland-launch","g2-wayland-launch","--nnp=1","--filter","--workbench",
                      "--js-flags=--jitless --sandbox-prohibit-insecure-mode",
                      "--ozone-platform=wayland","--disable-gpu","--disable-software-rasterizer",
                      /* The native Wayland frame callback already paces this
                         software output. Avoid a second synthetic-vsync wait. */
                      "--disable-frame-rate-limit",
                      "--disable-dev-shm-usage","--enable-logging=stderr","--v=0",
                      "/usr/share/codeoss-workbench",
                      "--user-data-dir=/home/port-test/.config/CodeOSS",
                      "--extensions-dir=/home/port-test/extensions","--sandbox","--new-window",
                      "--skip-welcome","--skip-release-notes","--log=info",project,(char *)NULL);
                _exit(116);
            }
            execl("/bin/g2-wayland-launch","g2-wayland-launch","--nnp=1","--filter",
                  "--js-flags=--jitless --sandbox-prohibit-insecure-mode",
                  "--ozone-platform=wayland","--disable-gpu","--disable-software-rasterizer",
                  "--disable-dev-shm-usage",
                  "--enable-logging=stderr","--v=0","/usr/share/electron-window-test",(char *)NULL);
            _exit(116);
        }
        if(check_case){execl("/bin/wayland-output-check","wayland-output-check",check_case,(char *)NULL);_exit(114);}
        execl("/bin/weston-simple-shm","weston-simple-shm",(char *)NULL);_exit(114);
    }
    close(pair[1]);
    struct sigaction ignore_pipe={.sa_handler=SIG_IGN};sigemptyset(&ignore_pipe.sa_mask);sigaction(SIGPIPE,&ignore_pipe,NULL);
    o.display=wl_display_create();
    if(!o.display){close(pair[0]);kill(child,SIGTERM);waitpid(child,NULL,0);return 5;}
    o.client=wl_client_create(o.display,pair[0]);
    if(!o.client){close(pair[0]);wl_display_destroy(o.display);kill(child,SIGTERM);waitpid(child,NULL,0);return 6;}
    o.client_destroy.notify=client_destroyed;wl_client_add_destroy_listener(o.client,&o.client_destroy);
    o.keymap_fd=-1;
    if(wl_display_init_shm(o.display) || !wl_global_create(o.display,&wl_compositor_interface,3,&o,bind_compositor) || !wl_global_create(o.display,&zwp_fullscreen_shell_v1_interface,1,&o,bind_shell) || !wl_global_create(o.display,&xdg_wm_base_interface,1,&o,bind_xdg) || keyboard_init(&o) || output_init(&o) || clipboard_init(&o))o.fatal=1;
    uint64_t start=now_ms();int status=-1,reaped=0;
    while(!stopping && !o.closed && !o.fatal) {
        if(waitpid(child,&status,WNOHANG)==child){reaped=1;break;}
        if(!o.workbench && now_ms()-start>65000){o.fatal=1;break;}
        wl_display_flush_clients(o.display);
        uint64_t dispatch_started_us=now_us();
        int dispatch_wait=2;
        /* The workbench follows the native desktop's 120 Hz cadence. Only
           actual callbacks move its deadline; idle must not make the first
           new frame wait for an unrelated periodic timer. */
        if(!o.hold && (!o.xdg.surface || o.xdg.mapped)) {
            struct frame_callback *pending_frame;
            wl_list_for_each(pending_frame,&o.callbacks,link)if(pending_frame->committed) {
                uint64_t left_us=o.next_frame_us>dispatch_started_us?o.next_frame_us-dispatch_started_us:0;
                uint64_t left=(left_us+999)/1000;
                if(left<(uint64_t)dispatch_wait)dispatch_wait=(int)left;
                break;
            }
        }
        if(wl_event_loop_dispatch(wl_display_get_event_loop(o.display),dispatch_wait)<0 && errno!=EINTR){o.fatal=1;break;}
        uint64_t callback_us=now_us(),now=callback_us/1000,dispatch_ms=(callback_us-dispatch_started_us)/1000;
        if(dispatch_ms>o.dispatch_max_ms)o.dispatch_max_ms=dispatch_ms;
        if(o.input_trace && dispatch_ms>=100 && o.timing_logs++<64)
            printf("NATIVE_DISPATCH_TIMING elapsed_ms=%lu ms=%lu\n",dispatch_ms,now);
        if(!stopping && !o.hold && (!o.xdg.surface || o.xdg.mapped) && callback_us>=o.next_frame_us) {
            struct frame_callback *f,*next;
            unsigned delivered=0;
            wl_list_for_each_safe(f,next,&o.callbacks,link)if(f->committed){wl_callback_send_done(f->resource,(uint32_t)now);o.callbacks_done++;delivered++;wl_resource_destroy(f->resource);}
            if(delivered)o.next_frame_us=callback_us+(o.workbench && !o.browser?8333:16667);
        }
        if(o.window.id) {
            if(o.electron && now-o.display_polled_at>=250) {
                o.display_polled_at=now;
                output_refresh(&o);
            }
            native_geometry(&o);
            if(o.stall_ms && !o.stall_started && o.key_focused) {o.stall_started=now_ms();printf("I1_INPUT_STALL_BEGIN ms=%u\n",o.stall_ms);}
            if(o.stall_started && now_ms()-o.stall_started<o.stall_ms)continue;
            if(o.stall_started && !o.stall_ended){o.stall_ended=1;puts("I1_INPUT_STALL_END");}
            keyboard_poll(&o);
            clipboard_poll(&o);
            struct os_window_event events[16];struct os_window_events q={o.window.id,16,(uintptr_t)events};
            long n=native(OS_WINDOW_INPUT,&q,sizeof(q));
            if(n<0 && (errno==EPIPE || errno==ENOENT || errno==ENODEV))o.closed=1;
            for(long i=0;i<n;i++) {
                if(events[i].type==OS_WINDOW_OVERFLOW)printf("I1_WINDOW_QUEUE_OVERFLOW dropped=%u\n",events[i].code);
                keyboard_event(&o,&events[i]);
                pointer_event(&o,&events[i]);
                if(events[i].type==OS_WINDOW_CLOSE)o.closed=1;
                else if(events[i].type==OS_WINDOW_FOCUS && events[i].code && o.frames) {
                    /* The existing desktop wrapper announces the initial
                       sequence as read. Repaint our retained output on focus,
                       without touching an already released client buffer. */
                    struct os_window_frame f={.id=o.window.id,.width=o.pixel_width,.height=o.pixel_height,.stride=o.pixel_width,.pixels=(uintptr_t)o.pixels};
                    if(!native(OS_WINDOW_PRESENT,&f,sizeof(f)))o.repaints++;
                } else o.ignored_input++;
            }
        }
        /* Publish input immediately, before entering another timed wait. */
        wl_display_flush_clients(o.display);
    }
    if(!reaped && o.client && o.xdg.toplevel) {
        xdg_toplevel_send_close(o.xdg.toplevel);o.xdg.closes++;wl_client_flush(o.client);
        printf("P6_XDG_CLOSE_SENT\n");uint64_t until=now_ms()+(o.electron?10000:1000);
        while(now_ms()<until) {
            wl_display_flush_clients(o.display);wl_event_loop_dispatch(wl_display_get_event_loop(o.display),10);
            if(waitpid(child,&status,WNOHANG)==child){reaped=1;break;}
        }
        if(!reaped)o.close_fallback=1;
    }
    if(!reaped) {
        /* Fullscreen-shell has no toplevel close event. Disconnect the owned
           client to wake a blocked dispatch, including a deliberately held
           frame; a signal alone may be retried inside the client library. */
        if(o.client){wl_client_flush(o.client);wl_client_destroy(o.client);}
        uint64_t until=now_ms()+2000;
        while(now_ms()<until) {
            wl_display_flush_clients(o.display);wl_event_loop_dispatch(wl_display_get_event_loop(o.display),10);
            if(waitpid(child,&status,WNOHANG)==child){reaped=1;break;}
        }
        if(!reaped){fprintf(stderr,"P6_FORCED_CHILD_CLEANUP\n");kill(child,SIGKILL);waitpid(child,&status,0);o.fatal=1;}
    }
    keyboard_leave(&o);wl_display_destroy_clients(o.display);
    clipboard_shutdown(&o);
    unsigned remaining_outputs=0;for(unsigned i=0;i<OUTPUT_LIMIT;i++)remaining_outputs+=o.outputs[i].resource!=NULL;
    printf("O1_RESOURCE_CLEANUP regions=%u outputs=%u\n",o.region_count,remaining_outputs);
    if(o.region_count || remaining_outputs)o.fatal=1;
    wl_display_destroy(o.display);
    if(o.keymap_fd>=0)close(o.keymap_fd);
    if(o.window.id){struct os_window_token t={o.window.id,0};native(OS_WINDOW_CLIENT_CLOSE,&t,sizeof(t));}
    printf("NATIVE_COPY_STATS commits=%u scanned_pixels=%lu presented_pixels=%lu full_frame_pixels=%lu unchanged_frames=%lu\n",
           o.frames,o.scanned_pixels,o.presented_pixels,o.possible_pixels,o.unchanged_frames);
    printf("NATIVE_TIMING_MAX copy_ms=%lu present_ms=%lu dispatch_ms=%lu\n",o.copy_max_ms,o.present_max_ms,o.dispatch_max_ms);
    printf("P6_SURFACE_EXIT frames=%u releases=%u callbacks=%u live_callbacks=%u child_status=%d elapsed_ms=%lu ignored_input=%u repaints=%u closed=%d fatal=%d configures=%u acks=%u closes=%u close_fallback=%d\n",o.frames,o.releases,o.callbacks_done,o.callback_count,status,now_ms()-start,o.ignored_input,o.repaints,o.closed,o.fatal,o.xdg.configures,o.xdg.acks,o.xdg.closes,o.close_fallback);
    /* Closing the workbench's loading window may precede its first frame.
       A clean runtime shutdown is still a successful user cancellation. */
    return o.fatal || !WIFEXITED(status) || WEXITSTATUS(status) ||
           (!o.frames && !(o.workbench && o.closed));
}
