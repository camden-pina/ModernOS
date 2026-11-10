/* Exercise production native vector masks, clipping and bounded cache ownership. */
#include "../../userland/desktop/render.c"
Desktop desk;
void *malloc(size_t);
void free(void *);
static size_t live, peak;
static void *host_allocate(size_t bytes) {void *p=malloc(bytes);if(p){live+=bytes;peak=MAX(peak,live);}return p;}
static void host_deallocate(void *p,size_t bytes) {if(p){live-=bytes;free(p);}}
#define allocate host_allocate
#define deallocate host_deallocate
#include "../../userland/desktop/icons.c"
#undef allocate
#undef deallocate
#define CHECK(x) do {if(!(x)){printf("FAIL icon line=%d scale=%d size=%d kind=%u\n",__LINE__,desk.scale,size,kind);exit(1);}}while(0)
static uint32_t pixels[416*416];
int main(void) {
 const int sizes[]={16,28,64,80,112,224,298,384},scales[]={100,125,150,200};unsigned cases=0;
 desk.paint=pixels;desk.paint_stride=416;desk.width=desk.height=416;
 for(unsigned si=0;si<4;si++)for(unsigned zi=0;zi<8;zi++)for(unsigned kind=0;kind<I_COUNT;kind++) {
  int size=sizes[zi];desk.scale=scales[si];desk.scale_index=si;
  for(unsigned i=0;i<416*416;i++)pixels[i]=0x112233;
  desk.clip=(Rect){8,8,size,size};icon(8,8,kind,size,0xFFFFFF);unsigned changed=0;
  for(int y=0;y<416;y++)for(int x=0;x<416;x++) {
   bool inside=x>=8 && y>=8 && x<8+size && y<8+size;
   if(inside)changed+=pixels[y*416+x]!=0x112233;
   else CHECK(pixels[y*416+x]==0x112233);
  }
  CHECK(changed>0);CHECK(cache_bytes==live);CHECK(live<=4*1024*1024);cases++;
 }
 icon_cleanup();if(live || cache_bytes)return 1;
 printf("PASS %u production icon raster/cache/clip cases, peak=%lu bytes, zero owned allocations after cleanup (ASan/UBSan)\n",cases,peak);return 0;
}
