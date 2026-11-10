/* Private native-window pointer events. No raw/global input access. */
static void pointer_leave(struct output *o) {
    if(o->pointer_focused){o->cursor_mode=OS_WINDOW_CURSOR_DEFAULT;cursor_apply(o);}
    if(o->pointer_focused && o->surface)
        for(unsigned i=0;i<4;i++)if(o->pointers[i]) {
            wl_pointer_send_leave(o->pointers[i],wl_display_next_serial(o->display),o->surface);
            if(wl_resource_get_version(o->pointers[i])>=5)wl_pointer_send_frame(o->pointers[i]);
        }
    o->pointer_focused=0;o->pointer_buttons=0;
    memset(o->pointer_enter_serial,0,sizeof(o->pointer_enter_serial));
}
static void pointer_destroyed(struct wl_resource *r) {
    struct output *o=wl_resource_get_user_data(r);
    for(unsigned i=0;i<4;i++)if(o->pointers[i]==r){o->pointers[i]=NULL;o->pointer_enter_serial[i]=0;}
}
static void pointer_cursor(struct wl_client *c,struct wl_resource *r,uint32_t serial,struct wl_resource *surface,int32_t x,int32_t y) {
    (void)c;struct output *o=wl_resource_get_user_data(r);
    if(surface && surface!=o->cursor_surface){fail(r,"cursor must use the owned cursor surface");return;}
    unsigned i;for(i=0;i<4;i++)if(o->pointers[i]==r)break;
    if(o->input_trace && o->timing_logs++<64) fprintf(stderr,"K1_CURSOR_REQUEST serial=%u expected=%u focused=%d surface=%d hotspot=%d,%d\n",
        serial,i<4?o->pointer_enter_serial[i]:0,o->pointer_focused,surface!=NULL,x,y);
    if(i==4 || !o->pointer_focused || !serial || o->pointer_enter_serial[i]!=serial)return;
    o->cursor_hot_x=x;o->cursor_hot_y=y;
    o->cursor_mode=surface?OS_WINDOW_CURSOR_BITMAP:OS_WINDOW_CURSOR_HIDDEN;cursor_apply(o);
}
static const struct wl_pointer_interface pointer_impl={.set_cursor=pointer_cursor,.release=destroy_resource};
static void get_pointer(struct wl_client *c,struct wl_resource *r,uint32_t id) {
    struct output *o=wl_resource_get_user_data(r);unsigned i=0;
    if(!o->electron){fail(r,"pointer not advertised for this diagnostic");return;}
    while(i<4&&o->pointers[i])i++;
    if(i==4){wl_client_post_no_memory(c);return;}
    struct wl_resource *p=wl_resource_create(c,&wl_pointer_interface,wl_resource_get_version(r),id);
    if(!p){wl_client_post_no_memory(c);return;}
    o->pointers[i]=p;wl_resource_set_implementation(p,&pointer_impl,o,pointer_destroyed);
    if(o->pointer_focused) {
        o->pointer_enter_serial[i]=wl_display_next_serial(o->display);
        wl_pointer_send_enter(p,o->pointer_enter_serial[i],o->surface,o->pointer_x,o->pointer_y);
        if(wl_resource_get_version(p)>=5)wl_pointer_send_frame(p);
    }
}
static void pointer_event(struct output *o,const struct os_window_event *e) {
    if(e->type==OS_WINDOW_OVERFLOW || e->type==OS_WINDOW_CLOSE || (e->type==OS_WINDOW_FOCUS&&!e->code)) {pointer_leave(o);return;}
    if(e->type!=OS_WINDOW_POINTER)return;
    if(o->input_trace && o->timing_logs++<64) fprintf(stderr,"K1_POINTER_EVENT inside=%u focused=%d buffer=%d x=%d y=%d\n",
        e->code,o->key_focused,o->surface_has_buffer,e->dx,e->dy);
    if(!e->code || !o->surface || !o->surface_has_buffer || !o->key_focused){pointer_leave(o);return;}
    wl_fixed_t x=wl_fixed_from_double((double)(e->dx-o->surface_left)/o->buffer_scale);
    wl_fixed_t y=wl_fixed_from_double((double)(e->dy-o->surface_top)/o->buffer_scale);
    uint32_t time=(uint32_t)now_ms();unsigned buttons=e->buttons&7;
    static const unsigned button_codes[3]={272,273,274};
    for(unsigned i=0;i<4;i++)if(o->pointers[i]) {
        struct wl_resource *p=o->pointers[i];
        if(!o->pointer_focused){o->pointer_enter_serial[i]=wl_display_next_serial(o->display);wl_pointer_send_enter(p,o->pointer_enter_serial[i],o->surface,x,y);}
        if(!o->pointer_focused || x!=o->pointer_x || y!=o->pointer_y)wl_pointer_send_motion(p,time,x,y);
        for(unsigned bit=0;bit<3;bit++)if((buttons^o->pointer_buttons)&(1u<<bit)) {
            uint32_t serial=wl_display_next_serial(o->display);clipboard_note_serial(o,serial);
            wl_pointer_send_button(p,serial,time,button_codes[bit],(buttons>>bit)&1);
        }
        if(e->wheel) {
            if(wl_resource_get_version(p)>=5) {
                wl_pointer_send_axis_source(p,WL_POINTER_AXIS_SOURCE_WHEEL);
                wl_pointer_send_axis_discrete(p,WL_POINTER_AXIS_VERTICAL_SCROLL,-e->wheel);
            }
            wl_pointer_send_axis(p,time,WL_POINTER_AXIS_VERTICAL_SCROLL,wl_fixed_from_int(-e->wheel*10));
        }
        if(wl_resource_get_version(p)>=5)wl_pointer_send_frame(p);
    }
    if(o->input_trace && e->wheel)printf("K1_POINTER wheel=%d x=%d y=%d\n",e->wheel,e->dx,e->dy);
    o->pointer_focused=1;o->pointer_x=x;o->pointer_y=y;o->pointer_buttons=buttons;
}
