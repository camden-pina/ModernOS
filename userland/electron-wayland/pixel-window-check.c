#define _GNU_SOURCE
#include <uapi/window.h>
#include <uapi/abi.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if(!(x)){fprintf(stderr,"PIXEL_WINDOW_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;} } while(0)
static long op(unsigned n,void *p,size_t size){return syscall(OS_WINDOW_SYSCALL,n,p,size);}
static uint32_t pattern(unsigned x,unsigned y){return ((x&255)<<16)|((y&255)<<8)|((x^y)&255);}
static int cursor_check(unsigned id,uint32_t *copy) {
    uint32_t small[12];for(unsigned i=0;i<12;i++)small[i]=0xff000000u|i*0x10101;
    struct os_window_cursor cursor={.id=id,.mode=OS_WINDOW_CURSOR_BITMAP,.width=4,.height=3,.stride=4,.hotspot_x=1,.hotspot_y=2,.pixels=(uintptr_t)small};
    CHECK(!op(OS_WINDOW_CURSOR,&cursor,sizeof(cursor)));
    struct os_window_cursor query={.id=id,.pixels=(uintptr_t)copy};
    CHECK(!op(OS_WINDOW_SERVER_CURSOR,&query,sizeof(query))&&query.sequence==1&&query.mode==OS_WINDOW_CURSOR_BITMAP&&query.width==4&&query.height==3&&query.hotspot_x==1&&query.hotspot_y==2&&!memcmp(small,copy,sizeof(small)));
    cursor.pixels=1;errno=0;CHECK(op(OS_WINDOW_CURSOR,&cursor,sizeof(cursor))==-1&&errno==EFAULT);cursor.pixels=(uintptr_t)small;
    small[0]=0x10ff0000;errno=0;CHECK(op(OS_WINDOW_CURSOR,&cursor,sizeof(cursor))==-1&&errno==EINVAL);small[0]=0xff000000;
    cursor.width=129;errno=0;CHECK(op(OS_WINDOW_CURSOR,&cursor,sizeof(cursor))==-1&&errno==EINVAL);cursor.width=4;
    cursor.hotspot_x=129;errno=0;CHECK(op(OS_WINDOW_CURSOR,&cursor,sizeof(cursor))==-1&&errno==EINVAL);cursor.hotspot_x=1;
    cursor.stride=3;errno=0;CHECK(op(OS_WINDOW_CURSOR,&cursor,sizeof(cursor))==-1&&errno==EINVAL);cursor.stride=4;
    cursor.reserved=1;errno=0;CHECK(op(OS_WINDOW_CURSOR,&cursor,sizeof(cursor))==-1&&errno==EINVAL);cursor.reserved=0;
    query.sequence=0;query.pixels=1;errno=0;CHECK(op(OS_WINDOW_SERVER_CURSOR,&query,sizeof(query))==-1&&errno==EFAULT);
    query.pixels=(uintptr_t)copy;CHECK(!op(OS_WINDOW_SERVER_CURSOR,&query,sizeof(query))&&query.sequence==1&&!memcmp(small,copy,sizeof(small)));
    query.pixels=1;CHECK(!op(OS_WINDOW_SERVER_CURSOR,&query,sizeof(query))&&query.sequence==1); /* same sequence does not copy */
    pid_t child=fork();CHECK(child>=0);
    if(!child){errno=0;CHECK(op(OS_WINDOW_CURSOR,&cursor,sizeof(cursor))==-1&&errno==EPERM);errno=0;CHECK(op(OS_WINDOW_SERVER_CURSOR,&query,sizeof(query))==-1&&errno==EPERM);_exit(0);}
    int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    cursor=(struct os_window_cursor){.id=id,.mode=OS_WINDOW_CURSOR_HIDDEN};CHECK(!op(OS_WINDOW_CURSOR,&cursor,sizeof(cursor)));
    CHECK(!op(OS_WINDOW_SERVER_CURSOR,&query,sizeof(query))&&query.sequence==2&&query.mode==OS_WINDOW_CURSOR_HIDDEN);
    cursor.mode=OS_WINDOW_CURSOR_DEFAULT;CHECK(!op(OS_WINDOW_CURSOR,&cursor,sizeof(cursor)));
    CHECK(!op(OS_WINDOW_SERVER_CURSOR,&query,sizeof(query))&&query.sequence==3&&!query.mode);
    size_t count=(size_t)OS_WINDOW_CURSOR_MAX*OS_WINDOW_CURSOR_MAX;uint32_t *large=malloc(count*4);CHECK(large);
    for(size_t i=0;i<count;i++)large[i]=0xff000000u|(i&0xffffff);
    cursor=(struct os_window_cursor){.id=id,.mode=OS_WINDOW_CURSOR_BITMAP,.width=OS_WINDOW_CURSOR_MAX,.height=OS_WINDOW_CURSOR_MAX,.stride=OS_WINDOW_CURSOR_MAX,.hotspot_x=-128,.hotspot_y=128,.pixels=(uintptr_t)large};
    CHECK(!op(OS_WINDOW_CURSOR,&cursor,sizeof(cursor)));
    query.pixels=(uintptr_t)copy;CHECK(!op(OS_WINDOW_SERVER_CURSOR,&query,sizeof(query))&&query.sequence==4&&query.width==128&&query.height==128&&!memcmp(large,copy,count*4));
    free(large);puts("NATIVE_CURSOR_PASS exact128x128_ARGB bounded_hotspots premultiplied_validation fault_atomicity same_sequence_no_copy owner_and_server_denials hidden_default cleanup_on_close");
    return 0;
}
static int geometry_check(void) {
    CHECK(!syscall(SYS_display,DISPLAY_ACQUIRE,0,0));CHECK(!op(OS_WINDOW_SERVER_START,NULL,0));
    struct os_window_info legacy={.version=OS_WINDOW_VERSION,.width=4,.height=3};
    CHECK(!op(OS_WINDOW_CREATE,&legacy,sizeof(legacy)));
    struct os_window_configure configuration={legacy.id,8,6,0};
    errno=0;CHECK(op(OS_WINDOW_SERVER_CONFIGURE,&configuration,sizeof(configuration))==-1&&errno==ENOTSUP);
    struct os_window_request request={legacy.id,OS_WINDOW_REQUEST_MAXIMIZE};
    errno=0;CHECK(op(OS_WINDOW_REQUEST_STATE,&request,sizeof(request))==-1&&errno==ENOTSUP);
    struct os_window_token token={legacy.id,0};CHECK(!op(OS_WINDOW_CLIENT_CLOSE,&token,sizeof(token)));
    struct os_window_info info={.version=OS_WINDOW_PIXEL_CLIENT_VERSION,.width=4,.height=3};
    CHECK(!op(OS_WINDOW_CREATE,&info,sizeof(info)));
    size_t bytes=(size_t)OS_WINDOW_PIXEL_MAX_WIDTH*OS_WINDOW_PIXEL_MAX_HEIGHT*4;
    uint32_t *pixels=malloc(bytes),*copy=malloc(bytes);CHECK(pixels&&copy);
    CHECK(!cursor_check(info.id,copy));
    for(unsigned y=0;y<OS_WINDOW_PIXEL_MAX_HEIGHT;y++)for(unsigned x=0;x<OS_WINDOW_PIXEL_MAX_WIDTH;x++)
        pixels[(size_t)y*OS_WINDOW_PIXEL_MAX_WIDTH+x]=pattern(x,y);
    struct os_window_frame frame={info.id,4,3,4,(uintptr_t)pixels,0};
    CHECK(!op(OS_WINDOW_PRESENT,&frame,sizeof(frame)));
    struct os_window_info next;CHECK(!op(OS_WINDOW_SERVER_NEXT,&next,sizeof(next))&&next.id==info.id);
    struct os_window_geometry geometry={.id=info.id};
    CHECK(!op(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))&&geometry.width==4&&geometry.height==3&&geometry.sequence==1);
    pid_t foreign=fork();CHECK(foreign>=0);
    if(!foreign) {
        errno=0;CHECK(op(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))==-1&&errno==EPERM);
        configuration.id=info.id;errno=0;CHECK(op(OS_WINDOW_SERVER_CONFIGURE,&configuration,sizeof(configuration))==-1&&errno==EPERM);
        request.id=info.id;errno=0;CHECK(op(OS_WINDOW_REQUEST_STATE,&request,sizeof(request))==-1&&errno==EPERM);
        errno=0;CHECK(op(OS_WINDOW_SERVER_REQUEST,&request,sizeof(request))==-1&&errno==EPERM);
        errno=0;CHECK(op(OS_WINDOW_PRESENT,&frame,sizeof(frame))==-1&&errno==EPERM);
        _exit(0);
    }
    int status;CHECK(waitpid(foreign,&status,0)==foreign&&WIFEXITED(status)&&!WEXITSTATUS(status));
    configuration=(struct os_window_configure){info.id,OS_WINDOW_PIXEL_MAX_WIDTH+1,OS_WINDOW_PIXEL_MAX_HEIGHT,0};
    errno=0;CHECK(op(OS_WINDOW_SERVER_CONFIGURE,&configuration,sizeof(configuration))==-1&&errno==EINVAL);
    configuration.width=OS_WINDOW_PIXEL_MAX_WIDTH;configuration.states=8;
    errno=0;CHECK(op(OS_WINDOW_SERVER_CONFIGURE,&configuration,sizeof(configuration))==-1&&errno==EINVAL);
    configuration.states=OS_WINDOW_STATE_MAXIMIZED;CHECK(!op(OS_WINDOW_SERVER_CONFIGURE,&configuration,sizeof(configuration)));
    CHECK(!op(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))&&geometry.width==4&&geometry.height==3&&geometry.sequence==1&&
        geometry.requested_width==OS_WINDOW_PIXEL_MAX_WIDTH&&geometry.requested_height==OS_WINDOW_PIXEL_MAX_HEIGHT&&geometry.states==OS_WINDOW_STATE_MAXIMIZED);
    configuration.states=OS_WINDOW_STATE_FULLSCREEN;CHECK(!op(OS_WINDOW_SERVER_CONFIGURE,&configuration,sizeof(configuration)));
    CHECK(!op(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))&&geometry.width==4&&geometry.sequence==1&&geometry.states==OS_WINDOW_STATE_FULLSCREEN);
    request=(struct os_window_request){info.id,OS_WINDOW_REQUEST_FULLSCREEN};CHECK(!op(OS_WINDOW_REQUEST_STATE,&request,sizeof(request)));
    CHECK(!op(OS_WINDOW_SERVER_REQUEST,&request,sizeof(request))&&request.action==OS_WINDOW_REQUEST_FULLSCREEN);
    request.action=OS_WINDOW_REQUEST_UNFULLSCREEN;CHECK(!op(OS_WINDOW_REQUEST_STATE,&request,sizeof(request)));
    errno=0;CHECK(op(OS_WINDOW_SERVER_REQUEST,(void*)1,sizeof(request))==-1&&errno==EFAULT);
    CHECK(!op(OS_WINDOW_SERVER_REQUEST,&request,sizeof(request))&&request.action==OS_WINDOW_REQUEST_UNFULLSCREEN);
    request.action=OS_WINDOW_REQUEST_UNFULLSCREEN+1;errno=0;CHECK(op(OS_WINDOW_REQUEST_STATE,&request,sizeof(request))==-1&&errno==EINVAL);
    configuration.states=OS_WINDOW_STATE_MAXIMIZED;CHECK(!op(OS_WINDOW_SERVER_CONFIGURE,&configuration,sizeof(configuration)));
    frame.pixels=(uintptr_t)copy;CHECK(!op(OS_WINDOW_SERVER_READ,&frame,sizeof(frame))&&frame.sequence==1&&!memcmp(copy,pixels,12*4));
    struct os_window_frame changed={info.id,OS_WINDOW_PIXEL_MAX_WIDTH,OS_WINDOW_PIXEL_MAX_HEIGHT,OS_WINDOW_PIXEL_MAX_WIDTH,1,0};
    errno=0;CHECK(op(OS_WINDOW_PRESENT,&changed,sizeof(changed))==-1&&errno==EFAULT);
    CHECK(!op(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))&&geometry.width==4&&geometry.sequence==1);
    changed.width--;changed.stride=changed.width;changed.pixels=(uintptr_t)pixels;
    errno=0;CHECK(op(OS_WINDOW_PRESENT,&changed,sizeof(changed))==-1&&errno==EINVAL);
    changed.width=changed.stride=OS_WINDOW_PIXEL_MAX_WIDTH;CHECK(!op(OS_WINDOW_PRESENT,&changed,sizeof(changed)));
    CHECK(!op(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))&&geometry.width==OS_WINDOW_PIXEL_MAX_WIDTH&&geometry.height==OS_WINDOW_PIXEL_MAX_HEIGHT&&geometry.sequence==2);
    frame.sequence=0;errno=0;CHECK(op(OS_WINDOW_SERVER_READ,&frame,sizeof(frame))==-1&&errno==EINVAL);
    changed.pixels=(uintptr_t)copy;CHECK(!op(OS_WINDOW_SERVER_READ,&changed,sizeof(changed))&&changed.sequence==2&&!memcmp(copy,pixels,bytes));
    request=(struct os_window_request){info.id,OS_WINDOW_REQUEST_MAXIMIZE};CHECK(!op(OS_WINDOW_REQUEST_STATE,&request,sizeof(request)));
    request.action=OS_WINDOW_REQUEST_MINIMIZE;CHECK(!op(OS_WINDOW_REQUEST_STATE,&request,sizeof(request)));
    request.action=OS_WINDOW_REQUEST_RESTORE;CHECK(!op(OS_WINDOW_REQUEST_STATE,&request,sizeof(request)));
    errno=0;CHECK(op(OS_WINDOW_SERVER_REQUEST,(void*)1,sizeof(request))==-1&&errno==EFAULT);
    CHECK(!op(OS_WINDOW_SERVER_REQUEST,&request,sizeof(request))&&request.id==info.id&&request.action==OS_WINDOW_REQUEST_RESTORE);
    errno=0;CHECK(op(OS_WINDOW_SERVER_REQUEST,&request,sizeof(request))==-1&&errno==EAGAIN);
    request.action=0;errno=0;CHECK(op(OS_WINDOW_REQUEST_STATE,&request,sizeof(request))==-1&&errno==EINVAL);
    geometry.reserved0=1;errno=0;CHECK(op(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))==-1&&errno==EINVAL);geometry.reserved0=0;
    configuration=(struct os_window_configure){info.id,8,6,0};CHECK(!op(OS_WINDOW_SERVER_CONFIGURE,&configuration,sizeof(configuration)));
    CHECK(!op(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))&&geometry.width==OS_WINDOW_PIXEL_MAX_WIDTH&&geometry.sequence==2&&geometry.requested_width==8&&!geometry.states);
    changed=(struct os_window_frame){info.id,8,6,8,(uintptr_t)pixels,0};CHECK(!op(OS_WINDOW_PRESENT,&changed,sizeof(changed)));
    CHECK(!op(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))&&geometry.width==8&&geometry.height==6&&geometry.sequence==3);
    changed.pixels=(uintptr_t)copy;CHECK(!op(OS_WINDOW_SERVER_READ,&changed,sizeof(changed))&&changed.sequence==3&&!memcmp(copy,pixels,8*6*4));
    token.id=info.id;CHECK(!op(OS_WINDOW_SERVER_CLOSE,&token,sizeof(token)));
    errno=0;CHECK(op(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))==-1&&errno==EPIPE);
    errno=0;CHECK(op(OS_WINDOW_SERVER_CONFIGURE,&configuration,sizeof(configuration))==-1&&errno==EPIPE);
    request.action=OS_WINDOW_REQUEST_MAXIMIZE;errno=0;CHECK(op(OS_WINDOW_REQUEST_STATE,&request,sizeof(request))==-1&&errno==EPIPE);
    CHECK(!op(OS_WINDOW_CLIENT_CLOSE,&token,sizeof(token)));
    errno=0;CHECK(op(OS_WINDOW_GEOMETRY,&geometry,sizeof(geometry))==-1&&errno==ENOENT);
    free(copy);free(pixels);CHECK(!syscall(SYS_display,DISPLAY_RELEASE,0,0));
    puts("NATIVE_WINDOW_GEOMETRY_PASS max2560x1600 grow_shrink exact_pixels retained_front fault_atomicity ownership coalesced_state_requests fullscreen legacy_bounds cleanup");
    return 0;
}
static int damage_check(void) {
    CHECK(!syscall(SYS_display,DISPLAY_ACQUIRE,0,0));CHECK(!op(OS_WINDOW_SERVER_START,NULL,0));
    struct os_window_info info={.version=OS_WINDOW_PIXEL_CLIENT_VERSION,.width=64,.height=64};
    CHECK(!op(OS_WINDOW_CREATE,&info,sizeof(info)));
    uint32_t pixels[4096],copy[4096],expected[4096];
    for(unsigned i=0;i<4096;i++)pixels[i]=expected[i]=0x123400+i;
    memset(copy,0,sizeof(copy));
    struct os_window_damage_frame f={.frame={info.id,64,64,64,(uintptr_t)pixels,0},.width=8,.height=2};
    errno=0;CHECK(op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f))==-1&&errno==EINVAL);
    f.width=f.height=64;CHECK(!op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f)));
    struct os_window_damage_frame read=f;read.frame.pixels=(uintptr_t)copy;
    CHECK(!op(OS_WINDOW_SERVER_READ_DAMAGE,&read,sizeof(read))&&read.frame.sequence==1&&
          !read.x&&!read.y&&read.width==64&&read.height==64&&!memcmp(copy,expected,sizeof(copy)));
    pid_t foreign=fork();CHECK(foreign>=0);
    if(!foreign){
        errno=0;CHECK(op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f))==-1&&errno==EPERM);
        errno=0;CHECK(op(OS_WINDOW_SERVER_READ_DAMAGE,&read,sizeof(read))==-1&&errno==EPERM);_exit(0);
    }
    int status;CHECK(waitpid(foreign,&status,0)==foreign&&WIFEXITED(status)&&!WEXITSTATUS(status));
    for(unsigned i=0;i<4096;i++)pixels[i]^=0xffffff; /* Out-of-region source pixels must never leak. */
    for(unsigned pass=0;pass<2;pass++) {
        f.x=pass?42:5;f.y=pass?26:3;f.width=pass?8:4;f.height=pass?3:2;
        for(unsigned y=f.y;y<f.y+f.height;y++)for(unsigned x=f.x;x<f.x+f.width;x++)
            expected[y*64+x]=pixels[y*64+x];
        CHECK(!op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f)));
    }
    CHECK(!op(OS_WINDOW_SERVER_READ_DAMAGE,&read,sizeof(read))&&read.frame.sequence==3&&
          read.x==5&&read.y==3&&read.width==45&&read.height==26&&!memcmp(copy,expected,sizeof(copy)));
    errno=0;CHECK(op(OS_WINDOW_SERVER_READ_DAMAGE,&read,sizeof(read))==-1&&errno==EAGAIN);
    /* A stale retained sequence must receive every pixel, not just the next patch. */
    read.frame.sequence=0;memset(copy,0,sizeof(copy));
    CHECK(!op(OS_WINDOW_SERVER_READ_DAMAGE,&read,sizeof(read))&&read.frame.sequence==3&&
          !read.x&&!read.y&&read.width==64&&read.height==64&&!memcmp(copy,expected,sizeof(copy)));
    f.x=UINT32_MAX;errno=0;CHECK(op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f))==-1&&errno==EINVAL);
    f.x=63;f.width=2;errno=0;CHECK(op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f))==-1&&errno==EINVAL);
    f.x=0;f.width=0;errno=0;CHECK(op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f))==-1&&errno==EINVAL);
    f.width=8;f.height=2;f.y=0;
    f.frame.pixels=UINT64_MAX-16;errno=0;CHECK(op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f))==-1&&errno==EFAULT);
    uint8_t *mapping=mmap(NULL,8192,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(mapping!=MAP_FAILED);
    CHECK(!mprotect(mapping,4096,PROT_READ|PROT_WRITE));
    uint32_t *edge=(uint32_t *)(mapping+4096-8*4);for(unsigned i=0;i<8;i++)edge[i]=0xffffff;
    f.frame.pixels=(uintptr_t)edge;
    errno=0;CHECK(op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f))==-1&&errno==EFAULT);
    read.frame.sequence=0;CHECK(!op(OS_WINDOW_SERVER_READ_DAMAGE,&read,sizeof(read))&&
          read.frame.sequence==3&&!memcmp(copy,expected,sizeof(copy))); /* First staged row did not publish. */
    f.frame.pixels=(uintptr_t)pixels;
    for(unsigned y=0;y<2;y++)for(unsigned x=0;x<8;x++)expected[y*64+x]=pixels[y*64+x];
    CHECK(!op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f)));
    read.frame.pixels=(uintptr_t)edge;
    errno=0;CHECK(op(OS_WINDOW_SERVER_READ_DAMAGE,&read,sizeof(read))==-1&&errno==EFAULT);
    read.frame.pixels=(uintptr_t)copy;
    CHECK(!op(OS_WINDOW_SERVER_READ_DAMAGE,&read,sizeof(read))&&read.frame.sequence==4&&
          !read.x&&!read.y&&read.width==8&&read.height==2&&!memcmp(copy,expected,sizeof(copy)));
    CHECK(!munmap(mapping,8192));
    struct os_window_configure configure={info.id,8,6,0};CHECK(!op(OS_WINDOW_SERVER_CONFIGURE,&configure,sizeof(configure)));
    f.frame.width=f.frame.stride=8;f.frame.height=6;f.width=8;f.height=2;
    errno=0;CHECK(op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f))==-1&&errno==EINVAL);
    f.height=6;CHECK(!op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f)));
    read.frame.width=read.frame.stride=8;read.frame.height=6;
    CHECK(!op(OS_WINDOW_SERVER_READ_DAMAGE,&read,sizeof(read))&&read.frame.sequence==5&&
          !read.x&&!read.y&&read.width==8&&read.height==6&&!memcmp(copy,pixels,8*6*4));
    struct os_window_frame legacy=f.frame;legacy.pixels=(uintptr_t)pixels;
    pixels[0]^=1;CHECK(!op(OS_WINDOW_PRESENT,&legacy,sizeof(legacy)));
    CHECK(!op(OS_WINDOW_SERVER_READ_DAMAGE,&read,sizeof(read))&&read.frame.sequence==6&&
          read.width==8&&read.height==6&&!memcmp(copy,pixels,8*6*4));
    struct os_window_token token={info.id,0};CHECK(!op(OS_WINDOW_CLIENT_CLOSE,&token,sizeof(token)));
    info=(struct os_window_info){.version=OS_WINDOW_VERSION,.width=8,.height=6};
    CHECK(!op(OS_WINDOW_CREATE,&info,sizeof(info)));f.frame.id=read.frame.id=info.id;
    errno=0;CHECK(op(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f))==-1&&errno==ENOTSUP);
    errno=0;CHECK(op(OS_WINDOW_SERVER_READ_DAMAGE,&read,sizeof(read))==-1&&errno==ENOTSUP);
    token.id=info.id;CHECK(!op(OS_WINDOW_CLIENT_CLOSE,&token,sizeof(token)));
    CHECK(!syscall(SYS_display,DISPLAY_RELEASE,0,0));
    puts("NATIVE_WINDOW_DAMAGE_PASS exact_regions coalesced_skipped_frames stale_sequence_full_copy staged_fault_atomicity failed_read_retry overflow_bounds owner_server_denials resize_full_refresh legacy_compatibility");
    return 0;
}
int main(int argc,char **argv){
    struct utsname system;CHECK(!uname(&system)&&!strcmp(system.sysname,"Coast"));
    if(argc==2&&!strcmp(argv[1],"--geometry"))return geometry_check();
    if(argc==2&&!strcmp(argv[1],"--damage"))return damage_check();
    int display=argc==2&&!strcmp(argv[1],"--pattern");
    CHECK(argc==1||display);
    setvbuf(stdout,NULL,_IONBF,0);
    if(!display){CHECK(!syscall(SYS_display,DISPLAY_ACQUIRE,0,0));CHECK(!op(OS_WINDOW_SERVER_START,NULL,0));}
    unsigned width=display?2000:OS_WINDOW_PIXEL_MAX_WIDTH,height=display?1200:OS_WINDOW_PIXEL_MAX_HEIGHT;
    struct os_window_info info={.version=OS_WINDOW_PIXEL_CLIENT_VERSION,.width=width,.height=height};
    strcpy(info.title,"Native pixel fidelity check");
    if(!display){
        struct os_window_info bad=info;bad.width++;errno=0;CHECK(op(OS_WINDOW_CREATE,&bad,sizeof(bad))==-1&&errno==EINVAL);
        bad=info;bad.height++;errno=0;CHECK(op(OS_WINDOW_CREATE,&bad,sizeof(bad))==-1&&errno==EINVAL);
        bad=info;bad.version=OS_WINDOW_PIXEL_CLIENT_VERSION+1;errno=0;CHECK(op(OS_WINDOW_CREATE,&bad,sizeof(bad))==-1&&errno==EINVAL);
        bad=info;bad.version=OS_WINDOW_VERSION;errno=0;CHECK(op(OS_WINDOW_CREATE,&bad,sizeof(bad))==-1&&errno==EINVAL);
    }
    size_t bytes=(size_t)width*height*4;
    uint32_t *pixels=malloc(bytes),*copy=display?NULL:malloc(bytes);CHECK(pixels&&(display||copy));
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++)pixels[(size_t)y*width+x]=pattern(x,y);
    for(unsigned attempt=0;attempt<(display?1:3);attempt++){
        info.id=info.client=0;info.sequence=0;
        CHECK(!op(OS_WINDOW_CREATE,&info,sizeof(info)));
        struct os_window_frame frame={.id=info.id,.width=width,.height=height,.stride=width,.pixels=(uintptr_t)pixels};
        CHECK(!op(OS_WINDOW_PRESENT,&frame,sizeof(frame)));
        if(display){
            puts("NATIVE_PIXEL_PATTERN_READY width=2000 height=1200");
            struct timespec delay={.tv_nsec=10000000};
            for(unsigned tick=0;tick<3000;tick++){
                struct os_window_event events[16];struct os_window_events q={info.id,16,(uintptr_t)events};
                long n=op(OS_WINDOW_INPUT,&q,sizeof(q));
                if(n<0&&errno!=EAGAIN)break;
                int closed=0;for(long i=0;i<n;i++)if(events[i].type==OS_WINDOW_CLOSE)closed=1;
                if(closed)break;
                nanosleep(&delay,NULL);
            }
        }else{
            struct os_window_info next;CHECK(!op(OS_WINDOW_SERVER_NEXT,&next,sizeof(next))&&next.version==OS_WINDOW_PIXEL_CLIENT_VERSION&&next.width==width&&next.height==height);
            frame.pixels=(uintptr_t)copy;CHECK(!op(OS_WINDOW_SERVER_READ,&frame,sizeof(frame))&&frame.sequence==1);
            CHECK(!memcmp(pixels,copy,bytes));
            frame.pixels=1;errno=0;CHECK(op(OS_WINDOW_PRESENT,&frame,sizeof(frame))==-1&&errno==EFAULT);
            frame.pixels=(uintptr_t)copy;frame.sequence=0;CHECK(!op(OS_WINDOW_SERVER_READ,&frame,sizeof(frame))&&frame.sequence==1);
            CHECK(!memcmp(pixels,copy,bytes));
        }
        struct os_window_token token={info.id,0};CHECK(!op(OS_WINDOW_CLIENT_CLOSE,&token,sizeof(token)));
    }
    free(copy);free(pixels);
    if(!display)CHECK(!syscall(SYS_display,DISPLAY_RELEASE,0,0));
    puts(display?"NATIVE_PIXEL_PATTERN_CLOSED":"NATIVE_PIXEL_WINDOW_PASS max2560x1600 exact_bytes fault_atomicity legacy_bounds reuse3");
    return 0;
}
