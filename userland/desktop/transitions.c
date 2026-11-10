#include "desktop.h"

/* Presentation-only crossfade. The retained scene stays exact, and client
   pixels always keep their native size. Only one short transition is active. */
static struct {
  Rect box;
  uint32_t *before, *saved;
  size_t bytes;
  uint64_t at;
  int width,height;
  unsigned duration;
} fade;
void transition_begin(Rect box) {
  if(!desk.ui_motion){if(fade.at)invalidate(fade.box);fade.at=0;return;}
  if (!desk.scene || (desk.drag && desk.drag_mode!=16)) return;
  if (fade.at) invalidate(fade.box);
  box=intersect(box,(Rect){0,0,desk.width,desk.height});
  if (empty(box)) return;
  size_t bytes=(size_t)box.w*box.h*4;
  if (bytes!=fade.bytes) {
    deallocate(fade.before,fade.bytes);deallocate(fade.saved,fade.bytes);
    fade.before=allocate(bytes);fade.saved=allocate(bytes);
    fade.bytes=bytes;
  }
  if (!fade.before || !fade.saved) { fade.at=0;return; }
  fade.box=box;fade.width=desk.width;fade.height=desk.height;fade.duration=desk.ui_motion==2?180:120;
  for(int y=0;y<box.h;y++)memcpy(fade.before+(size_t)y*box.w,
    desk.scene+(size_t)(box.y+y)*desk.width+box.x,box.w*4);
  fade.at=now_ms();
  invalidate(box);
}
void transition_tick(void) {
  if (!fade.at) return;
  invalidate(fade.box);
  if (now_ms()-fade.at>=fade.duration || !desk.ui_motion || (desk.drag && desk.drag_mode!=16) || desk.width!=fade.width || desk.height!=fade.height) fade.at=0;
}
bool transition_present(void) {
  if (!fade.at) return false;
  unsigned d=fade.duration,t=MIN(d,now_ms()-fade.at),alpha=(d-t)*(d-t)*255/(d*d);
  Rect b=fade.box;
  for(int y=0;y<b.h;y++) {
    uint32_t *row=desk.scene+(size_t)(b.y+y)*desk.width+b.x;
    memcpy(fade.saved+(size_t)y*b.w,row,b.w*4);
    for(int x=0;x<b.w;x++)row[x]=coverage_over(fade.before[(size_t)y*b.w+x],row[x],alpha);
  }
  return true;
}
void transition_restore(void) {
  Rect b=fade.box;
  for(int y=0;y<b.h;y++)memcpy(desk.scene+(size_t)(b.y+y)*desk.width+b.x,
    fade.saved+(size_t)y*b.w,b.w*4);
}
void transition_cleanup(void) {
  deallocate(fade.before,fade.bytes);deallocate(fade.saved,fade.bytes);
  memset(&fade,0,sizeof(fade));
}
