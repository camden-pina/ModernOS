/* Actual production premultiplied foreground painter, not a copied model. */
#include "../userland/desktop/render.c"
Desktop desk;
static uint32_t pixels[64];
#define CHECK(x) do {if(!(x)){printf("GLASS PIXEL FAIL line=%d\n",__LINE__);__builtin_trap();}}while(0)
static uint32_t shadow_canvas[512 * 512], shadow_full[512 * 512];
static void shadow_checks(void) {
 const int scales[] = {100, 125, 150, 200};
 for (int scale = 0; scale < 4; scale++) for (int dark = 0; dark < 2; dark++)
  for (int shape = 0; shape < 2; shape++) {
   desk.scale=scales[scale];desk.dark_mode=dark;desk.width=desk.height=512;
   desk.paint=shadow_canvas;desk.paint_stride=512;desk.paint_alpha=false;
   desk.clip=(Rect){0,0,512,512};
   Window w={.box={S(50),S(40),S(100),S(80)}};
   int radius=S(shape?16:WINDOW_CORNER);
   fill(desk.clip,0xd0d8e0);window_shadow(&w,radius);
   CHECK(shadow_canvas[(w.box.y+w.box.h-1)*512+w.box.x]!=0xd0d8e0);
   CHECK(shadow_canvas[(w.box.y+w.box.h/2)*512+w.box.x+w.box.w/2]==0xd0d8e0);
   CHECK(shadow_canvas[(w.box.y+S(18))*512+w.box.x-S(4)]>
         shadow_canvas[(w.box.y+w.box.h/2)*512+w.box.x-S(4)]);
   memcpy(shadow_full,shadow_canvas,sizeof(shadow_full));
   fill(desk.clip,0xd0d8e0);
   for (int y=0;y<512;y+=41) for (int x=0;x<512;x+=37) {
    desk.clip=intersect((Rect){x,y,37,41},(Rect){0,0,512,512});
    window_shadow(&w,radius);
   }
   CHECK(!memcmp(shadow_full,shadow_canvas,sizeof(shadow_full)));
  }
 puts("PASS rounded shadows: window/Spotlight cutouts, curved falloff, untouched interiors and tiled/full composition at all four scales in both themes (ASan/UBSan).");
}
int main(void) {
 desk.width=8;desk.height=8;desk.scale=100;desk.paint=pixels;desk.paint_stride=8;desk.clip=(Rect){0,0,8,8};
 fill((Rect){0,0,8,8},0x123456);CHECK(pixels[0]==0x123456);
 desk.paint_alpha=true;fill((Rect){0,0,8,8},0x123456);CHECK(pixels[0]==0xff123456);
 memset(pixels,0,sizeof(pixels));blend((Rect){1,1,2,2},0xff0000,128);CHECK(pixels[9]==0x80800000);CHECK(pixels[0]==0);
 blend((Rect){1,1,2,2},0x0000ff,64);CHECK(pixels[9]==0xa0600040);
 blend((Rect){1,1,2,2},0xffffff,255);CHECK(pixels[9]==0xffffffff);
 CHECK(glass_over(0,0x123456)==0x123456);CHECK(glass_over(0xffabcdef,0x010203)==0xffabcdef);
 CHECK(glass_over(0x80800000,0x0000ff)==0x80007f);
 Window *lower=&desk.windows[0],*upper=&desk.windows[1];
 *lower=(Window){.alive=true,.z=1,.box={0,0,8,8}};
 *upper=(Window){.alive=true,.z=2,.box={4,0,4,8}};
 desk.mouse_x=6;desk.mouse_y=3;CHECK(!wm_pointer_active(lower));CHECK(wm_pointer_active(upper));
 desk.mouse_x=2;CHECK(wm_pointer_active(lower));CHECK(!wm_pointer_active(upper));
 desk.launcher=true;desk.launcher_box=(Rect){1,1,2,3};CHECK(!wm_pointer_active(lower));desk.launcher=false;
 desk.dialog.open=true;CHECK(!wm_pointer_active(lower));desk.dialog.open=false;
 upper->minimized=true;desk.mouse_x=6;CHECK(wm_pointer_active(lower));
 shadow_checks();
 puts("PASS pointer ownership: occluded underlay, exposed lower window, launcher, modal and minimized windows.");
 puts("PASS production cached painter: opaque fills, transparent clear, premultiplied partial coverage, source-over accumulation and compositing over actual RGB backdrop (ASan/UBSan).");
 return 0;
}
