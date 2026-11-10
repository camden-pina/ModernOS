/* Focus-delegated UTF-8 clipboard broker. The client gets no clipboard syscall
 * authority. Every transfer rechecks the actual native window generation;
 * selection writes additionally require a recent issued input serial.
 * No named sockets, external devices, drag ownership or blocking pipe I/O.
 */
enum { CLIP_SOURCES=4,CLIP_OFFERS=8,CLIP_DEVICES=4,CLIP_MANAGERS=2,CLIP_WRITES=4,CLIP_SERIALS=64 };
struct clipboard_source {struct clipboard_state *state;struct wl_resource *resource;unsigned mimes;};
struct clipboard_offer {struct clipboard_state *state;struct wl_resource *resource;size_t length;uint8_t text[OS_CLIPBOARD_LIMIT];};
struct clipboard_write {struct clipboard_state *state;struct clipboard_offer *offer;struct wl_event_source *watch;int fd;size_t offset;uint64_t deadline,generation;};
struct clipboard_state {
    struct output *output;
    struct wl_resource *managers[CLIP_MANAGERS],*devices[CLIP_DEVICES];
    struct clipboard_source sources[CLIP_SOURCES],*selection;
    struct clipboard_offer offers[CLIP_OFFERS];
    struct clipboard_write writes[CLIP_WRITES];
    struct wl_event_source *pull_watch;
    struct clipboard_source *pull_source;
    int pull_fd;size_t pull_length;uint64_t pull_deadline,pull_generation,pull_native_generation;
    uint32_t serials[CLIP_SERIALS];unsigned serial_next;
    uint64_t native_generation,next_poll;
    uint8_t pull_text[OS_CLIPBOARD_LIMIT+1],native_text[OS_CLIPBOARD_LIMIT];size_t native_length;
};
_Static_assert(sizeof(struct clipboard_state)<=1024*1024,"clipboard private metadata and text pool bound");
static bool clipboard_owned_focus(struct clipboard_state *s,uint64_t expected) {
    struct output *o=s->output;
    if(!o->key_focused || !o->window.id || !o->surface_has_buffer)return false;
    struct os_window_keyboard q={.version=OS_WINDOW_KEYBOARD_VERSION,.size=sizeof(q),.id=o->window.id};
    return !native(OS_WINDOW_KEYBOARD_STATE,&q,sizeof(q)) && q.generation==o->key_generation && (!expected || q.generation==expected);
}
static long clipboard_native(unsigned operation,void *text,size_t size,uint64_t expected) {
    return syscall(COAST_SYSCALL_CLIPBOARD,operation,text,size,expected);
}
static void clipboard_note_serial(struct output *o,uint32_t serial) {
    if(o->clipboard && serial)o->clipboard->serials[o->clipboard->serial_next++%CLIP_SERIALS]=serial;
}
static void clipboard_write_close(struct clipboard_write *w) {
    if(w->watch)wl_event_source_remove(w->watch);
    if(w->fd>=0)close(w->fd);
    w->watch=NULL;w->fd=-1;w->offer=NULL;
}
static void clipboard_pull_close(struct clipboard_state *s) {
    if(s->pull_watch)wl_event_source_remove(s->pull_watch);
    if(s->pull_fd>=0)close(s->pull_fd);
    s->pull_watch=NULL;s->pull_fd=-1;s->pull_source=NULL;s->pull_length=0;
}
static void clipboard_focus_lost(struct output *o) {
    struct clipboard_state *s=o->clipboard;if(!s)return;
    /* Finish an already authorized copy even if the user immediately switches
       windows. Its bounded lease cannot start another transfer or read the
       clipboard, and the atomic generation check preserves a newer copy. */
    for(unsigned i=0;i<CLIP_WRITES;i++)clipboard_write_close(&s->writes[i]);
    memset(s->serials,0,sizeof(s->serials));s->serial_next=0;s->native_generation=0;
}
static unsigned clipboard_mime(const char *mime) {
    return !strcmp(mime,"text/plain;charset=utf-8")?1:!strcmp(mime,"text/plain")?2:!strcmp(mime,"UTF8_STRING")?4:0;
}
static const char *clipboard_source_mime(unsigned mimes) {return mimes&1?"text/plain;charset=utf-8":mimes&2?"text/plain":"UTF8_STRING";}
static void clipboard_source_offer(struct wl_client *client,struct wl_resource *resource,const char *mime) {
    struct clipboard_source *source=wl_resource_get_user_data(resource);source->mimes|=clipboard_mime(mime);
}
static void clipboard_source_destroyed(struct wl_resource *resource) {
    struct clipboard_source *source=wl_resource_get_user_data(resource);struct clipboard_state *s=source->state;
    if(s->pull_source==source)clipboard_pull_close(s);
    if(s->selection==source)s->selection=NULL;
    source->resource=NULL;source->mimes=0;
}
static void clipboard_source_actions(struct wl_client *client,struct wl_resource *resource,uint32_t actions) { }
static const struct wl_data_source_interface clipboard_source_impl={.offer=clipboard_source_offer,.destroy=destroy_resource,.set_actions=clipboard_source_actions};
static void clipboard_create_source(struct wl_client *client,struct wl_resource *resource,uint32_t id) {
    struct clipboard_state *s=wl_resource_get_user_data(resource);unsigned i=0;
    while(i<CLIP_SOURCES && s->sources[i].resource)i++;
    if(i==CLIP_SOURCES){wl_client_post_no_memory(client);return;}
    struct wl_resource *r=wl_resource_create(client,&wl_data_source_interface,1,id);
    if(!r){wl_client_post_no_memory(client);return;}
    s->sources[i]=(struct clipboard_source){.state=s,.resource=r};
    wl_resource_set_implementation(r,&clipboard_source_impl,&s->sources[i],clipboard_source_destroyed);
}
static int clipboard_write_ready(int fd,uint32_t mask,void *data) {
    struct clipboard_write *w=data;struct clipboard_state *s=w->state;
    if(!w->offer || !w->offer->resource || !clipboard_owned_focus(s,w->generation) || now_ms()>=w->deadline) {clipboard_write_close(w);return 0;}
    while(w->offset<w->offer->length) {
        ssize_t n=write(fd,w->offer->text+w->offset,w->offer->length-w->offset);
        if(n>0){w->offset+=n;continue;}
        if(n<0 && errno==EINTR)continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return 0;
        clipboard_write_close(w);return 0;
    }
    if(s->output->input_trace)printf("CLIPBOARD_CLIENT_READ bytes=%zu\n",w->offset);
    clipboard_write_close(w);return 0;
}
static void clipboard_offer_accept(struct wl_client *client,struct wl_resource *resource,uint32_t serial,const char *mime) { }
static void clipboard_offer_receive(struct wl_client *client,struct wl_resource *resource,const char *mime,int32_t fd) {
    struct clipboard_offer *offer=wl_resource_get_user_data(resource);struct clipboard_state *s=offer->state;
    struct stat st;int flags=fcntl(fd,F_GETFL);
    /* Clipboard receive accepts an anonymous pipe write end only. Even an
       intentionally supplied file/terminal descriptor is never a write proxy. */
    if(!clipboard_mime(mime) || !clipboard_owned_focus(s,0) || flags<0 ||
       (flags&O_ACCMODE)!=O_WRONLY || fstat(fd,&st) || !S_ISFIFO(st.st_mode) ||
       fcntl(fd,F_SETFL,flags|O_NONBLOCK)) {close(fd);return;}
    unsigned i=0;while(i<CLIP_WRITES && s->writes[i].fd>=0)i++;
    if(i==CLIP_WRITES){close(fd);return;}
    struct clipboard_write *w=&s->writes[i];
    *w=(struct clipboard_write){.state=s,.offer=offer,.fd=fd,.deadline=now_ms()+5000,.generation=s->output->key_generation};
    w->watch=wl_event_loop_add_fd(wl_display_get_event_loop(s->output->display),fd,WL_EVENT_WRITABLE,clipboard_write_ready,w);
    if(!w->watch)clipboard_write_close(w);
    else clipboard_write_ready(fd,WL_EVENT_WRITABLE,w);
}
static void clipboard_offer_destroyed(struct wl_resource *resource) {
    struct clipboard_offer *offer=wl_resource_get_user_data(resource);struct clipboard_state *s=offer->state;
    for(unsigned i=0;i<CLIP_WRITES;i++)if(s->writes[i].offer==offer)clipboard_write_close(&s->writes[i]);
    offer->resource=NULL;offer->length=0;
}
static void clipboard_offer_finish(struct wl_client *client,struct wl_resource *resource) { }
static void clipboard_offer_actions(struct wl_client *client,struct wl_resource *resource,uint32_t actions,uint32_t preferred) { }
static const struct wl_data_offer_interface clipboard_offer_impl={.accept=clipboard_offer_accept,.receive=clipboard_offer_receive,.destroy=destroy_resource,.finish=clipboard_offer_finish,.set_actions=clipboard_offer_actions};
static bool clipboard_send_offer(struct clipboard_state *s,struct wl_resource *device) {
    if(!s->native_length){wl_data_device_send_selection(device,NULL);return true;}
    unsigned i=0;while(i<CLIP_OFFERS && s->offers[i].resource)i++;
    if(i==CLIP_OFFERS){wl_data_device_send_selection(device,NULL);return false;}
    struct wl_resource *r=wl_resource_create(wl_resource_get_client(device),&wl_data_offer_interface,1,0);
    if(!r)return false;
    struct clipboard_offer *offer=&s->offers[i];offer->state=s;offer->resource=r;offer->length=s->native_length;
    memcpy(offer->text,s->native_text,offer->length);
    wl_resource_set_implementation(r,&clipboard_offer_impl,offer,clipboard_offer_destroyed);
    wl_data_device_send_data_offer(device,r);
    wl_data_offer_send_offer(r,"text/plain;charset=utf-8");wl_data_offer_send_offer(r,"text/plain");wl_data_offer_send_offer(r,"UTF8_STRING");
    wl_data_device_send_selection(device,r);return true;
}
static int clipboard_pull_ready(int fd,uint32_t mask,void *data) {
    struct clipboard_state *s=data;
    if(!s->pull_source || s->pull_source!=s->selection || now_ms()>=s->pull_deadline) {clipboard_pull_close(s);return 0;}
    for(;;) {
        ssize_t n=read(fd,s->pull_text+s->pull_length,sizeof(s->pull_text)-s->pull_length);
        if(n>0) {
            s->pull_length+=n;
            if(s->pull_length>OS_CLIPBOARD_LIMIT){if(s->output->input_trace)puts("CLIPBOARD_CLIENT_OVERSIZED_REJECTED");clipboard_pull_close(s);return 0;}
            continue;
        }
        if(n<0 && errno==EINTR)continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return 0;
        if(!n) {
            struct os_clipboard_info info;
            if(clipboard_native(OS_CLIPBOARD_WRITE_IF_CURRENT,s->pull_text,s->pull_length,s->pull_native_generation)==(long)s->pull_length) {
                if(!clipboard_native(OS_CLIPBOARD_INFO,&info,sizeof(info),0))s->native_generation=info.generation;
                if(s->output->input_trace)printf("CLIPBOARD_NATIVE_WRITE bytes=%zu\n",s->pull_length);
            }
        }
        clipboard_pull_close(s);return 0;
    }
}
static void clipboard_set_selection(struct wl_client *client,struct wl_resource *resource,struct wl_resource *selection,uint32_t serial) {
    struct clipboard_state *s=wl_resource_get_user_data(resource);bool issued=false;
    for(unsigned i=0;i<CLIP_SERIALS;i++)issued|=serial && s->serials[i]==serial;
    if(!issued || !clipboard_owned_focus(s,0))return;
    struct clipboard_source *source=NULL;
    if(selection) {
        if(!wl_resource_instance_of(selection,&wl_data_source_interface,&clipboard_source_impl))return;
        source=wl_resource_get_user_data(selection);
        if(source->state!=s || !source->mimes)return;
    }
    struct os_clipboard_info info;
    if(clipboard_native(OS_CLIPBOARD_INFO,&info,sizeof(info),0))return;
    clipboard_pull_close(s);
    if(s->selection && s->selection!=source && s->selection->resource)wl_data_source_send_cancelled(s->selection->resource);
    s->selection=source;s->native_generation=info.generation;
    if(!source) {
        if(!clipboard_native(OS_CLIPBOARD_WRITE,NULL,0,0) && !clipboard_native(OS_CLIPBOARD_INFO,&info,sizeof(info),0))s->native_generation=info.generation;
        return;
    }
    int pipefd[2];if(pipe2(pipefd,O_CLOEXEC))return;
    if(fcntl(pipefd[0],F_SETFL,O_NONBLOCK)){close(pipefd[0]);close(pipefd[1]);return;}
    s->pull_fd=pipefd[0];s->pull_source=source;s->pull_length=0;s->pull_generation=s->output->key_generation;
    s->pull_native_generation=info.generation;s->pull_deadline=now_ms()+5000;
    s->pull_watch=wl_event_loop_add_fd(wl_display_get_event_loop(s->output->display),pipefd[0],WL_EVENT_READABLE,clipboard_pull_ready,s);
    if(!s->pull_watch){close(pipefd[1]);clipboard_pull_close(s);return;}
    wl_data_source_send_send(source->resource,clipboard_source_mime(source->mimes),pipefd[1]);close(pipefd[1]);
}
static void clipboard_start_drag(struct wl_client *client,struct wl_resource *resource,struct wl_resource *source,struct wl_resource *origin,struct wl_resource *icon,uint32_t serial) {
    /* No compositor drag operation is started or global drag grant issued. */
    if(source)wl_data_source_send_cancelled(source);
}
static void clipboard_device_destroyed(struct wl_resource *resource) {
    struct clipboard_state *s=wl_resource_get_user_data(resource);
    for(unsigned i=0;i<CLIP_DEVICES;i++)if(s->devices[i]==resource)s->devices[i]=NULL;
}
static const struct wl_data_device_interface clipboard_device_impl={.start_drag=clipboard_start_drag,.set_selection=clipboard_set_selection,.release=destroy_resource};
static void clipboard_get_device(struct wl_client *client,struct wl_resource *resource,uint32_t id,struct wl_resource *seat) {
    struct clipboard_state *s=wl_resource_get_user_data(resource);
    if(!wl_resource_instance_of(seat,&wl_seat_interface,&seat_impl) || wl_resource_get_user_data(seat)!=s->output || wl_resource_get_client(seat)!=client){fail(resource,"clipboard seat must be owned");return;}
    unsigned i=0;while(i<CLIP_DEVICES && s->devices[i])i++;
    if(i==CLIP_DEVICES){wl_client_post_no_memory(client);return;}
    struct wl_resource *r=wl_resource_create(client,&wl_data_device_interface,1,id);
    if(!r){wl_client_post_no_memory(client);return;}
    s->devices[i]=r;wl_resource_set_implementation(r,&clipboard_device_impl,s,clipboard_device_destroyed);
    s->native_generation=0;
}
static const struct wl_data_device_manager_interface clipboard_manager_impl={.create_data_source=clipboard_create_source,.get_data_device=clipboard_get_device};
static void clipboard_manager_destroyed(struct wl_resource *resource) {
    struct clipboard_state *s=wl_resource_get_user_data(resource);
    for(unsigned i=0;i<CLIP_MANAGERS;i++)if(s->managers[i]==resource)s->managers[i]=NULL;
}
static void clipboard_bind(struct wl_client *client,void *data,uint32_t version,uint32_t id) {
    struct clipboard_state *s=data;unsigned i=0;
    while(i<CLIP_MANAGERS && s->managers[i])i++;
    if(i==CLIP_MANAGERS){wl_client_post_no_memory(client);return;}
    struct wl_resource *r=wl_resource_create(client,&wl_data_device_manager_interface,1,id);
    if(!r){wl_client_post_no_memory(client);return;}
    s->managers[i]=r;wl_resource_set_implementation(r,&clipboard_manager_impl,s,clipboard_manager_destroyed);
}
static int clipboard_init(struct output *o) {
    if(!o->electron)return 0;
    struct clipboard_state *s=calloc(1,sizeof(*s));if(!s)return -1;
    o->clipboard=s;s->output=o;s->pull_fd=-1;
    for(unsigned i=0;i<CLIP_WRITES;i++){s->writes[i].state=s;s->writes[i].fd=-1;}
    return wl_global_create(o->display,&wl_data_device_manager_interface,1,s,clipboard_bind)?0:-1;
}
static void clipboard_poll(struct output *o) {
    struct clipboard_state *s=o->clipboard;if(!s)return;
    uint64_t now=now_ms();
    if(s->pull_fd>=0 && now>=s->pull_deadline)clipboard_pull_close(s);
    for(unsigned i=0;i<CLIP_WRITES;i++)if(s->writes[i].fd>=0 && now>=s->writes[i].deadline)clipboard_write_close(&s->writes[i]);
    if(now<s->next_poll || !clipboard_owned_focus(s,0))return;
    s->next_poll=now+100;
    struct os_clipboard_info info;if(clipboard_native(OS_CLIPBOARD_INFO,&info,sizeof(info),0) || info.bytes>OS_CLIPBOARD_LIMIT || info.generation==s->native_generation)return;
    if(s->pull_source && info.generation==s->pull_native_generation)return;
    long n=clipboard_native(OS_CLIPBOARD_READ,s->native_text,sizeof(s->native_text),info.generation);if(n<0)return;
    s->native_length=n;
    if(s->selection && s->selection->resource)wl_data_source_send_cancelled(s->selection->resource);
    s->selection=NULL;clipboard_pull_close(s);bool complete=true;
    for(unsigned i=0;i<CLIP_DEVICES;i++)if(s->devices[i])complete&=clipboard_send_offer(s,s->devices[i]);
    if(complete)s->native_generation=info.generation;
}
static void clipboard_shutdown(struct output *o) {
    struct clipboard_state *s=o->clipboard;if(!s)return;
    clipboard_focus_lost(o);clipboard_pull_close(s);unsigned live=0;
    for(unsigned i=0;i<CLIP_SOURCES;i++)live+=s->sources[i].resource!=NULL;
    for(unsigned i=0;i<CLIP_OFFERS;i++)live+=s->offers[i].resource!=NULL;
    for(unsigned i=0;i<CLIP_DEVICES;i++)live+=s->devices[i]!=NULL;
    for(unsigned i=0;i<CLIP_MANAGERS;i++)live+=s->managers[i]!=NULL;
    printf("CLIPBOARD_RESOURCE_CLEANUP live=%u\n",live);if(live)o->fatal=1;
    free(s);o->clipboard=NULL;
}
