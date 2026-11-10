#!/usr/bin/env python3
"""Sanitize the actual native cursor rasterizer at every scale and screen edge."""
import argparse, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define COAST_DESKTOP_H
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define WINDOW_HEADER 28
#define DESKTOP_TOP 22
typedef struct {int x,y,w,h;} Rect;
enum {APP_FILES,APP_TERMINAL,APP_BROWSER,APP_SETTINGS,APP_EDITOR,APP_POWER,APP_CODE};
typedef struct {int app;bool fullscreen;Rect box;} Window;
typedef struct {Rect (*toolbar_hover)(Window*,int,int);} AppOps;
static AppOps apps[12];
static struct {
 int scale,scale_index,mouse_x,mouse_y,width,height,drag_mode;
 unsigned cursor_kind;bool cursor_dirty,trace,launcher,snap_assist;
 Window *drag;
 struct {bool open,input;Rect box;} dialog;
 struct {bool open;Rect box;} menu;
 Rect launcher_box;
 uint32_t *output;
} desk;
static int S(int n){return (n*desk.scale+50)/100;}
static bool contains(Rect r,int x,int y){return x>=r.x&&y>=r.y&&x<r.x+r.w&&y<r.y+r.h;}
static bool empty(Rect r){return r.w<=0||r.h<=0;}
static uint32_t coverage_over(uint32_t f,uint32_t b,unsigned a){
 uint32_t out=0;for(unsigned s=0;s<24;s+=8)out|=(((((f>>s)&255)*a+((b>>s)&255)*(255-a)+127)/255)<<s);return out;
}
static bool game_cursor_draw(void){return false;}
static bool game_cursor_bounds(Rect *r){(void)r;return false;}
static bool game_pointer_captured(void){return false;}
static bool wm_fullscreen_active(void){return false;}
static Window *wm_hit_test(int x,int y){(void)x;(void)y;return NULL;}
static int wm_resize_edges(Window *w,int x,int y){(void)w;(void)x;(void)y;return 0;}
static int desktop_bottom(void){return 80;}
static Rect window_body(Window *w){return w->box;}
static Rect window_control(Window *w,int i){(void)w;(void)i;return (Rect){0};}
static bool game_window_loading(Window *w){(void)w;return false;}
static bool terminal_is_preferences(Window *w){(void)w;return false;}
#define HOVER(name) static Rect name(Window *w,int x,int y){(void)w;(void)x;(void)y;return (Rect){0};}
HOVER(menubar_hover) HOVER(editor_hover) HOVER(files_hover) HOVER(settings_hover)
HOVER(power_hover) HOVER(codeoss_hover)
#define TEXT(name) static bool name(Window *w,int x,int y){(void)w;(void)x;(void)y;return false;}
TEXT(editor_text_hit) TEXT(codeoss_text_hit) TEXT(files_text_hit) TEXT(browser_text_hit)
static Rect taskbar_hover(int x,int y){(void)x;(void)y;return (Rect){0};}
static Rect dialog_hover(int x,int y){(void)x;(void)y;return (Rect){0};}
#include "userland/desktop/cursor.c"
int main(int argc,char **argv){
 assert(argc==2);desk.width=640;desk.height=480;
 desk.output=malloc(640*480*4);assert(desk.output);
 const int scales[]={100,125,150,200};
 const int points[][2]={{0,0},{639,0},{0,479},{639,479},{320,240}};
 unsigned cases=0;
 for(int scale=0;scale<4;scale++){
  desk.scale=scales[scale];desk.scale_index=scale;
  for(unsigned kind=0;kind<C_NATIVE;kind++){
   Cursor *c=prepare(kind);assert(c->width<=64&&c->height<=64&&c->hot_x>=0&&c->hot_y>=0);
   for(unsigned point=0;point<5;point++){
    for(unsigned i=0;i<640*480;i++)desk.output[i]=0x8495a6;
    desk.mouse_x=points[point][0];desk.mouse_y=points[point][1];desk.cursor_kind=kind;
    desktop_cursor_draw();unsigned changed=0;
    Rect bounds={desk.mouse_x-c->hot_x,desk.mouse_y-c->hot_y,c->width,c->height};
    for(int y=0;y<480;y++)for(int x=0;x<640;x++)if(desk.output[y*640+x]!=0x8495a6){
     assert(contains(bounds,x,y));changed++;
    }
    assert(point!=4||changed>20);cases++;
   }
  }
 }
 desk.scale=200;desk.scale_index=3;
 uint32_t *atlas=malloc(720*80*4);assert(atlas);free(desk.output);desk.output=atlas;
 desk.width=720;desk.height=80;
 for(int i=0;i<720*80;i++)atlas[i]=0xbed0e0;
 for(unsigned kind=0;kind<C_NATIVE;kind++){
  desk.mouse_x=36+kind*72;desk.mouse_y=36;desk.cursor_kind=kind;desktop_cursor_draw();
 }
 FILE *f=fopen(argv[1],"wb");assert(f);fprintf(f,"P6\n720 80\n255\n");
 for(int i=0;i<720*80;i++){uint32_t p=atlas[i];unsigned char rgb[]={p>>16,p>>8,p};assert(fwrite(rgb,1,3,f)==3);}
 fclose(f);free(atlas);
 printf("PASS actual native cursor rasterizer: %u scale/operation/edge cases, bounded hotspots, ASan/UBSan and atlas\n",cases);
}
'''

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output-root',type=Path,required=True);a=p.parse_args()
 out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False)
 source=out/'cursor.c';source.write_text(HARNESS);binary=out/'cursor'
 subprocess.run(['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT),str(source),'-o',str(binary)],check=True)
 result=subprocess.run([str(binary),str(out/'cursor-atlas.ppm')],check=True,capture_output=True,text=True)
 (out/'result.txt').write_text(result.stdout);print(result.stdout,end='')
 from PIL import Image
 Image.open(out/'cursor-atlas.ppm').save(out/'cursor-atlas.png')
if __name__=='__main__':main()
