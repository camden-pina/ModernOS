#include "../userland/desktop/snap.c"
#include "../userland/desktop/game.c"
Desktop desk;
static unsigned pointer_presses;
static int pointer_x, pointer_y, popup_damage;
static uint64_t time_ms;
static uint32_t output[1024*768];
void *mmap(void *,size_t,int,int,int,long);
int mprotect(void *,size_t,int);
int munmap(void *,size_t);
Window *wm_focused(void) { return &desk.windows[0]; }
bool wm_pointer_active(Window *w) { (void)w; return true; }
Rect window_body(Window *w) { return (Rect){w->box.x+1,w->box.y+S(WINDOW_HEADER),w->box.w-2,w->box.h-S(WINDOW_HEADER)-1}; }
bool contains(Rect r,int x,int y) { return x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h; }
long syscall6(long n,long a,long b,long c,long d,long e,long f) {
  (void)c;(void)d;(void)e;(void)f;
  if(n==SYS_window && a==OS_WINDOW_SERVER_SEND) {
    struct os_window_events *batch=(void *)b;
    struct os_window_event *event=(void *)(uintptr_t)batch->events;
    if(event->type==OS_WINDOW_POINTER && (event->buttons&1)) pointer_presses++;
    if(event->type==OS_WINDOW_POINTER) { pointer_x=event->dx;pointer_y=event->dy; }
  }
  return 0;
}
int S(int n) { return (n * desk.scale + 50) / 100; }
int desktop_bottom(void) { return S(72); }
void transition_begin(Rect r) {(void)r;}
void wm_focus(Window *w) {(void)w;}
void damage_window(Window *w) { (void)w; }
void invalidate_window(Window *w) { (void)w; }
void invalidate(Rect r) { if(!memcmp(&r,&desk.snap_box,sizeof(r)))popup_damage++; }
void wm_trace_geometry(Window *w) { (void)w; }
void trace(const char *a, Window *w, const char *b) { (void)a; (void)w; (void)b; }
void wm_fullscreen(Window *w, bool b) { w->fullscreen = b; }
bool empty(Rect r) { return r.w <= 0 || r.h <= 0; }
int clampi(int64_t x,int lo,int hi){return x<lo?lo:x>hi?hi:x;}
uint64_t now_ms(void){return time_ms;}
Rect window_control(Window *w,int i){return (Rect){w->box.x+i*20,w->box.y,20,28};}
Rect intersect(Rect a,Rect b){int x=MAX(a.x,b.x),y=MAX(a.y,b.y);return (Rect){x,y,MAX(0,MIN(a.x+a.w,b.x+b.w)-x),MAX(0,MIN(a.y+a.h,b.y+b.h)-y)};}
uint32_t *paint_pixel(int x,int y){return output+y*1024+x;}
void fill(Rect r,uint32_t color){r=intersect(r,desk.clip);for(int y=r.y;y<r.y+r.h;y++)for(int x=r.x;x<r.x+r.w;x++)output[y*1024+x]=color;}
void label(int x,int y,const char *s,Font f,uint32_t c,int width){(void)x;(void)y;(void)s;(void)f;(void)c;(void)width;}
#define CHECK(x) do { if (!(x)) { printf("FAIL snap line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
  for (int scale = 100; scale <= 200; scale += 25) {
    desk.scale = scale;
    for (int width = 801; width <= 2561; width += 176) {
      desk.width = width; desk.height = width * 5 / 8 + 200;
      Rect work = snap_geometry(-1, 0);
      for (int layout = 0; layout < 6; layout++) {
        int64_t area = 0;
        for (int slot = 0; slot < slots[layout]; slot++) {
          Rect r = snap_geometry(layout, slot);
          CHECK(r.x >= work.x && r.y >= work.y && r.x + r.w <= work.x + work.w && r.y + r.h <= work.y + work.h);
          CHECK(!empty(r)); area += (int64_t)r.w * r.h;
          for (int other = 0; other < slot; other++) {
            Rect q = snap_geometry(layout, other);
            CHECK(r.x >= q.x + q.w || q.x >= r.x + r.w || r.y >= q.y + q.h || q.y >= r.y + r.h);
          }
        }
        CHECK(area == (int64_t)work.w * work.h);
      }
      Window w = {.alive = true, .box = {120, 80, 600, 400}};
      Rect original = w.box;
      snap_apply(&w, 0, 0); snap_apply(&w, 3, 3); snap_apply(&w, -1, 0);
      CHECK(!memcmp(&original, &w.restore, sizeof(original)) && w.maximized && !w.snapped);
    }
  }
  CHECK(empty(snap_geometry(6, 0)) && empty(snap_geometry(3, 4)));
  desk.scale=100;desk.width=1024;desk.height=768;
  desk.snap_assist_enabled=desk.snap_hover_enabled=desk.snap_bar_enabled=true;
  static Game client;
  client.info=(struct os_window_info){.id=1,.version=OS_WINDOW_PIXEL_CLIENT_VERSION,.width=800,.height=600};
  client.painted=true;
  Window *w=&desk.windows[0];
  *w=(Window){.alive=true,.app=APP_WORKBENCH,.id=17,.box={0,0,802,629},.state=&client};
  desk.mouse_x=120;desk.mouse_y=150;
  pixel_pointer(w,0);
  desk.snap_open=true;desk.snap_owner=w;desk.snap_owner_id=w->id;
  desk.snap_box=(Rect){100,100,360,188};desk.buttons=1;
  pixel_pointer(w,0);CHECK(!pointer_presses);
  CHECK(snap_mouse(120,150,1));
  CHECK(!desk.snap_open && desk.snap_pointer_blocked && w->snapped);
  pixel_pointer(w,0);CHECK(!pointer_presses);
  desk.buttons=0;desk.snap_pointer_blocked=false;pixel_pointer(w,0);
  CHECK(!pointer_presses);
  desk.buttons=1;pixel_pointer(w,0);CHECK(pointer_presses==1);
  desk.buttons=0;desk.snap_pointer_blocked=false;
  snap_show(w);popup_damage=0;
  Rect cell=slot_rect(diagram(2),2,1);
  CHECK(snap_mouse(cell.x+4,cell.y+4,0));
  CHECK(desk.snap_layout==2 && desk.snap_slot==1 && popup_damage>0);
  preview(w,0,0);popup_damage=0;preview(w,4,0);
  CHECK(desk.snap_layout==4 && desk.snap_slot==0 && popup_damage>0);
  popup_damage=0;snap_mouse(desk.snap_box.x+1,desk.snap_box.y+1,0);
  CHECK(!desk.snap_preview && popup_damage>0);snap_cancel();
  desk.windows[1]=(Window){.alive=true,.id=18,.z=10,.box={10,20,500,300}};
  desk.windows[2]=(Window){.alive=true,.id=19,.z=9,.box={30,40,500,300}};
  desk.windows[3]=(Window){.alive=true,.id=20,.minimized=true,.z=12,.box={50,60,500,300}};
  desk.windows[4]=(Window){.alive=true,.id=21,.workspace=1,.z=14,.box={70,80,500,300}};
  Rect restore=desk.windows[1].box;
  snap_drag(w,desk.width/2,S(DESKTOP_TOP)+4,false);CHECK(desk.snap_bar);
  cell=slot_rect(diagram(3),3,2);
  snap_drag(w,cell.x+4,cell.y+4,false);CHECK(desk.snap_layout==3 && desk.snap_slot==2);
  snap_drag(w,cell.x+4,cell.y+4,true);
  CHECK(!desk.snap_open && desk.snap_assist && w->snap_slot==2);
  CHECK(assist.next==0 && assist.count==2);
  CHECK(!desk.windows[1].snapped && !desk.windows[2].snapped);
  for(int i=0;i<assist.count;i++) {
    Rect card=assist.cards[i],occupied=snap_geometry(3,2);
    CHECK(empty(intersect(card,occupied)));
    CHECK(snap_window_hidden(assist.candidates[i]));
  }
  assist_choose(1);CHECK(desk.snap_assist && assist.next==1 && assist.count==1);
  CHECK(desk.windows[2].snapped && desk.windows[2].snap_slot==0);
  assist_choose(0);CHECK(!desk.snap_assist && desk.windows[1].snap_slot==1);
  CHECK(!memcmp(&restore,&desk.windows[1].restore,sizeof(restore)));
  CHECK(!desk.windows[3].snapped && !desk.windows[4].snapped);
  arrange(w,2,0);CHECK(desk.snap_assist && assist.count==2);
  assist_choose(0);CHECK(!desk.snap_assist && desk.windows[2].snap_slot==2);
  /* Extra windows remain unsnapped when the selected layout fills. */
  desk.windows[3].minimized=false;
  arrange(w,0,0);CHECK(desk.snap_assist && assist.count==3);
  Window *chosen=assist.candidates[2];
  assist_choose(2);CHECK(!desk.snap_assist && chosen->snapped && chosen->snap_layout==0 && chosen->snap_slot==1);
  /* Closing a candidate prunes it; workspace changes and Escape cancel. */
  arrange(w,3,0);desk.windows[2].alive=false;assist_refresh();
  CHECK(desk.snap_assist && assist.count==2);
  snap_key((Key){.code=KEY_ESCAPE,.down=true});CHECK(!desk.snap_assist);
  desk.windows[3].minimized=true;desk.windows[1].minimized=true;
  arrange(w,2,0);CHECK(!desk.snap_assist);
  static uint32_t pixels[37*23];for(unsigned i=0;i<37*23;i++)pixels[i]=i+1;
  client.pixels=pixels;client.info.width=37;client.info.height=23;
  for(int width=35;width<=401;width+=61) for(int height=27;height<=313;height+=47) {
    w->box=(Rect){3,7,width+2,height+29};desk.clip=(Rect){0,0,1024,768};
    memset(output,0,sizeof(output));game_draw(w);Rect b=window_body(w);
    for(int y=0;y<height;y++)for(int x=0;x<width;x++)
      CHECK(output[(b.y+y)*1024+b.x+x]==pixels[MIN(y,22)*37+MIN(x,36)]);
    desk.mouse_x=b.x+width-1;desk.mouse_y=b.y+height-1;pixel_pointer(w,0);
    CHECK(pointer_x==width-1 && pointer_y==height-1);
  }
  static uint32_t wide[2400*23];for(unsigned i=0;i<2400*23;i++)wide[i]=i+1;
  client.pixels=wide;client.info.width=2400;
  for(int width=2391;width<2500;width+=17) for(int clip=0;clip<2;clip++) {
    w->box=(Rect){3,7,width+2,25+29};
    desk.clip=clip?(Rect){60,50,500,9}:(Rect){0,0,1024,768};
    memset(output,0,sizeof(output));game_draw(w);Rect b=window_body(w);
    Rect area=intersect(intersect(b,desk.clip),(Rect){b.x,b.y,2400,23});
    for(int y=area.y;y<area.y+area.h;y++)for(int x=area.x;x<area.x+area.w;x++)
      CHECK(output[y*1024+x]==wide[(y-b.y)*2400+x-b.x]);
  }
  desk.clip=(Rect){0,0,1024,768};memset(output,0,sizeof(output));
  CHECK(game_thumbnail(w,(Rect){10,10,2400,23}));
  CHECK(output[10*1024+10]==wide[0]);
  client.info.width=800;client.info.height=600;
  w->box=(Rect){3,7,802,629};configure_native_window(w,&client);
  desk.drag=w;desk.drag_mode=10;time_ms=1000;
  w->box.w=702;w->box.h=529;configure_native_window(w,&client);
  CHECK(w->box.w==702 && w->box.h==529);
  CHECK(client.requested_width==700 && client.requested_height==500);
  client.info.width=700;client.info.height=500;configure_native_window(w,&client);
  CHECK(w->box.w==702 && w->box.h==529);
  desk.drag=NULL;
  puts("PASS sequential Snap Assist: explicit choice, final auto-placement, too many/few candidates, closing, cancellation, occupied-space exclusion and restore isolation");
  puts("PASS native pixels and pointer coordinates remain 1:1; border growth/shrink follows input while client redraws asynchronously (ASan/UBSan)");
  puts("PASS native popup capture through release; ordinary editor clicks still delivered (ASan/UBSan)");
  puts("PASS snap geometry: six gap-free non-overlapping layouts, odd work areas, five scales, restore across repeated snapping (ASan/UBSan)");
  return 0;
}
