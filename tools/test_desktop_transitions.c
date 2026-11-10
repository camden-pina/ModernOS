#include "../userland/desktop/transitions.c"
#include "../userland/desktop/coverage.c"
Desktop desk;
void *malloc(size_t);void free(void *);
static uint64_t clock_value=100;
static uint32_t pixels[32*24];
static unsigned damage;
void *allocate(size_t n){return malloc(n);}
void deallocate(void *p,size_t n){(void)n;free(p);}
uint64_t now_ms(void){return clock_value;}
bool empty(Rect r){return r.w<=0 || r.h<=0;}
Rect intersect(Rect a,Rect b){int x=MAX(a.x,b.x),y=MAX(a.y,b.y);return (Rect){x,y,MAX(0,MIN(a.x+a.w,b.x+b.w)-x),MAX(0,MIN(a.y+a.h,b.y+b.h)-y)};}
void invalidate(Rect r){(void)r;damage++;}
#define CHECK(x) do{if(!(x)){printf("FAIL transition line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 desk.ui_motion=1;desk.width=32;desk.height=24;desk.scene=pixels;
 for(int i=0;i<32*24;i++)pixels[i]=0x123456;
 transition_begin((Rect){-4,-4,36,28});
 for(int i=0;i<32*24;i++)pixels[i]=0xABCDEF;
 CHECK(transition_present());CHECK(pixels[0]==0x123456);
 transition_restore();CHECK(pixels[0]==0xABCDEF);
 clock_value+=60;transition_tick();CHECK(transition_present());
 CHECK(pixels[0]!=0x123456 && pixels[0]!=0xABCDEF);
 transition_restore();CHECK(pixels[0]==0xABCDEF);
 clock_value+=60;transition_tick();CHECK(!transition_present());
 CHECK(pixels[0]==0xABCDEF && damage>=3);
 transition_begin((Rect){0,0,32,24});desk.width=16;transition_tick();CHECK(!transition_present());
 desk.ui_motion=0;transition_begin((Rect){0,0,16,24});CHECK(!transition_present());
 desk.ui_motion=2;transition_begin((Rect){0,0,16,24});clock_value+=120;
 transition_tick();CHECK(transition_present());transition_restore();clock_value+=60;
 transition_tick();CHECK(!transition_present());
 transition_cleanup();
 puts("PASS production fades interpolate without scaling, restore exact retained pixels, finish after 120 ms and cancel on display changes (ASan/UBSan)");
 return 0;
}
