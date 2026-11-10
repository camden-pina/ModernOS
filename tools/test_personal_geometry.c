/* Sanitize real production Dock geometry at extreme size/DPI/visibility states. */
#include "../userland/desktop/dock.c"
Desktop desk;
int S(int v) {return (v*desk.scale+50)/100;}
int clampi(int64_t v,int low,int high) {return v<low?low:v>high?high:v;}
bool contains(Rect r,int x,int y) {return x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h;}
#define CHECK(x) do {if(!(x)){printf("FAIL geometry line=%d width=%d scale=%d size=%d\n",__LINE__,desk.width,desk.scale,desk.dock_size);exit(1);}}while(0)
int main(void) {
  unsigned cases=0;const int widths[]={640,800,1280,1920,2560},scales[]={100,125,150,200},sizes[]={16,24,30,42,64,96,112},mags[]={0,16,24,50,75,100};
  for(unsigned wi=0;wi<5;wi++)for(unsigned si=0;si<4;si++)for(int visible=0;visible<3;visible++)for(unsigned zi=0;zi<7;zi++)for(unsigned mi=0;mi<6;mi++)for(int hovered=0;hovered<10;hovered++) {
    memset(&desk,0,sizeof(desk));desk.width=widths[wi];desk.height=1600;desk.scale=scales[si];desk.dock_size=sizes[zi];desk.dock_magnification=mags[mi];
    desk.dock_pins=visible==0?0:visible==1?0x3f:0xff;
    if(visible==0)for(int i=0;i<APP_COUNT;i++){desk.windows[i].alive=true;desk.windows[i].app=i;}
    for(int i=0;i<APP_COUNT+2;i++)magnify[i]=1024+(i==hovered?mags[mi]*1024/100:0);
    Rect b,c[APP_COUNT+2],t[APP_COUNT+2];layout(&b,c,t);int list[APP_COUNT+2],n=roles(list);
    Rect cached=taskbar_dock_cache_rect();CHECK(cached.x<=b.x && cached.x+cached.w>=b.x+b.w && cached.y==b.y && cached.h==b.h);
    CHECK(b.x>=0 && b.x+b.w<=desk.width);CHECK(b.y>=0 && b.y+b.h<=desk.height);
    for(int i=0;i<n;i++) {
      CHECK(t[i].x>=0 && t[i].x+t[i].w<=desk.width);CHECK(t[i].w>0 && t[i].w==t[i].h);
      CHECK(t[i].x>=c[i].x && t[i].x+t[i].w<=c[i].x+c[i].w);
      if(i)CHECK(c[i-1].x+c[i-1].w==c[i].x && t[i-1].x+t[i-1].w<=t[i].x);
      if(i==hovered%n) CHECK(dock_hit(t[i].x+t[i].w/2,t[i].y+t[i].h/2)==i);
    }
    cases++;
  }
  printf("PASS %u real Dock fit/neighbor/hit cases at 100/125/150/200%% (ASan/UBSan)\n",cases);return 0;
}
