/* Bounded cursor copies remain private to the owned native window. No client
 * receives a global pointer, display handle, or writable SHM mapping. */
static void cursor_unlink(struct output *o) {
    if(o->cursor_pending)wl_list_remove(&o->cursor_pending_destroy.link);
    wl_list_init(&o->cursor_pending_destroy.link);o->cursor_pending=NULL;
}
static void cursor_pending_gone(struct wl_listener *listener,void *unused) {
    (void)unused;struct output *o=wl_container_of(listener,o,cursor_pending_destroy);
    cursor_unlink(o);
}
static void cursor_apply(struct output *o) {
    if(!o->window.id || !o->pointer_focused)return;
    struct os_window_cursor cursor={.id=o->window.id,.mode=o->cursor_mode};
    if(cursor.mode==OS_WINDOW_CURSOR_BITMAP) {
        if(!o->cursor_width || !o->cursor_height)cursor.mode=OS_WINDOW_CURSOR_HIDDEN;
        else {
        int64_t x=(int64_t)o->cursor_hot_x*o->cursor_scale,y=(int64_t)o->cursor_hot_y*o->cursor_scale;
        if(x<-(int64_t)OS_WINDOW_CURSOR_MAX || x>OS_WINDOW_CURSOR_MAX || y<-(int64_t)OS_WINDOW_CURSOR_MAX || y>OS_WINDOW_CURSOR_MAX)return;
        cursor.width=o->cursor_width;cursor.height=o->cursor_height;cursor.stride=o->cursor_width;
        cursor.hotspot_x=x;cursor.hotspot_y=y;cursor.pixels=(uintptr_t)o->cursor_pixels;
        }
    }
    if(native(OS_WINDOW_CURSOR,&cursor,sizeof(cursor)) && errno!=EPIPE && errno!=ENOENT && errno!=ENODEV) {
        fprintf(stderr,"P6_NATIVE_CURSOR_ERROR errno=%d\n",errno);o->fatal=1;
    }
}
static void cursor_commit(struct output *o,struct wl_resource *surface) {
    struct wl_resource *buffer=o->cursor_pending;
    if(!buffer){cursor_apply(o);return;}
    struct wl_shm_buffer *shm=wl_shm_buffer_get(buffer);
    int width=wl_shm_buffer_get_width(shm),height=wl_shm_buffer_get_height(shm),stride=wl_shm_buffer_get_stride(shm);
    uint32_t format=wl_shm_buffer_get_format(shm);
    if(width<1 || height<1 || width>(int)OS_WINDOW_CURSOR_MAX || height>(int)OS_WINDOW_CURSOR_MAX ||
       stride<width*4 || stride>(int)OS_WINDOW_CURSOR_MAX*4 ||
       (format!=WL_SHM_FORMAT_ARGB8888 && format!=WL_SHM_FORMAT_XRGB8888)) {fail(surface,"cursor format/stride exceeds bounds");return;}
    wl_shm_buffer_begin_access(shm);const uint8_t *data=wl_shm_buffer_get_data(shm);
    for(int y=0;y<height;y++)for(int x=0;x<width;x++) {
        uint32_t pixel;memcpy(&pixel,data+(size_t)y*stride+x*4,4);
        if(format==WL_SHM_FORMAT_XRGB8888)pixel|=0xff000000u;
        o->cursor_pixels[(size_t)y*width+x]=pixel;
    }
    wl_shm_buffer_end_access(shm);
    o->cursor_width=width;o->cursor_height=height;o->cursor_scale=o->cursor_pending_scale;
    cursor_unlink(o);wl_buffer_send_release(buffer);cursor_apply(o);
}
