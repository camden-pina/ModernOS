/* Included only by the original output adapter. No kernel or authority change. */
struct wm_binding { struct output *output; struct wl_resource *resource; uint32_t ping; };
static void xdg_reset(struct output *o) {
    struct xdg_state *x=&o->xdg;
    x->pending_count=0;x->initial=x->configured=x->mapped=0;x->acked_serial=0;
    x->geometry_set=x->geometry_dirty=x->hints_dirty=0;
    memset(x->min,0,sizeof(x->min));memset(x->max,0,sizeof(x->max));
    memset(x->pending_min,0,sizeof(x->pending_min));memset(x->pending_max,0,sizeof(x->pending_max));
    x->title[0]=x->app_id[0]=0;
}
static void blank_output(struct output *o) {
    for(int i=0;i<o->width*o->height;i++)o->pixels[i]=0x18202a;
    o->pixel_width=o->width;o->pixel_height=o->height;
    if(o->window.id) {
        struct os_window_frame f={.id=o->window.id,.width=o->width,.height=o->height,.stride=o->width,.pixels=(uintptr_t)o->pixels};
        if(!native(OS_WINDOW_PRESENT,&f,sizeof(f)))o->repaints++;
    }
}
static void xdg_send_configure(struct output *o) {
    struct xdg_state *x=&o->xdg;
    if(!x->surface || !x->toplevel)return;
    if(x->pending_count==8){wl_resource_post_no_memory(x->surface);return;}
    struct wl_array states;wl_array_init(&states);
    if(o->key_focused) {
        uint32_t *state=wl_array_add(&states,sizeof(*state));
        if(!state){wl_array_release(&states);wl_resource_post_no_memory(x->toplevel);return;}
        *state=XDG_TOPLEVEL_STATE_ACTIVATED;
    }
    /* Preserve maximize in the native restore record, while the client sees
       fullscreen as its current state. Chromium gives MAXIMIZED precedence
       when both states appear and then cannot toggle fullscreen off. */
    if((o->native_states&OS_WINDOW_STATE_MAXIMIZED) && !(o->native_states&OS_WINDOW_STATE_FULLSCREEN)) {
        uint32_t *state=wl_array_add(&states,sizeof(*state));
        if(!state){wl_array_release(&states);wl_resource_post_no_memory(x->toplevel);return;}
        *state=XDG_TOPLEVEL_STATE_MAXIMIZED;
    }
    if(o->native_states&OS_WINDOW_STATE_FULLSCREEN) {
        uint32_t *state=wl_array_add(&states,sizeof(*state));
        if(!state){wl_array_release(&states);wl_resource_post_no_memory(x->toplevel);return;}
        *state=XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
    int width=o->electron?o->width/o->scale:0,height=o->electron?o->height/o->scale:0;
    xdg_toplevel_send_configure(x->toplevel,width,height,&states);wl_array_release(&states);
    uint32_t serial=wl_display_next_serial(o->display);
    x->pending[x->pending_count++]=serial;x->initial=1;x->configures++;
    xdg_surface_send_configure(x->surface,serial);
    printf("P6_XDG_CONFIGURE serial=%u size=%dx%d pending=%u states=%u\n",serial,width,height,x->pending_count,o->native_states);
}
static int native_geometry(struct output *o) {
    if(!o->electron || !o->window.id)return 0;
    struct os_window_geometry geometry={.id=o->window.id};
    if(native(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))) {
        if(errno==EPIPE || errno==ENOENT || errno==ENODEV)o->closed=1;
        else o->fatal=1;
        return -1;
    }
    if(!geometry.requested_width || !geometry.requested_height ||
       geometry.requested_width>MAX_WIDTH || geometry.requested_height>MAX_HEIGHT ||
       geometry.requested_width%o->scale || geometry.requested_height%o->scale) {o->fatal=1;return -1;}
    int changed=o->width!=(int)geometry.requested_width || o->height!=(int)geometry.requested_height ||
                o->native_states!=geometry.states;
    if(changed) {
        o->width=geometry.requested_width;o->height=geometry.requested_height;o->native_states=geometry.states;
        o->native_configure_dirty=1;
    }
    /* Coalesce native drag/focus changes while the client's bounded configure
       queue is full. Client-generated protocol floods retain their old limit. */
    if(o->native_configure_dirty && o->xdg.initial && !o->closed && o->xdg.pending_count<8) {
        xdg_send_configure(o);o->native_configure_dirty=0;
    }
    return changed;
}
static void xdg_focus_configure(struct output *o) {
    if(o->electron && !o->closed && o->xdg.initial && o->xdg.toplevel) {
        o->native_configure_dirty=1;native_geometry(o);
    }
}
static void xdg_surface_freed(struct wl_resource *r) {
    struct output *o=wl_resource_get_user_data(r);
    o->xdg.surface=NULL;o->xdg.owner=NULL;xdg_reset(o);
}
static void xdg_top_freed(struct wl_resource *r) {
    struct output *o=wl_resource_get_user_data(r);
    keyboard_leave(o);output_surface_state(o,0);
    o->xdg.toplevel=NULL;blank_output(o);pending_unlink(o);
    o->attached=o->lost=o->surface_has_buffer=0;xdg_reset(o);
}
static void xdg_destroy(struct wl_client *c,struct wl_resource *r) {
    (void)c;struct output *o=wl_resource_get_user_data(r);
    if(o->xdg.toplevel){wl_resource_post_error(r,XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,"destroy toplevel before xdg_surface");return;}
    wl_resource_destroy(r);
}
static void top_parent(struct wl_client *c,struct wl_resource *r,struct wl_resource *parent) {
    (void)c;
    if(parent==r)wl_resource_post_error(r,XDG_TOPLEVEL_ERROR_INVALID_PARENT,"toplevel cannot parent itself");
    else if(parent)fail(r,"multiple toplevels/parents unsupported");
}
static void top_title(struct wl_client *c,struct wl_resource *r,const char *title) {
    (void)c;struct output *o=wl_resource_get_user_data(r);snprintf(o->xdg.title,sizeof(o->xdg.title),"%s",title);
}
static void top_appid(struct wl_client *c,struct wl_resource *r,const char *id) {
    (void)c;struct output *o=wl_resource_get_user_data(r);snprintf(o->xdg.app_id,sizeof(o->xdg.app_id),"%s",id);
}
static void top_menu(struct wl_client *c,struct wl_resource *r,struct wl_resource *seat,uint32_t serial,int32_t x,int32_t y) {
    (void)c;(void)r;(void)seat;(void)serial;(void)x;(void)y; /* No advertised seat or valid input serial. */
}
static void top_move(struct wl_client *c,struct wl_resource *r,struct wl_resource *seat,uint32_t serial) {
    (void)c;(void)r;(void)seat;(void)serial;
}
static void top_resize(struct wl_client *c,struct wl_resource *r,struct wl_resource *seat,uint32_t serial,uint32_t edges) {
    (void)c;(void)seat;(void)serial;
    if(edges!=0 && edges!=1 && edges!=2 && edges!=4 && edges!=5 && edges!=6 && edges!=8 && edges!=9 && edges!=10)
        wl_resource_post_error(r,XDG_TOPLEVEL_ERROR_INVALID_RESIZE_EDGE,"invalid edge enum");
    /* Even a valid request has no valid seat serial in this diagnostic. */
}
static void top_min(struct wl_client *c,struct wl_resource *r,int32_t w,int32_t h) {
    (void)c;struct output *o=wl_resource_get_user_data(r);
    if(w<0 || h<0){wl_resource_post_error(r,XDG_TOPLEVEL_ERROR_INVALID_SIZE,"negative minimum");return;}
    o->xdg.pending_min[0]=w;o->xdg.pending_min[1]=h;o->xdg.hints_dirty=1;
}
static void top_max(struct wl_client *c,struct wl_resource *r,int32_t w,int32_t h) {
    (void)c;struct output *o=wl_resource_get_user_data(r);
    if(w<0 || h<0){wl_resource_post_error(r,XDG_TOPLEVEL_ERROR_INVALID_SIZE,"negative maximum");return;}
    o->xdg.pending_max[0]=w;o->xdg.pending_max[1]=h;o->xdg.hints_dirty=1;
}
static void native_state_request(struct wl_resource *r,unsigned action) {
    struct output *o=wl_resource_get_user_data(r);
    if(o->electron && !o->window.id){o->native_action=action;return;}
    if(o->electron && o->window.id) {
        struct os_window_request request={o->window.id,action};
        if(native(OS_WINDOW_REQUEST_STATE,&request,sizeof(request))) {
            if(errno==EPIPE || errno==ENOENT || errno==ENODEV)o->closed=1;else o->fatal=1;
        }
    } else if(o->xdg.initial)xdg_send_configure(o);
}
static void top_maximize(struct wl_client *c,struct wl_resource *r) { (void)c;native_state_request(r,OS_WINDOW_REQUEST_MAXIMIZE); }
static void top_restore(struct wl_client *c,struct wl_resource *r) { (void)c;native_state_request(r,OS_WINDOW_REQUEST_RESTORE); }
static void top_fullscreen(struct wl_client *c,struct wl_resource *r,struct wl_resource *output) {
    (void)c;(void)output;native_state_request(r,OS_WINDOW_REQUEST_FULLSCREEN);
}
static void top_unfullscreen(struct wl_client *c,struct wl_resource *r) { (void)c;native_state_request(r,OS_WINDOW_REQUEST_UNFULLSCREEN); }
static void top_minimize(struct wl_client *c,struct wl_resource *r) {
    (void)c;struct output *o=wl_resource_get_user_data(r);
    if(o->electron && o->window.id)native_state_request(r,OS_WINDOW_REQUEST_MINIMIZE);
}
static const struct xdg_toplevel_interface xdg_top_impl={
    .destroy=destroy_resource,.set_parent=top_parent,.set_title=top_title,.set_app_id=top_appid,
    .show_window_menu=top_menu,.move=top_move,.resize=top_resize,.set_max_size=top_max,.set_min_size=top_min,
    .set_maximized=top_maximize,.unset_maximized=top_restore,
    .set_fullscreen=top_fullscreen,.unset_fullscreen=top_unfullscreen,.set_minimized=top_minimize
};
static void xdg_get_top(struct wl_client *c,struct wl_resource *r,uint32_t id) {
    struct output *o=wl_resource_get_user_data(r);
    if(o->xdg.toplevel || o->role==1){wl_resource_post_error(r,XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,"surface already has a role object");return;}
    o->xdg.toplevel=wl_resource_create(c,&xdg_toplevel_interface,1,id);
    if(!o->xdg.toplevel){wl_client_post_no_memory(c);return;}
    o->role=2;o->selected=1;xdg_reset(o);
    wl_resource_set_implementation(o->xdg.toplevel,&xdg_top_impl,o,xdg_top_freed);
}
static void xdg_get_popup(struct wl_client *c,struct wl_resource *r,uint32_t id,struct wl_resource *parent,struct wl_resource *positioner) {
    (void)c;(void)id;(void)parent;(void)positioner;fail(r,"popups unsupported in single-toplevel output");
}
static void xdg_geometry(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h) {
    (void)c;struct output *o=wl_resource_get_user_data(r);
    if(!o->xdg.toplevel){wl_resource_post_error(r,XDG_SURFACE_ERROR_NOT_CONSTRUCTED,"toplevel required");return;}
    if(w<=0 || h<=0){wl_resource_post_error(r,XDG_SURFACE_ERROR_INVALID_SIZE,"geometry must be positive");return;}
    o->xdg.pending_geometry=(struct window_geometry){x,y,w,h};o->xdg.geometry_dirty=1;
}
static void xdg_ack(struct wl_client *c,struct wl_resource *r,uint32_t serial) {
    (void)c;struct output *o=wl_resource_get_user_data(r);struct xdg_state *x=&o->xdg;
    if(!x->toplevel){wl_resource_post_error(r,XDG_SURFACE_ERROR_NOT_CONSTRUCTED,"toplevel required");return;}
    unsigned i;for(i=0;i<x->pending_count;i++)if(x->pending[i]==serial)break;
    if(i==x->pending_count){wl_resource_post_error(r,XDG_SURFACE_ERROR_INVALID_SERIAL,"unknown or already consumed serial");return;}
    x->pending_count-=i+1;memmove(x->pending,x->pending+i+1,x->pending_count*sizeof(x->pending[0]));
    x->acked_serial=serial;x->configured=1;x->acks++;
    printf("P6_XDG_ACK serial=%u remaining=%u\n",serial,x->pending_count);
}
static const struct xdg_surface_interface xdg_surface_impl={.destroy=xdg_destroy,.get_toplevel=xdg_get_top,.get_popup=xdg_get_popup,.set_window_geometry=xdg_geometry,.ack_configure=xdg_ack};
static void wm_freed(struct wl_resource *r) {
    struct wm_binding *b=wl_resource_get_user_data(r);if(b->output->xdg.owner==r)b->output->xdg.owner=NULL;
    b->output->xdg.bindings--;free(b);
}
static void wm_destroy(struct wl_client *c,struct wl_resource *r) {
    (void)c;struct wm_binding *b=wl_resource_get_user_data(r);
    if(b->output->xdg.owner==r && b->output->xdg.surface){wl_resource_post_error(r,XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,"xdg surfaces remain alive");return;}
    wl_resource_destroy(r);
}
static void wm_positioner(struct wl_client *c,struct wl_resource *r,uint32_t id) { (void)c;(void)id;fail(r,"positioners/popups unsupported"); }
static void wm_surface(struct wl_client *c,struct wl_resource *r,uint32_t id,struct wl_resource *surface) {
    struct wm_binding *b=wl_resource_get_user_data(r);struct output *o=b->output;
    /* Chromium may allocate its cursor surface before its application surface.
       Classify the actual toplevel at role assignment, before any app buffer. */
    if(surface==o->cursor_surface && !o->xdg.surface && !o->role && !o->pending && !o->surface_has_buffer) {
        struct wl_resource *cursor=o->surface;o->surface=o->cursor_surface;o->cursor_surface=cursor;
    }
    if(surface!=o->surface || o->xdg.surface || o->role){wl_resource_post_error(r,XDG_WM_BASE_ERROR_ROLE,"surface already assigned/claimed");return;}
    if(o->pending || o->surface_has_buffer){wl_resource_post_error(r,XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,"buffer attached before xdg construction");return;}
    o->xdg.surface=wl_resource_create(c,&xdg_surface_interface,1,id);
    if(!o->xdg.surface){wl_client_post_no_memory(c);return;}
    o->xdg.owner=r;wl_resource_set_implementation(o->xdg.surface,&xdg_surface_impl,o,xdg_surface_freed);
}
static void wm_pong(struct wl_client *c,struct wl_resource *r,uint32_t serial) {
    (void)c;struct wm_binding *b=wl_resource_get_user_data(r);if(serial==b->ping)b->output->xdg.pongs++;
}
static const struct xdg_wm_base_interface wm_impl={.destroy=wm_destroy,.create_positioner=wm_positioner,.get_xdg_surface=wm_surface,.pong=wm_pong};
static void bind_xdg(struct wl_client *c,void *data,uint32_t version,uint32_t id) {
    (void)version;struct output *o=data;
    if(o->xdg.bindings==8){wl_client_post_no_memory(c);return;}
    struct wm_binding *b=calloc(1,sizeof(*b));if(!b){wl_client_post_no_memory(c);return;}
    b->resource=wl_resource_create(c,&xdg_wm_base_interface,1,id);
    if(!b->resource){free(b);wl_client_post_no_memory(c);return;}
    b->output=o;b->ping=wl_display_next_serial(o->display);o->xdg.bindings++;
    wl_resource_set_implementation(b->resource,&wm_impl,b,wm_freed);xdg_wm_base_send_ping(b->resource,b->ping);
}
/* -1 error, 1 lifecycle-only commit handled, 0 proceed to a buffer copy. */
static int xdg_commit(struct output *o) {
    struct xdg_state *x=&o->xdg;
    if(!x->surface)return 0;
    if(!x->toplevel){wl_resource_post_error(x->surface,XDG_SURFACE_ERROR_NOT_CONSTRUCTED,"toplevel required before commit");return -1;}
    if(x->hints_dirty) {
        for(unsigned i=0;i<2;i++)if(x->pending_max[i] && x->pending_min[i]>x->pending_max[i]) {
            wl_resource_post_error(x->toplevel,XDG_TOPLEVEL_ERROR_INVALID_SIZE,"minimum exceeds maximum");return -1;
        }
        memcpy(x->min,x->pending_min,sizeof(x->min));memcpy(x->max,x->pending_max,sizeof(x->max));x->hints_dirty=0;
    }
    if(x->geometry_dirty){x->geometry=x->pending_geometry;x->geometry_set=1;x->geometry_dirty=0;}
    if(o->attached && !o->pending && x->mapped) {
        keyboard_leave(o);output_surface_state(o,0);
        blank_output(o);pending_unlink(o);o->attached=0;o->surface_has_buffer=0;xdg_reset(o);
        printf("P6_XDG_UNMAPPED\n");return 1;
    }
    if(o->attached && !o->pending){pending_unlink(o);o->attached=0;}
    if(o->attached && o->pending && !x->configured){wl_resource_post_error(x->surface,XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,"ack configure before buffer commit");return -1;}
    if(!x->initial){xdg_send_configure(o);return 1;}
    return 0;
}
