#include "desktop.h"

enum { C_ARROW, C_TEXT, C_HAND, C_GRAB, C_GRABBING, C_EW, C_NS, C_NWSE,
       C_NESW, C_BUSY, C_NATIVE };
typedef struct { float x, y; } Point;
typedef struct {
  int width, height, hot_x, hot_y;
  bool ready;
  uint8_t outer[64 * 64], inner[64 * 64];
} Cursor;
static Cursor cursors[4][C_NATIVE];
static bool inside(const Point *p, int count, float x, float y) {
  bool hit = false;
  for (int i = 0, j = count - 1; i < count; j = i++)
    if ((p[i].y > y) != (p[j].y > y) &&
        x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x)
      hit = !hit;
  return hit;
}
static bool segment(float x, float y, float ax, float ay, float bx, float by,
                    float radius) {
  float dx = bx - ax, dy = by - ay;
  float t = ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy);
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  x -= ax + t * dx; y -= ay + t * dy;
  return x * x + y * y <= radius * radius;
}
static bool ink(unsigned kind, float x, float y) {
  static const Point arrow[] = {{1,1},{1,18},{5.2f,14},{8.2f,20},
                                {11,18.8f},{8.1f,12.7f},{14,12.7f}};
  static const Point hand[] = {{7,11},{7,3},{8,2},{9,2},{10,3},{10,9},
      {12,8},{14,9},{16,9},{19,11},{19,17},{16,22},{8,22},{3,15},
      {3,13},{5,12},{7,14}};
  static const Point grab[] = {{4,13},{3,8},{4,7},{6,8},{7,12},{6,4},
      {7,3},{9,4},{10,11},{10,3},{12,3},{13,11},{14,5},{16,5},{16,13},
      {18,9},{20,10},{20,15},{16,22},{8,22},{3,16},{2,14}};
  static const Point fist[] = {{4,13},{4,8},{6,7},{8,8},{10,7},{12,8},
      {14,8},{17,10},{18,15},{15,21},{8,21},{3,15},{2,12}};
  if (kind == C_ARROW) return inside(arrow, 7, x, y);
  if (kind == C_HAND) return inside(hand, sizeof(hand)/sizeof(*hand), x, y);
  if (kind == C_GRAB) return inside(grab, sizeof(grab)/sizeof(*grab), x, y);
  if (kind == C_GRABBING) return inside(fist, sizeof(fist)/sizeof(*fist), x, y);
  if (kind == C_TEXT)
    return segment(x,y,12,3,12,22,.7f) || segment(x,y,8,3,16,3,.7f) ||
           segment(x,y,8,22,16,22,.7f);
  if (kind == C_BUSY) {
    x -= 12; y -= 12;
    float radius = x*x+y*y;
    return radius >= 30 && radius <= 78;
  }
  /* Rotate one double-headed arrow for all four resize directions. */
  x -= 12; y -= 12;
  float u = x, v = y;
  if (kind == C_NS) { u = y; v = x; }
  if (kind == C_NWSE) { u = (x+y)*.7071068f; v = (y-x)*.7071068f; }
  if (kind == C_NESW) { u = (x-y)*.7071068f; v = (x+y)*.7071068f; }
  return segment(u,v,-9,0,9,0,.9f) || segment(u,v,-9,0,-5,-4,.9f) ||
      segment(u,v,-9,0,-5,4,.9f) || segment(u,v,9,0,5,-4,.9f) ||
      segment(u,v,9,0,5,4,.9f);
}
static Cursor *prepare(unsigned kind) {
  Cursor *c = &cursors[desk.scale_index][kind];
  if (c->ready) return c;
  c->width = S(25); c->height = S(25);
  c->hot_x = c->hot_y = S(12);
  if (kind == C_ARROW) { c->width=S(16); c->height=S(22); c->hot_x=c->hot_y=0; }
  if (kind == C_HAND) { c->hot_x=S(8); c->hot_y=S(3); }
  for (int y=0; y<c->height; y++) for (int x=0; x<c->width; x++) {
    unsigned a=0,b=0;
    for (int sy=0; sy<4; sy++) for (int sx=0; sx<4; sx++) {
      float px=(x+(sx+.5f)/4)*100/desk.scale;
      float py=(y+(sy+.5f)/4)*100/desk.scale;
      bool center=ink(kind,px,py);
      b+=center;
      a+=center || ink(kind,px-1,py) || ink(kind,px+1,py) ||
          ink(kind,px,py-1) || ink(kind,px,py+1);
    }
    c->outer[y*64+x]=(a*255+8)/16;
    c->inner[y*64+x]=(b*255+8)/16;
  }
  c->ready=true;
  return c;
}
static unsigned resize_cursor(int edges) {
  if ((edges&3) && (edges&12))
    return edges==5 || edges==10 ? C_NWSE : C_NESW;
  return edges&3 ? C_EW : C_NS;
}
static unsigned choose(void) {
  int x=desk.mouse_x,y=desk.mouse_y;
  if (desk.drag) return desk.drag_mode==16 ? C_GRABBING : resize_cursor(desk.drag_mode);
  if (desk.snap_assist) return C_ARROW;
  if (desk.dialog.open) {
    Rect b=desk.dialog.box;
    if (desk.dialog.input && contains((Rect){b.x+S(22),b.y+b.h-S(83),b.w-S(44),S(28)},x,y)) return C_TEXT;
    return empty(dialog_hover(x,y)) ? C_ARROW : C_HAND;
  }
  if (desk.menu.open && contains(desk.menu.box,x,y)) return C_HAND;
  if (desk.launcher && contains(desk.launcher_box,x,y)) {
    Rect b=desk.launcher_box;
    return contains((Rect){b.x+S(54),b.y+S(12),b.w-S(108),S(40)},x,y) ? C_TEXT : C_HAND;
  }
  if (!wm_fullscreen_active() && (y<S(DESKTOP_TOP) || y>=desk.height-desktop_bottom()))
    return empty(taskbar_hover(x,y)) ? C_ARROW : C_HAND;
  Window *w=wm_hit_test(x,y);
  if (!w) return C_ARROW;
  int edges=wm_resize_edges(w,x,y);
  if (edges) return resize_cursor(edges);
  if (w->app==APP_FILES && files_text_hit(w,x,y)) return C_TEXT;
  if (!w->fullscreen && y<w->box.y+S(WINDOW_HEADER)) {
    for (int i=0;i<3;i++) if (contains(window_control(w,i),x,y)) return C_HAND;
    if (apps[w->app].toolbar_hover && !empty(apps[w->app].toolbar_hover(w,x,y))) return C_HAND;
    return C_GRAB;
  }
  if (!empty(menubar_hover(w,x,y))) return C_HAND;
  if (game_window_loading(w)) return C_BUSY;
  if ((w->app==APP_EDITOR && editor_text_hit(w,x,y)) ||
      (w->app==APP_CODE && codeoss_text_hit(w,x,y)) ||
      (w->app==APP_TERMINAL && !terminal_is_preferences(w) && contains(window_body(w),x,y))) return C_TEXT;
  Rect r={0};
  if (w->app==APP_EDITOR) r=editor_hover(w,x,y);
  if (w->app==APP_FILES) r=files_hover(w,x,y);
  if (w->app==APP_SETTINGS) r=settings_hover(w,x,y);
  if (w->app==APP_POWER) r=power_hover(w,x,y);
  if (w->app==APP_CODE) r=codeoss_hover(w,x,y);
  return empty(r) ? C_ARROW : C_HAND;
}
Rect desktop_cursor_bounds(void) {
  if (game_pointer_captured()) return (Rect){0};
  Rect r;
  unsigned kind;
  if (!desk.drag && game_cursor_bounds(&r)) kind=C_NATIVE;
  else {
    kind=choose();
    Cursor *c=prepare(kind);
    r=(Rect){desk.mouse_x-c->hot_x,desk.mouse_y-c->hot_y,c->width,c->height};
  }
  if (kind!=desk.cursor_kind) {
    desk.cursor_dirty=true;
    if (desk.trace) printf("DESKTOP CURSOR kind=%u mouse=%d,%d\n",kind,desk.mouse_x,desk.mouse_y);
  }
  desk.cursor_kind=kind;
  return r;
}
void desktop_cursor_draw(void) {
  if (desk.cursor_kind==C_NATIVE) { game_cursor_draw(); return; }
  Cursor *c=prepare(desk.cursor_kind);
  int left=desk.mouse_x-c->hot_x,top=desk.mouse_y-c->hot_y;
  bool light=desk.cursor_kind==C_ARROW || desk.cursor_kind==C_HAND ||
             desk.cursor_kind==C_GRAB || desk.cursor_kind==C_GRABBING;
  for (int y=0;y<c->height;y++) for (int x=0;x<c->width;x++) {
    int px=left+x,py=top+y;
    if (px<0 || py<0 || px>=desk.width || py>=desk.height) continue;
    unsigned a=c->outer[y*64+x],b=c->inner[y*64+x];
    if (!a) continue;
    uint32_t *target=desk.output+(size_t)py*desk.width+px;
    *target=coverage_over(light?0x25303B:0xFCFDFE,*target,a);
    if (b) *target=coverage_over(light?0xFCFDFE:desk.cursor_kind==C_BUSY?0x2479F2:0x25303B,*target,b);
  }
}
