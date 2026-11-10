#include "../userland/desktop/render.c"
Desktop desk;
static unsigned seed=123456789;
static unsigned next(void){seed=seed*1664525+1013904223;return seed;}
#define CHECK(x) do{if(!(x)){printf("FAIL damage line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
  desk.width=97;desk.height=73;desk.scale=100;
  for(int round=0;round<5000;round++) {
    unsigned char expected[97*73]={0};desk.dirty_count=0;
    for(int i=0;i<20;i++) {
      Rect r={(int)(next()%130)-15,(int)(next()%110)-15,(int)(next()%55),(int)(next()%45)};
      Rect clip=intersect(r,(Rect){0,0,97,73});
      for(int y=clip.y;y<clip.y+clip.h;y++)for(int x=clip.x;x<clip.x+clip.w;x++)expected[y*97+x]=1;
      compositor_damage(r);CHECK(desk.dirty_count<=32);
      unsigned char actual[97*73]={0};
      for(int j=0;j<desk.dirty_count;j++) {
        Rect q=desk.dirty[j];CHECK(q.x>=0 && q.y>=0 && q.x+q.w<=97 && q.y+q.h<=73);
        for(int y=q.y;y<q.y+q.h;y++)for(int x=q.x;x<q.x+q.w;x++)CHECK(++actual[y*97+x]==1);
      }
      for(int p=0;p<97*73;p++)CHECK(!expected[p] || actual[p]);
    }
  }
  desk.width=2560;desk.height=1600;desk.scale=200;desk.dirty_count=0;
  Window w={.box={80,90,2410,1365}};damage_native_resize(&w,(Rect){80,90,2402,1357});
  unsigned long area=0;for(int i=0;i<desk.dirty_count;i++)area+=(unsigned long)desk.dirty[i].w*desk.dirty[i].h;
  CHECK(area<2560UL*1600/3);
  puts("PASS production damage: 100000 randomized inserts preserve coverage with disjoint bounds; native border resize repaints less than one third of the screen (ASan/UBSan)");
  return 0;
}
