#include "../userland/desktop/editor.c"
Desktop desk;
void *malloc(size_t);void free(void *);
static bool fail_alloc;static size_t live;static int notices;
void *allocate(size_t n){if(fail_alloc)return NULL;void *p=malloc(n);if(p)live+=n;return p;}
void deallocate(void *p,size_t n){if(p){live-=n;free(p);}}
void dialog_notice(Window *w,const char *a,const char *b){(void)w;(void)a;(void)b;notices++;}
void dialog_error(Window *w,const char *a,long e){(void)w;(void)a;(void)e;notices++;}
void copy_text(char *out,size_t n,const char *in){if(n){size_t z=MIN(n-1,strlen(in));memcpy(out,in,z);out[z]=0;}}
void append_text(char *out,size_t n,const char *in){size_t z=strlen(out);if(z<n)copy_text(out+z,n-z,in);}
const char *basename(const char *s){const char *p=s;while(*s){if(*s=='/')p=s+1;s++;}return p;}
void invalidate_window(Window *w){(void)w;}void trace(const char *a,Window *w,const char *b){(void)a;(void)w;(void)b;}
int S(int n){return(n*desk.scale+50)/100;}
int clampi(int64_t n,int lo,int hi){return n<lo?lo:n>hi?hi:n;}
Rect window_body(Window *w){return(Rect){w->box.x+1,w->box.y+S(WINDOW_HEADER),w->box.w-2,w->box.h-S(WINDOW_HEADER)-1};}
int text_width(const char *s,Font f){(void)f;return strlen(s)*S(7);}uint64_t now_ms(void){return 0;}
#define CHECK(c)do{if(!(c)){printf("FAIL editor line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
int main(void){
 Window w={.alive=true,.app=APP_EDITOR,.box={0,30,720,460}};Editor *e=allocate(sizeof(*e));memset(e,0,sizeof(*e));w.state=e;desk.scale=100;desk.editor_text_size=11;desk.editor_tab_width=4;
 const char *original="first\r\n\xc3\xa9\tlast\r\nfirst\r\n";strcpy(e->text,original);e->length=strlen(original);e->exists=true;e->original_length=e->length;e->original_hash=hash(14695981039346656037ULL,e->text,e->length);
 e->cursor=0;e->anchor=5;CHECK(replace_selection(&w,"changed",7));CHECK(!memcmp(e->text,"changed\r\n",9));undo_edit(&w,false);CHECK(!strcmp(e->text,original)&&!e->dirty);undo_edit(&w,true);CHECK(!memcmp(e->text,"changed\r\n",9));undo_edit(&w,false);
 e->cursor=e->anchor=7;CHECK(replace_selection(&w,"UTF:",4));CHECK(e->history_count==1);undo_edit(&w,false);CHECK(!strcmp(e->text,original));
 e->readonly=true;size_t n=e->length;CHECK(!replace_selection(&w,"bad",3));CHECK(e->length==n);e->readonly=false;
 fail_alloc=true;CHECK(!replace_selection(&w,"bad",3));CHECK(!strcmp(e->text,original));fail_alloc=false;
 for(int i=0;i<100;i++){e->cursor=e->anchor=e->length;CHECK(replace_selection(&w,"x",1));}CHECK(e->history_count==EDIT_HISTORY);for(int i=0;i<64;i++)undo_edit(&w,false);CHECK(e->length==strlen(original)+36);for(int i=0;i<64;i++)undo_edit(&w,true);CHECK(e->length==strlen(original)+100);
 strcpy(e->find,"first");e->cursor=e->anchor=0;find_next(&w);CHECK(e->anchor==0&&e->cursor==5);find_next(&w);CHECK(e->anchor==16&&e->cursor==21);find_next(&w);CHECK(e->anchor==0);
 for(int scale=100;scale<=200;scale+=25){desk.scale=scale;for(int width=480;width<=1000;width+=20){w.box.w=S(width);w.box.h=S(460);Rect previous={0};for(int id=0;id<15;id++){Rect r;toolbar_layout(&w,id,&r);CHECK(r.x>=0&&r.x+r.w<=w.box.w);CHECK(r.y>=previous.y);if(r.y==previous.y)CHECK(r.x>=previous.x+previous.w);CHECK(r.y+r.h<window_body(&w).y+toolbar_bottom(&w));previous=r;}}}
 editor_close(&w);CHECK(live==0);CHECK(notices==2);puts("PASS production editor byte-exact undo/redo/history cap/redo invalidation/UTF-8+CRLF/find/read-only/OOM and 135 compact toolbar layouts (ASan/UBSan)");return 0;
}
