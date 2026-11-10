#include <sched.h>
#include <video/display.h>
#include <video/window.h>
#include <uapi/window.h>
#include <text_utf8.h>
#include <mm/pmm.h>
#include <string.h>
#define EVENTS 128
static mutex_t window_mutex;
static unsigned server, next_id, focused;
static uint64_t event_sequence;
static uint64_t keyboard_generation;
struct client_window {
    struct os_window_info info;
    bool claimed, closing;
    uint32_t *front, *back;
    size_t pixel_capacity;
    struct os_window_event events[EVENTS];
    unsigned head, tail, dropped;
    unsigned requested_width, requested_height, states, state_request;
    struct os_window_cursor cursor;
    uint32_t *cursor_pixels;
    struct os_keyboard_snapshot keyboard;
    uint64_t keyboard_generation, last_event_sequence;
    uint64_t read_sequence;
    unsigned damage_x, damage_y, damage_width, damage_height;
};
static struct client_window clients[OS_WINDOW_SLOTS];
static void frame_damage(struct client_window *w, unsigned x, unsigned y, unsigned width, unsigned height) {
    if (w->damage_width) {
        unsigned right=x+width, bottom=y+height;
        if (right<w->damage_x+w->damage_width) right=w->damage_x+w->damage_width;
        if (bottom<w->damage_y+w->damage_height) bottom=w->damage_y+w->damage_height;
        if (x>w->damage_x) x=w->damage_x;
        if (y>w->damage_y) y=w->damage_y;
        width=right-x; height=bottom-y;
    }
    w->damage_x=x; w->damage_y=y; w->damage_width=width; w->damage_height=height;
}
static void keyboard_invalidate(struct client_window *w) {
    memset(&w->keyboard, 0, sizeof(w->keyboard));
    /* Exhaustion permanently disables new state handoffs; never reuse an epoch. */
    w->keyboard_generation = keyboard_generation == UINT64_MAX ? 0 : ++keyboard_generation;
}
static bool keyboard_valid(const struct os_keyboard_snapshot *s) {
    return s->version == OS_KEYBOARD_STATE_VERSION && s->size == sizeof(*s) &&
        !s->reserved && !(s->flags & ~OS_KEYBOARD_VALID) && !(s->locks & ~7u) &&
        s->modifiers <= 255 && !(s->pressed[0] & 15u) &&
        s->pressed[28] == s->modifiers &&
        (!(s->flags & OS_KEYBOARD_VALID) || s->source_generation);
}
static void keyboard_event(struct client_window *w, const struct os_window_event *e) {
    if (!(w->keyboard.flags & OS_KEYBOARD_VALID)) return;
    if (e->type == OS_WINDOW_KEY ||
        (e->type == OS_WINDOW_HID_KEY && (e->code < 4 || e->dy < 0 || e->dy > 255))) {
        keyboard_invalidate(w);
        return;
    }
    if (e->type != OS_WINDOW_HID_KEY) return;
    unsigned key = e->code, mask = 1u << (key % 8);
    bool held = (w->keyboard.pressed[key / 8] & mask) != 0;
    if (e->buttons && !held) {
        if (key == 0x39) w->keyboard.locks ^= OS_KEYBOARD_LOCK_CAPS;
        if (key == 0x53) w->keyboard.locks ^= OS_KEYBOARD_LOCK_NUM;
        if (key == 0x47) w->keyboard.locks ^= OS_KEYBOARD_LOCK_SCROLL;
    }
    if (e->buttons) w->keyboard.pressed[key / 8] |= mask;
    else w->keyboard.pressed[key / 8] &= ~mask;
    w->keyboard.modifiers = (unsigned)e->dy;
    w->keyboard.pressed[28] = (uint8_t)e->dy;
}
static struct client_window *find(unsigned id) {
    for (unsigned i = 0; i < OS_WINDOW_SLOTS; i++)
        if (clients[i].info.id == id && id)
            return &clients[i];
    return NULL;
}
bool window_client_focused(unsigned pid) {
    mutex_lock(&window_mutex);
    struct client_window *w = find(focused);
    bool permitted = w && !w->closing && w->claimed && w->info.client == pid;
    mutex_unlock(&window_mutex);
    return permitted;
}
static void clear(struct client_window *w) {
    if (focused == w->info.id)
        focused = 0;
    kfree(w->front);
    kfree(w->back);
    kfree(w->cursor_pixels);
    memset(w, 0, sizeof(*w));
}
static void push(struct client_window *w, struct os_window_event event) {
    unsigned n = (w->head + 1) % EVENTS;
    if (n == w->tail) {
        w->tail = (w->tail + 1) % EVENTS;
        w->dropped++;
        keyboard_invalidate(w);
    }
    event.sequence = ++event_sequence;
    w->last_event_sequence = event.sequence;
    keyboard_event(w, &event);
    w->events[w->head] = event;
    w->head = n;
}
void window_drop(unsigned pid) {
    mutex_lock(&window_mutex);
    if (pid == server) {
        server = focused = 0;
        for (unsigned i = 0; i < OS_WINDOW_SLOTS; i++)
            clear(&clients[i]);
    } else
        for (unsigned i = 0; i < OS_WINDOW_SLOTS; i++)
            if (clients[i].info.client == pid)
                clear(&clients[i]);
    mutex_unlock(&window_mutex);
}
static bool server_operation(unsigned op) {
    return (op >= OS_WINDOW_SERVER_NEXT && op <= OS_WINDOW_SERVER_KEYBOARD_SET) ||
           op == OS_WINDOW_SERVER_CONFIGURE || op == OS_WINDOW_SERVER_REQUEST || op == OS_WINDOW_SERVER_CURSOR ||
           op == OS_WINDOW_SERVER_READ_DAMAGE;
}
static long window_dispatch(struct task *t, unsigned op, uint64_t pointer, uint64_t size) {
    if (t->browser_sandbox || t->context->web_role)
        return -E_PERM;
    if (op > OS_WINDOW_SERVER_READ_DAMAGE)
        return -E_INVAL;
    if (op == OS_WINDOW_SERVER_START) {
        if (pointer || size)
            return -E_INVAL;
        if (!display_owned(t->pid))
            return -E_PERM;
        long r = server && server != t->pid ? -E_BUSY : 0;
        if (!r)
            server = t->pid;
        return r;
    }
    if (server_operation(op) && (server != t->pid || !display_owned(t->pid)))
        return -E_PERM;
    if (!server)
        return -E_NODEV;
    if (op == OS_WINDOW_CURSOR || op == OS_WINDOW_SERVER_CURSOR) {
        struct os_window_cursor request;
        if (size!=sizeof(request)) return -E_INVAL;
        if (!user_copy_in(&t->space,&request,pointer,sizeof(request))) return -E_FAULT;
        if (request.reserved) return -E_INVAL;
        struct client_window *w=find(request.id);
        if (!w) return -E_NOENT;
        if (op==OS_WINDOW_CURSOR && w->info.client!=t->pid) return -E_PERM;
        if (w->closing) return -E_PIPE;
        if (w->info.version!=OS_WINDOW_PIXEL_CLIENT_VERSION) return -E_NOTSUP;
        if (op==OS_WINDOW_SERVER_CURSOR) {
            if (request.sequence!=w->cursor.sequence && w->cursor.mode==OS_WINDOW_CURSOR_BITMAP &&
                !user_copy_out(&t->space,request.pixels,w->cursor_pixels,(size_t)w->cursor.width*w->cursor.height*4)) return -E_FAULT;
            uint64_t pixels=request.pixels;
            request=w->cursor;request.id=w->info.id;request.pixels=pixels;
            return user_copy_out(&t->space,pointer,&request,sizeof(request))?0:-E_FAULT;
        }
        if (request.sequence || request.mode>OS_WINDOW_CURSOR_HIDDEN) return -E_INVAL;
        uint32_t *pixels=NULL;
        if (request.mode==OS_WINDOW_CURSOR_BITMAP) {
            if (!request.width || !request.height || request.width>OS_WINDOW_CURSOR_MAX || request.height>OS_WINDOW_CURSOR_MAX ||
                request.stride<request.width || request.stride>OS_WINDOW_CURSOR_MAX ||
                request.hotspot_x<-(int)OS_WINDOW_CURSOR_MAX || request.hotspot_x>(int)OS_WINDOW_CURSOR_MAX ||
                request.hotspot_y<-(int)OS_WINDOW_CURSOR_MAX || request.hotspot_y>(int)OS_WINDOW_CURSOR_MAX) return -E_INVAL;
            pixels=kmalloc((size_t)request.width*request.height*4);
            if (!pixels) return -E_NOMEM;
            for (unsigned y=0;y<request.height;y++) {
                if (!user_copy_in(&t->space,pixels+(size_t)y*request.width,request.pixels+(size_t)y*request.stride*4,(size_t)request.width*4)) {
                    kfree(pixels);return -E_FAULT;
                }
            }
            for (unsigned i=0;i<request.width*request.height;i++) {
                unsigned pixel=pixels[i],alpha=pixel>>24;
                if (((pixel>>16)&255)>alpha || ((pixel>>8)&255)>alpha || (pixel&255)>alpha) {kfree(pixels);return -E_INVAL;}
            }
        } else if (request.width || request.height || request.stride || request.hotspot_x || request.hotspot_y || request.pixels) return -E_INVAL;
        if (w->cursor.sequence==UINT64_MAX) {kfree(pixels);return -E_NOSPC;}
        request.sequence=w->cursor.sequence+1;request.pixels=0;
        if (request.mode==OS_WINDOW_CURSOR_BITMAP) request.stride=request.width;
        kfree(w->cursor_pixels);w->cursor_pixels=pixels;w->cursor=request;
        return 0;
    }
    if (op == OS_WINDOW_SERVER_CONFIGURE) {
        struct os_window_configure request;
        if (size != sizeof(request)) return -E_INVAL;
        if (!user_copy_in(&t->space,&request,pointer,sizeof(request))) return -E_FAULT;
        if (!request.width || !request.height ||
            request.width>OS_WINDOW_PIXEL_MAX_WIDTH || request.height>OS_WINDOW_PIXEL_MAX_HEIGHT ||
            (request.states&~(OS_WINDOW_STATE_MAXIMIZED|OS_WINDOW_STATE_MINIMIZED|OS_WINDOW_STATE_FULLSCREEN))) return -E_INVAL;
        struct client_window *w=find(request.id);
        if (!w) return -E_NOENT;
        if (w->closing) return -E_PIPE;
        if (w->info.version!=OS_WINDOW_PIXEL_CLIENT_VERSION) return -E_NOTSUP;
        if (w->requested_width!=request.width || w->requested_height!=request.height || w->states!=request.states) {
            w->requested_width=request.width; w->requested_height=request.height; w->states=request.states;
            push(w,(struct os_window_event){.type=OS_WINDOW_CONFIGURE,.code=w->states,
                 .dx=request.width,.dy=request.height});
        }
        return 0;
    }
    if (op == OS_WINDOW_GEOMETRY) {
        struct os_window_geometry request;
        if (size!=sizeof(request)) return -E_INVAL;
        if (!user_copy_in(&t->space,&request,pointer,sizeof(request))) return -E_FAULT;
        if (request.reserved0 || request.reserved1) return -E_INVAL;
        struct client_window *w=find(request.id);
        if (!w) return -E_NOENT;
        if (t->pid!=server && t->pid!=w->info.client) return -E_PERM;
        if (w->closing) return -E_PIPE;
        struct os_window_geometry result={.id=w->info.id,.width=w->info.width,.height=w->info.height,
            .states=w->states,.requested_width=w->requested_width,.requested_height=w->requested_height,
            .sequence=w->info.sequence};
        return user_copy_out(&t->space,pointer,&result,sizeof(result))?0:-E_FAULT;
    }
    if (op == OS_WINDOW_REQUEST_STATE) {
        struct os_window_request request;
        if (size!=sizeof(request)) return -E_INVAL;
        if (!user_copy_in(&t->space,&request,pointer,sizeof(request))) return -E_FAULT;
        if (request.action<OS_WINDOW_REQUEST_MAXIMIZE || request.action>OS_WINDOW_REQUEST_UNFULLSCREEN) return -E_INVAL;
        struct client_window *w=find(request.id);
        if (!w) return -E_NOENT;
        if (w->info.client!=t->pid) return -E_PERM;
        if (w->closing) return -E_PIPE;
        if (w->info.version!=OS_WINDOW_PIXEL_CLIENT_VERSION) return -E_NOTSUP;
        /* One coalesced request per owned window, never a global action. */
        w->state_request=request.action;
        return 0;
    }
    if (op == OS_WINDOW_SERVER_REQUEST) {
        if (size!=sizeof(struct os_window_request)) return -E_INVAL;
        for (unsigned i=0;i<OS_WINDOW_SLOTS;i++) {
            struct client_window *w=&clients[i];
            if (!w->info.id || w->closing || !w->state_request) continue;
            struct os_window_request request={w->info.id,w->state_request};
            if (!user_copy_out(&t->space,pointer,&request,sizeof(request))) return -E_FAULT;
            w->state_request=0; return 0;
        }
        return -E_AGAIN;
    }
    if (op >= OS_WINDOW_SERVER_KEYBOARD_GET && op <= OS_WINDOW_KEYBOARD_STATE) {
        struct os_window_keyboard request;
        if (size != sizeof(request)) return -E_INVAL;
        if (!user_copy_in(&t->space, &request, pointer, sizeof(request))) return -E_FAULT;
        if (request.version != OS_WINDOW_KEYBOARD_VERSION || request.size != sizeof(request) ||
            request.reserved || !request.id) return -E_INVAL;
        struct client_window *w = find(request.id);
        if (!w) return -E_NOENT;
        if (op == OS_WINDOW_KEYBOARD_STATE && w->info.client != t->pid) return -E_PERM;
        if (w->closing) return -E_PIPE;
        if (focused != w->info.id) return -E_PERM;
        if (!w->keyboard_generation) return -E_NOSPC;
        if (request.generation && request.generation != w->keyboard_generation) return -E_AGAIN;
        if (op == OS_WINDOW_SERVER_KEYBOARD_SET) {
            if (!request.generation || request.event_sequence != w->last_event_sequence) return -E_AGAIN;
            if (!keyboard_valid(&request.state)) return -E_INVAL;
            if (w->keyboard.version && (request.state.sequence < w->keyboard.sequence ||
                request.state.source_generation < w->keyboard.source_generation)) return -E_AGAIN;
            if (!user_access(&t->space, pointer, sizeof(request), true)) return -E_FAULT;
            if (!(request.state.flags & OS_KEYBOARD_VALID) && (w->keyboard.flags & OS_KEYBOARD_VALID))
                keyboard_invalidate(w);
            w->keyboard = request.state;
        } else if (op == OS_WINDOW_KEYBOARD_STATE && !(w->keyboard.flags & OS_KEYBOARD_VALID))
            return -E_AGAIN;
        struct os_window_keyboard result = {.version=OS_WINDOW_KEYBOARD_VERSION,
            .size=sizeof(result), .id=w->info.id, .generation=w->keyboard_generation,
            .event_sequence=w->last_event_sequence, .state=w->keyboard};
        return user_copy_out(&t->space, pointer, &result, sizeof(result)) ? 0 : -E_FAULT;
    }
    if (op == OS_WINDOW_CREATE) {
        struct os_window_info info;
        if (size != sizeof(info))
            return -E_INVAL;
        if (!user_copy_in(&t->space, &info, pointer, sizeof(info)) ||
            !user_access(&t->space, pointer, sizeof(info), true))
            return -E_FAULT;
        size_t len = 0;
        while (len < sizeof(info.title) && info.title[len])
            len++;
        bool native_pixels = info.version == OS_WINDOW_PIXEL_CLIENT_VERSION;
        if ((info.version != OS_WINDOW_VERSION && info.version != OS_WINDOW_KEYBOARD_CLIENT_VERSION && !native_pixels) ||
            info.id || info.client || info.reserved ||
            info.sequence || !info.width || !info.height ||
            info.width > (native_pixels ? OS_WINDOW_PIXEL_MAX_WIDTH : OS_WINDOW_MAX_WIDTH) ||
            info.height > (native_pixels ? OS_WINDOW_PIXEL_MAX_HEIGHT : OS_WINDOW_MAX_HEIGHT) || len == sizeof(info.title) ||
            !text_utf8_valid(info.title, len))
            return -E_INVAL;
        for (size_t i = 0; i < len; i++)
            if ((unsigned char)info.title[i] < 32)
                return -E_INVAL;
        long r = -E_NOSPC;
        struct client_window *w = NULL;
        for (unsigned i = 0; i < OS_WINDOW_SLOTS; i++) {
            if (clients[i].info.client == t->pid) {
                r = -E_BUSY;
                goto create_done;
            }
            if (!clients[i].info.id)
                w = &clients[i];
        }
        if (!w)
            goto create_done;
        size_t bytes = (size_t)info.width * info.height * 4;
        uint32_t *front = kmallocz(bytes), *back = kmalloc(bytes);
        if (!front || !back) {
            kfree(front);
            kfree(back);
            r = -E_NOMEM;
            goto create_done;
        }
        if (next_id == UINT32_MAX) {
            kfree(front);
            kfree(back);
            r = -E_NOSPC;
            goto create_done;
        }
        info.id = ++next_id;
        info.client = t->pid;
        if (!user_copy_out(&t->space, pointer, &info, sizeof(info))) {
            kfree(front);
            kfree(back);
            r = -E_FAULT;
            goto create_done;
        }
        *w = (struct client_window){.info = info, .front = front, .back = back,
            .pixel_capacity=bytes,
            .requested_width=info.width,.requested_height=info.height};
        r = 0;
    create_done:
        return r;
    }
    if (op == OS_WINDOW_SERVER_NEXT) {
        if (size != sizeof(struct os_window_info))
            return -E_INVAL;
        long r = -E_AGAIN;
        for (unsigned i = 0; i < OS_WINDOW_SLOTS; i++) {
            struct client_window *w = &clients[i];
            if (!w->info.id || w->claimed || w->closing)
                continue;
            if (!user_copy_out(&t->space, pointer, &w->info, sizeof(w->info)))
                r = -E_FAULT;
            else {
                w->claimed = true;
                r = 0;
            }
            break;
        }
        return r;
    }
    if (op == OS_WINDOW_PRESENT || op == OS_WINDOW_SERVER_READ ||
        op == OS_WINDOW_PRESENT_DAMAGE || op == OS_WINDOW_SERVER_READ_DAMAGE) {
        bool region=op==OS_WINDOW_PRESENT_DAMAGE || op==OS_WINDOW_SERVER_READ_DAMAGE;
        bool present=op==OS_WINDOW_PRESENT || op==OS_WINDOW_PRESENT_DAMAGE;
        struct os_window_damage_frame request={0};
        size_t record_size=region?sizeof(request):sizeof(request.frame);
        if (size!=record_size) return -E_INVAL;
        if (!user_copy_in(&t->space,&request,pointer,record_size)) return -E_FAULT;
        struct os_window_frame *frame=&request.frame;
        struct client_window *w=find(frame->id);
        if (!w) return -E_NOENT;
        if (present && w->info.client!=t->pid) return -E_PERM;
        if (w->closing) return -E_PIPE;
        if (region && w->info.version!=OS_WINDOW_PIXEL_CLIENT_VERSION) return -E_NOTSUP;
        bool resize=frame->width!=w->info.width || frame->height!=w->info.height;
        if (frame->stride!=frame->width || (resize &&
            (!present || frame->width!=w->requested_width || frame->height!=w->requested_height))) return -E_INVAL;
        size_t bytes=(size_t)frame->width*frame->height*4;
        if (present) {
            if (!region) {
                request.x=request.y=0; request.width=frame->width; request.height=frame->height;
            }
            if (!request.width || !request.height || request.x>=frame->width || request.y>=frame->height ||
                request.width>frame->width-request.x || request.height>frame->height-request.y) return -E_INVAL;
            bool full=!request.x && !request.y && request.width==frame->width && request.height==frame->height;
            if ((resize || !w->info.sequence) && !full) return -E_INVAL;
            if (w->info.sequence==UINT64_MAX) return -E_NOSPC;
            if (frame->pixels>UINT64_MAX-bytes) return -E_FAULT;
            /* Retain bounded capacity across live resize. Both stores remain
               private: publish only after the entire new frame validates. */
            bool grow=bytes>w->pixel_capacity;
            size_t capacity=grow ? (bytes+1048575)&~(size_t)1048575 : w->pixel_capacity;
            uint32_t *target=grow?kmalloc(capacity):w->back;
            uint32_t *spare=grow?kmalloc(capacity):NULL;
            if (!target || (grow && !spare)) {
                if (grow) {kfree(target); kfree(spare);}
                return -E_NOMEM;
            }
            bool copied=true;
            if (full) copied=user_copy_in(&t->space,target,frame->pixels,bytes);
            else for (unsigned y=request.y;y<request.y+request.height;y++) {
                size_t offset=(size_t)y*frame->width+request.x;
                if (!user_copy_in(&t->space,target+offset,frame->pixels+offset*4,(size_t)request.width*4)) {
                    copied=false; break;
                }
            }
            if (!copied) {
                if (grow) {kfree(target); kfree(spare);}
                return -E_FAULT;
            }
            if (grow) {
                kfree(w->front); kfree(w->back);
                w->front=target; w->back=spare;
                w->pixel_capacity=capacity;
            } else if (full) {
                uint32_t *old=w->front; w->front=w->back; w->back=old;
            } else {
                /* All user reads completed into private staging. A fault on
                   any row above cannot expose a partially updated front. */
                for (unsigned y=request.y;y<request.y+request.height;y++) {
                    size_t offset=(size_t)y*frame->width+request.x;
                    memcpy(w->front+offset,target+offset,(size_t)request.width*4);
                }
            }
            if (resize) {
                w->info.width=frame->width; w->info.height=frame->height;
                w->damage_width=w->damage_height=0;
            }
            frame_damage(w,request.x,request.y,request.width,request.height);
            w->info.sequence++;
            return 0;
        }
        if (frame->sequence==w->info.sequence) return -E_AGAIN;
        request.x=request.y=0; request.width=frame->width; request.height=frame->height;
        if (region && frame->sequence==w->read_sequence && w->damage_width) {
            request.x=w->damage_x; request.y=w->damage_y;
            request.width=w->damage_width; request.height=w->damage_height;
        }
        if (frame->pixels>UINT64_MAX-bytes || !user_access(&t->space,pointer,record_size,true)) return -E_FAULT;
        if (!region || (!request.x && request.width==frame->width)) {
            size_t offset=region ? (size_t)request.y*frame->width : 0;
            size_t count=region ? (size_t)request.height*frame->width*4 : bytes;
            if (!user_copy_out(&t->space,frame->pixels+offset*4,w->front+offset,count)) return -E_FAULT;
        } else for (unsigned y=request.y;y<request.y+request.height;y++) {
            size_t offset=(size_t)y*frame->width+request.x;
            if (!user_copy_out(&t->space,frame->pixels+offset*4,w->front+offset,(size_t)request.width*4)) return -E_FAULT;
        }
        frame->sequence=w->info.sequence;
        if (!user_copy_out(&t->space,pointer,&request,record_size)) return -E_FAULT;
        w->read_sequence=w->info.sequence; w->damage_width=w->damage_height=0;
        return 0;
    }
    if (op == OS_WINDOW_INPUT || op == OS_WINDOW_SERVER_SEND) {
        struct os_window_events request;
        if (size != sizeof(request))
            return -E_INVAL;
        if (!user_copy_in(&t->space, &request, pointer, sizeof(request)))
            return -E_FAULT;
        if (!request.count || request.count > 64)
            return -E_INVAL;
        struct os_window_event events[64];
        if (op == OS_WINDOW_SERVER_SEND) {
            if (!user_copy_in(&t->space, events, request.events, request.count * sizeof(events[0])))
                return -E_FAULT;
            for (unsigned i = 0; i < request.count; i++)
                if (events[i].type < OS_WINDOW_KEY || events[i].type > OS_WINDOW_HID_KEY ||
                    events[i].type == OS_WINDOW_OVERFLOW ||
                    (events[i].type == OS_WINDOW_HID_KEY &&
                     (events[i].code > 255 || events[i].buttons > 1)))
                    return -E_INVAL;
        } else if (!user_access(&t->space, request.events, request.count * sizeof(events[0]), true))
            return -E_FAULT;
        struct client_window *w = find(request.id);
        long r = 0;
        if (!w)
            r = -E_NOENT;
        else if (op == OS_WINDOW_INPUT && w->info.client != t->pid)
            r = -E_PERM;
        else if (op == OS_WINDOW_SERVER_SEND) {
            if (w->closing)
                r = -E_PIPE;
            else if (focused != w->info.id)
                r = -E_AGAIN;
            else
                for (unsigned i = 0; i < request.count; i++)
                    push(w, events[i]);
        } else {
            unsigned n = 0, tail = w->tail;
            if (w->dropped)
                events[n++] =
                    (struct os_window_event){.type = OS_WINDOW_OVERFLOW, .code = w->dropped};
            while (tail != w->head && n < request.count) {
                events[n++] = w->events[tail];
                tail = (tail + 1) % EVENTS;
            }
            if (!n)
                r = w->closing ? -E_PIPE : -E_AGAIN;
            else if (!user_copy_out(&t->space, request.events, events, n * sizeof(events[0])))
                r = -E_FAULT;
            else {
                w->tail = tail;
                w->dropped = 0;
                r = n;
            }
        }
        return r;
    }
    if (op == OS_WINDOW_SERVER_FOCUS) {
        struct os_window_focus f;
        if (size != sizeof(f))
            return -E_INVAL;
        if (!user_copy_in(&t->space, &f, pointer, sizeof(f)))
            return -E_FAULT;
        if (f.active > 1 || (f.active && !f.id))
            return -E_INVAL;
        struct client_window *w = find(f.id);
        long r = 0;
        if (f.id && !w)
            r = -E_NOENT;
        else if (w && w->closing)
            r = -E_PIPE;
        else {
            unsigned target = f.active ? f.id : 0;
            if (focused != target) {
                struct client_window *old = find(focused);
                if (old) {
                    keyboard_invalidate(old);
                    old->head = old->tail = old->dropped = 0;
                    push(old, (struct os_window_event){.type = OS_WINDOW_FOCUS, .code = 0});
                }
                focused = target;
                if (target) {
                    keyboard_invalidate(w);
                    push(w, (struct os_window_event){.type = OS_WINDOW_FOCUS, .code = 1});
                }
            }
        }
        return r;
    }
    if (op == OS_WINDOW_CLIENT_CLOSE || op == OS_WINDOW_SERVER_CLOSE) {
        struct os_window_token token;
        if (size != sizeof(token))
            return -E_INVAL;
        if (!user_copy_in(&t->space, &token, pointer, sizeof(token)))
            return -E_FAULT;
        if (token.reserved)
            return -E_INVAL;
        struct client_window *w = find(token.id);
        long r = 0;
        if (!w)
            r = -E_NOENT;
        else if (op == OS_WINDOW_CLIENT_CLOSE && w->info.client != t->pid)
            r = -E_PERM;
        else if (op == OS_WINDOW_CLIENT_CLOSE)
            clear(w);
        else if (!w->closing) {
            w->closing = true;
            kfree(w->front);
            kfree(w->back);
            w->front = w->back = NULL;
            w->head = w->tail = w->dropped = 0;
            if (focused == w->info.id)
                focused = 0;
            push(w, (struct os_window_event){.type = OS_WINDOW_CLOSE});
        }
        return r;
    }
    return -E_INVAL;
}

long window_call(struct task *t, unsigned op, uint64_t pointer, uint64_t size) {
    /* Admission, server identity, frames and queues form one atomic state.
       This mutex never nests display/VFS locks; display_owned is atomic. */
    mutex_lock(&window_mutex);
    long result = window_dispatch(t, op, pointer, size);
    mutex_unlock(&window_mutex);
    return result;
}
