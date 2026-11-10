#include "../userland/desktop/terminal_preferences_window.c"
Desktop desk;void *malloc(size_t);void free(void *);static int active,error,closed,saved,changes;static size_t live;
void *allocate(size_t n){void *p=malloc(n);if(p)live+=n;return p;}void deallocate(void *p,size_t n){if(p){live-=n;free(p);}}
unsigned profiles_active(void){return active;}int preferences_save(Window *w){(void)w;saved++;return error;}
void terminal_preferences_changed(void){changes++;}void invalidate_window(Window *w){(void)w;}
void trace(const char *a,Window *w,const char *b){(void)a;(void)w;(void)b;}
void wm_close(Window *w){closed++;terminal_prefs_close(w);w->alive=false;}
void copy_text(char *o,size_t n,const char *s){size_t z=MIN(n-1,strlen(s));memcpy(o,s,z);o[z]=0;}
int clampi(int64_t n,int lo,int hi){return n<lo?lo:n>hi?hi:n;}int S(int n){return(n*desk.scale+50)/100;}
Rect window_body(Window *w){return(Rect){w->box.x+1,w->box.y+S(WINDOW_HEADER),w->box.w-2,w->box.h-S(WINDOW_HEADER)-1};}
#define CHECK(c)do{if(!(c)){printf("FAIL prefs line %d %s\n",__LINE__,#c);exit(1);}}while(0)
int main(void){Window w={.alive=true,.app=APP_TERMINAL,.box={0,30,620,492}};desk.scale=100;desk.terminal_text_size=11;desk.terminal_opacity=100;
 CHECK(!terminal_prefs_init(&w));CHECK(terminal_is_preferences(&w));Preferences *p=w.state;activate(&w,12);CHECK(p->palette==2&&desk.terminal_palette==0&&p->changed);activate(&w,70);CHECK(desk.terminal_palette==2&&!p->changed&&saved==1&&changes==1);
 activate(&w,13);active=1;activate(&w,70);CHECK(desk.terminal_palette==2&&saved==1);CHECK(!apply(&w));active=0;
 error=-E_ROFS;CHECK(!apply(&w));CHECK(p->changed&&desk.terminal_palette==3);error=0;
 activate(&w,14);activate(&w,71);CHECK(closed==1&&desk.terminal_palette==3&&live==0);
 w.alive=true;CHECK(!terminal_prefs_init(&w));p=w.state;CHECK(p->palette==3&&!p->changed);
 for(int scale=100;scale<=200;scale+=25){desk.scale=scale;w.box=(Rect){0,S(22),S(620),S(340)};for(int page=0;page<3;page++){activate(&w,page);int list[40],n=ids(&w,list);CHECK(n<=40);p->scroll=max_scroll(&w);for(int i=0;i<n;i++){Rect r=control(&w,list[i]);CHECK(r.w>0&&r.h>0);CHECK(r.x>=0&&r.x+r.w<=w.box.w);if(list[i]>=70)CHECK(r.y+r.h<=w.box.y+w.box.h);}}}
 terminal_prefs_close(&w);CHECK(live==0);puts("PASS production Terminal preferences draft/apply/cancel/reopen/profile-switch guard/persistence failure and compact DPI layouts (ASan/UBSan)");return 0;}
