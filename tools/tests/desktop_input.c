#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include "text_input_model.h"
#include "pointer_motion.h"
#include <text_utf8.h>
#include <drivers/usb/hid_mouse.h>
#include <input/mouse.h>

static void text_tests(void) {
    assert(text_utf8_valid("plain\ttext\r\n",12));
    const char unicode[]={0xc2,0xa3,0xe2,0x82,0xac,0xf0,0x9f,0x98,0x80};
    assert(text_utf8_valid(unicode,sizeof(unicode)));
    const unsigned char bad[][5]={{0},{27},{127},{0xc0,0x80},{0xe0,0x80,0x80},{0xed,0xa0,0x80},{0xf4,0x90,0x80,0x80},{0xf5,0x80,0x80,0x80},{0x80},{0xc2}};
    const size_t sizes[]={1,1,1,2,3,3,4,4,1,1};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++)assert(!text_utf8_valid((const char *)bad[i],sizes[i]));
    char text[32]="A\xc2\xa3\xe2\x82\xacZ";
    assert(input_next(text,1)==3 && input_next(text,3)==6);
    assert(input_previous(text,6)==3 && input_previous(text,3)==1 && input_position(text,5)==3);
    int cursor=6,anchor=1;
    assert(input_replace(text,sizeof(text),&cursor,&anchor,"ok",2));
    assert(!strcmp(text,"AokZ") && cursor==3 && anchor==3);
    cursor=4;anchor=1;
    assert(input_replace(text,sizeof(text),&cursor,&anchor,"",0) && !strcmp(text,"A"));
    strcpy(text,"1234567");cursor=3;anchor=3;
    char old[32];strcpy(old,text);
    assert(!input_replace(text,8,&cursor,&anchor,"x",1) && !strcmp(text,old) && cursor==3 && anchor==3);
    cursor=7;anchor=0;
    assert(input_replace(text,8,&cursor,&anchor,"abcdefg",7) && !strcmp(text,"abcdefg"));
    assert(!input_replace(text,0,&cursor,&anchor,"",0));
    puts("PASS UTF-8 scalars/control rejection, selection boundaries, replacement and atomic capacity failure");
}
static int run_motion(int interval,int dx,int duration,bool accelerated) {
    PointerMotion p={0};int sum=0;
    for(int time=interval;time<=duration;time+=interval) {
        int32_t x,y;pointer_motion(&p,100,accelerated,dx,0,1000000+(uint64_t)time*1000,&x,&y);
        assert(x>=0 && y==0);sum+=x;
    }
    return sum;
}
static void motion_tests(void) {
    PointerMotion p={0};int32_t x,y;
    pointer_motion(&p,100,false,INT32_MAX,INT32_MIN,1000000,&x,&y);
    assert(x==INT32_MAX && y==INT32_MIN);
    memset(&p,0,sizeof(p));int total=0;
    for(int i=1;i<=100;i++){pointer_motion(&p,50,false,1,0,1000000+i*8000,&x,&y);total+=x;}
    assert(total==50);
    for(int i=101;i<=200;i++){pointer_motion(&p,50,false,-1,0,1000000+i*8000,&x,&y);total+=x;}
    assert(total==0 && p.fraction_x==0);
    int a=run_motion(8,8,2000,true),b=run_motion(1,1,2000,true);
    assert(a>2000 && b>2000 && (a-b<40) && (b-a<40));
    assert(run_motion(8,1,2000,true)==250);
    pointer_motion(&p,300,true,INT32_MAX,INT32_MIN,9000000,&x,&y);
    assert(x==INT32_MAX && y==INT32_MIN);
    pointer_motion(&p,50,false,1,0,10000000,&x,&y);
    assert(x==0); /* Idle resets the speed estimate. */
    pointer_motion(&p,50,false,1,0,12000000,&x,&y);assert(x==1);
    PointerMotion edge={0};int position=0;
    for(int i=0;i<2;i++) {
        pointer_motion(&edge,50,false,1,0,20000000+i*1000,&x,&y);
        position+=x;pointer_clip_fraction(&edge,position,0,10,10);
    }
    assert(position==1);
    position=0;memset(&edge,0,sizeof(edge));
    pointer_motion(&edge,50,false,-1,0,21000000,&x,&y);pointer_clip_fraction(&edge,0,0,10,10);
    assert(edge.fraction_x==0);
    for(int i=0;i<2;i++) {pointer_motion(&edge,50,false,1,0,22000000+i*1000,&x,&y);position+=x;pointer_clip_fraction(&edge,position,0,10,10);}
    assert(position==1);
    printf("PASS linear/fractional movement, reversal, bounded extremes, low-speed precision and 125/1000Hz adaptive totals %d/%d\n",a,b);
}
static const uint8_t descriptor[]={
 0x05,0x0d,0x09,0x05,0xa1,0x01, /* Digitizers / Touch Pad application */
 0x05,0x01,0x09,0x01,0xa1,0x00,
 0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,0x95,0x03,0x75,0x01,0x81,0x02,
 0x95,0x01,0x75,0x05,0x81,0x01,
 0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x02,0x81,0x06,0xc0,0xc0};
static void hid_tests(void) {
    hid_mouse_layout_t layout;
    assert(hid_mouse_parse_descriptor(descriptor,sizeof(descriptor),&layout));
    assert(layout.touchpad && !layout.mouse_application && !layout.absolute);
    assert(hid_mouse_layout_fits(&layout,3));
    uint8_t packet[]={1,0x7f,0x81};hid_mouse_sample_t sample;
    assert(hid_mouse_decode(&layout,packet,sizeof(packet),&sample)==HID_MOUSE_DECODED);
    assert(sample.buttons==1 && sample.dx==127 && sample.dy==-127);
    uint8_t altered[sizeof(descriptor)];memcpy(altered,descriptor,sizeof(altered));
    assert(altered[sizeof(altered)-3]==6);altered[sizeof(altered)-3]=2;
    assert(!hid_mouse_parse_descriptor(altered,sizeof(altered),&layout) && layout.touchpad && layout.absolute);
    memcpy(altered,descriptor,sizeof(altered));altered[3]=4;
    assert(!hid_mouse_parse_descriptor(altered,sizeof(altered),&layout) && !layout.touchpad);
    memcpy(altered,descriptor,sizeof(altered));altered[1]=1;altered[3]=2;
    assert(hid_mouse_parse_descriptor(altered,sizeof(altered),&layout) && !layout.touchpad && layout.mouse_application);
    for(size_t i=1;i<sizeof(descriptor);i++)assert(!hid_mouse_parse_descriptor(descriptor,i,&layout));
    mouse_init();assert(mouse_attach(1));assert(mouse_capabilities(1)==(OS_POINTER_MOUSE|OS_POINTER_RELATIVE));
    mouse_set_capabilities(1,OS_POINTER_TOUCHPAD|OS_POINTER_RELATIVE);
    assert(mouse_capabilities(1)==(OS_POINTER_TOUCHPAD|OS_POINTER_RELATIVE));
    mouse_detach(1);assert(mouse_attach(1));assert(mouse_capabilities(1)==(OS_POINTER_MOUSE|OS_POINTER_RELATIVE));
    puts("PASS real HID Mouse/relative TouchPad distinction, absolute/touchscreen/truncation rejection, decoding and source reuse");
}
int main(void){text_tests();motion_tests();hid_tests();return 0;}
