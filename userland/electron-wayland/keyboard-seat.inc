/* Focused native-owner state -> bounded Wayland keyboard seat.
 * No named listener, global capture or layout changes. */
#include "keymap-us.h"
enum { K1_KEYBOARD_LIMIT=4 };
static unsigned evdev(unsigned usage) {
    static const uint16_t map[104]={
        [4]=30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44,
        [30]=2,3,4,5,6,7,8,9,10,11,28,1,14,15,57,12,13,26,27,43,43,39,40,41,51,52,53,58,
        [58]=59,60,61,62,63,64,65,66,67,68,87,88,99,70,119,110,102,104,111,107,109,106,105,108,103,
        [83]=69,98,55,74,78,96,79,80,81,75,76,77,71,72,73,82,83,86,127,116,117
    };
    static const uint16_t modifiers[8]={29,42,56,125,97,54,100,126};
    return usage>=224 && usage<232?modifiers[usage-224]:usage<104?map[usage]:0;
}
static uint32_t depressed(const struct os_keyboard_snapshot *s) {
    unsigned m=s->modifiers;
    return ((m&0x22)?K1_XKB_Shift:0)|((m&0x11)?K1_XKB_Control:0)|
           ((m&0x44)?K1_XKB_Mod1:0)|((m&0x88)?K1_XKB_Mod4:0);
}
static uint32_t locked(const struct os_keyboard_snapshot *s) {
    return ((s->locks&1)?K1_XKB_Lock:0)|((s->locks&2)?K1_XKB_Mod2:0);
}
static void keyboard_modifiers(struct output *o,struct wl_resource *r) {
    wl_keyboard_send_modifiers(r,wl_display_next_serial(o->display),depressed(&o->key_state),0,locked(&o->key_state),0);
}
static void keyboard_enter(struct output *o,struct wl_resource *r) {
    uint32_t keys[256];unsigned count=0;
    for(unsigned i=4;i<256;i++)if((o->key_state.pressed[i/8]>>(i%8))&1) {
        unsigned key=evdev(i);if(key)keys[count++]=key;
    }
    struct wl_array a={.size=count*sizeof(uint32_t),.alloc=sizeof(keys),.data=keys};
    wl_keyboard_send_enter(r,wl_display_next_serial(o->display),o->surface,&a);keyboard_modifiers(o,r);
}
static void keyboard_leave(struct output *o) {
    clipboard_focus_lost(o);
    pointer_leave(o);
    if(o->key_focused && o->surface)
        for(unsigned i=0;i<K1_KEYBOARD_LIMIT;i++)if(o->keyboards[i])
            wl_keyboard_send_leave(o->keyboards[i],wl_display_next_serial(o->display),o->surface);
    o->key_focused=0;memset(&o->key_state,0,sizeof(o->key_state));
}
static void keyboard_destroyed(struct wl_resource *r) {
    struct output *o=wl_resource_get_user_data(r);
    for(unsigned i=0;i<K1_KEYBOARD_LIMIT;i++)if(o->keyboards[i]==r)o->keyboards[i]=NULL;
}
static const struct wl_keyboard_interface keyboard_impl={.release=destroy_resource};
static void get_keyboard(struct wl_client *c,struct wl_resource *r,uint32_t id) {
    struct output *o=wl_resource_get_user_data(r);unsigned i=0;
    while(i<K1_KEYBOARD_LIMIT && o->keyboards[i])i++;
    if(i==K1_KEYBOARD_LIMIT){fail(r,"keyboard resource bound");return;}
    struct wl_resource *k=wl_resource_create(c,&wl_keyboard_interface,wl_resource_get_version(r),id);
    if(!k){wl_client_post_no_memory(c);return;}
    o->keyboards[i]=k;wl_resource_set_implementation(k,&keyboard_impl,o,keyboard_destroyed);
    wl_keyboard_send_keymap(k,WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,o->keymap_fd,sizeof(k1_keymap));
    if(wl_resource_get_version(k)>=4)wl_keyboard_send_repeat_info(k,25,500);
    if(o->key_focused)keyboard_enter(o,k);
}
static void no_pointer(struct wl_client *c,struct wl_resource *r,uint32_t id) {
    (void)c;(void)id;fail(r,"seat only advertises keyboard");
}
static const struct wl_seat_interface seat_impl={.get_pointer=get_pointer,.get_keyboard=get_keyboard,.get_touch=no_pointer,.release=destroy_resource};
static void seat_destroyed(struct wl_resource *r) {
    struct output *o=wl_resource_get_user_data(r);if(o->seat_count)o->seat_count--;
}
static void bind_seat(struct wl_client *c,void *data,uint32_t version,uint32_t id) {
    struct output *o=data;
    if(o->seat_count==K1_KEYBOARD_LIMIT){wl_client_post_no_memory(c);return;}
    struct wl_resource *r=wl_resource_create(c,&wl_seat_interface,version>5?5:version,id);
    if(!r){wl_client_post_no_memory(c);return;}
    o->seat_count++;wl_resource_set_implementation(r,&seat_impl,o,seat_destroyed);
    wl_seat_send_capabilities(r,WL_SEAT_CAPABILITY_KEYBOARD|(o->electron?WL_SEAT_CAPABILITY_POINTER:0));
    if(wl_resource_get_version(r)>=2)wl_seat_send_name(r,"Coast owned window");
}
static int keyboard_init(struct output *o) {
    o->keymap_fd=memfd_create("K1-fixed-us-keymap",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    if(o->keymap_fd<0)return -1;
    size_t at=0;
    while(at<sizeof(k1_keymap)) {
        ssize_t n=write(o->keymap_fd,k1_keymap+at,sizeof(k1_keymap)-at);
        if(n<=0)return -1;
        at+=n;
    }
    if(fcntl(o->keymap_fd,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL))return -1;
    return wl_global_create(o->display,&wl_seat_interface,5,o,bind_seat)?0:-1;
}
static void keyboard_poll(struct output *o) {
    if(!o->window.id || !o->surface || !o->surface_has_buffer){keyboard_leave(o);return;}
    struct os_window_keyboard q={.version=OS_WINDOW_KEYBOARD_VERSION,.size=sizeof(q),.id=o->window.id};
    if(native(OS_WINDOW_KEYBOARD_STATE,&q,sizeof(q))) {bool was=o->key_focused;keyboard_leave(o);if(was)xdg_focus_configure(o);return;}
    if(!o->key_focused || o->key_generation!=q.generation) {
        keyboard_leave(o);o->key_state=q.state;o->key_generation=q.generation;
        o->key_watermark=q.event_sequence;o->key_focused=1;
        for(unsigned i=0;i<K1_KEYBOARD_LIMIT;i++)if(o->keyboards[i])keyboard_enter(o,o->keyboards[i]);
        printf("K1_SEAT_ENTER generation=%lu watermark=%lu\n",q.generation,q.event_sequence);
        xdg_focus_configure(o);
    }
}
static void keyboard_event(struct output *o,const struct os_window_event *e) {
    /* Overflow notifications have sequence0; never discard this control event
       as an old data edge. Resync through the existing focused snapshot. */
    if(e->type==OS_WINDOW_OVERFLOW){bool was=o->key_focused;keyboard_leave(o);if(was)xdg_focus_configure(o);return;}
    if(e->sequence<=o->key_watermark)return;
    if(e->type==OS_WINDOW_OVERFLOW || (e->type==OS_WINDOW_FOCUS && !e->code) || e->type==OS_WINDOW_CLOSE) {
        bool was=o->key_focused;keyboard_leave(o);if(was && e->type!=OS_WINDOW_CLOSE)xdg_focus_configure(o);return;
    }
    if(e->type!=OS_WINDOW_HID_KEY || !o->key_focused)return;
    o->key_watermark=e->sequence;
    if(e->code<4 || e->code>255 || e->dy<0 || e->dy>255){keyboard_leave(o);return;}
    unsigned usage=e->code,mask=1u<<(usage%8);bool held=(o->key_state.pressed[usage/8]&mask)!=0;
    if(e->buttons && !held) {
        if(usage==0x39)o->key_state.locks^=1;
        if(usage==0x53)o->key_state.locks^=2;
        if(usage==0x47)o->key_state.locks^=4;
    }
    if(e->buttons)o->key_state.pressed[usage/8]|=mask;else o->key_state.pressed[usage/8]&=~mask;
    o->key_state.modifiers=e->dy;o->key_state.pressed[28]=e->dy;
    unsigned code=evdev(usage);
    if(o->input_trace)printf("K1_INPUT usage=%u down=%u modifiers=%u forwarded=%u ms=%lu sequence=%lu\n",usage,!!e->buttons,e->dy,code && held!=!!e->buttons,now_ms(),e->sequence);
    for(unsigned i=0;i<K1_KEYBOARD_LIMIT;i++)if(o->keyboards[i]) {
        keyboard_modifiers(o,o->keyboards[i]);
        if(code && held!=!!e->buttons) {
            uint32_t serial=wl_display_next_serial(o->display);clipboard_note_serial(o,serial);
            wl_keyboard_send_key(o->keyboards[i],serial,(uint32_t)now_ms(),code,e->buttons?1:0);
        }
    }
}
