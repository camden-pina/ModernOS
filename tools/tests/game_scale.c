/* Exercise the production compositor draw loop against independent pixel
   positions, including clipping, downscaling and large software viewports. */
#include "../../userland/desktop/game.c"
extern void *malloc(size_t);
extern void free(void *);
#define CHECK(x) do {if(!(x)){printf("GAME SCALE FAIL line=%d: %s\n",__LINE__,#x);__builtin_trap();}}while(0)
Desktop desk;
static Rect body;
static uint32_t *target;
static unsigned stride,rows,scale=100;
int S(int n){return (n*scale+50)/100;}
Rect window_body(Window *w){(void)w;return body;}
Rect intersect(Rect a,Rect b){int x=MAX(a.x,b.x),y=MAX(a.y,b.y),right=MIN(a.x+a.w,b.x+b.w),bottom=MIN(a.y+a.h,b.y+b.h);return (Rect){x,y,MAX(0,right-x),MAX(0,bottom-y)};}
uint32_t *paint_pixel(int x,int y){CHECK(x>=0 && (unsigned)x<stride && y>=0 && (unsigned)y<rows);return target+(size_t)y*stride+x;}
void fill(Rect r,uint32_t color){(void)r;(void)color;}
void label(int x,int y,const char *text,Font font,uint32_t color,int width){(void)x;(void)y;(void)text;(void)font;(void)color;(void)width;}
static void fixture(unsigned sw,unsigned sh,unsigned width,unsigned height,bool clipped){
 stride=width;rows=height+S(27);body=(Rect){0,0,width,rows};
 target=malloc((size_t)stride*rows*4);uint32_t *source=malloc((size_t)sw*sh*4);CHECK(target && source);
 for(size_t i=0;i<(size_t)sw*sh;i++)source[i]=0x123400^(i*97 & 0xffffff);
 for(size_t i=0;i<(size_t)stride*rows;i++)target[i]=0xdeadbeef;
 Game game={.info={.width=sw,.height=sh},.pixels=source,.painted=true};Window window={.state=&game};
 desk.trace=false;desk.clip=clipped?(Rect){width/5,height/7,MAX(1,width/2),MAX(1,height/2)}:body;
 game_draw(&window);Rect view=game.view,area=intersect(view,desk.clip);
 CHECK(view.w>0 && view.h>0);
 for(unsigned y=0;y<rows;y++)for(unsigned x=0;x<stride;x++){
  uint32_t expected=0xdeadbeef;
  if((int)x>=area.x && (int)x<area.x+area.w && (int)y>=area.y && (int)y<area.y+area.h){
   unsigned sx=(uint64_t)(x-view.x)*sw/view.w,sy=(uint64_t)(y-view.y)*sh/view.h;
   CHECK(sx<sw && sy<sh);expected=source[(size_t)sy*sw+sx];
  }
  CHECK(target[(size_t)y*stride+x]==expected);
 }
 free(target);free(source);
}
int main(void){
 const unsigned source[][2]={{1,1},{4,3},{320,200},{1024,768}};
 const unsigned views[][2]={{1,1},{53,40},{640,480},{1986,1490},{2560,1517}};
 for(unsigned s=0;s<4;s++)for(unsigned v=0;v<5;v++)for(unsigned c=0;c<2;c++){
  scale=v%2?150:100;fixture(source[s][0],source[s][1],views[v][0],views[v][1],c);
 }
 puts("PASS production software-window scaling: exact nearest pixels, clipped subregions, 1px/downscale/large surfaces and UI scales (ASan/UBSan)");return 0;
}
