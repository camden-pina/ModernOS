/* Host sanitizers execute the production Terminal and clipboard clients. */
#include "../../userland/desktop/terminal.c"
#include "../../userland/desktop/terminal_screen.c"
#include "../../userland/desktop/terminal_reflow.c"
#include "../../userland/desktop/clipboard.c"
#include "../../userland/desktop/menubar.c"
#include <text_utf8.h>
extern void *malloc(size_t);
extern void free(void *);
#define REQUIRE(x) do {if(!(x)){printf("TERMINAL HOST FAIL line=%d expression=%s\n",__LINE__,#x);__builtin_trap();}}while(0)
Desktop desk;
const AppOps apps[APP_COUNT]={[APP_TERMINAL]={.key=terminal_key}};
Window *wm_open(App app,const char *path){(void)app;(void)path;__builtin_trap();}
void wm_close(Window *w){(void)w;__builtin_trap();}
static Window window;
static uint64_t clock_ms=1000,generation=1;
static unsigned foreground=100,stale_reads;
static struct os_termios mode={.lflag=ICANON|ISIG};
static char shared[OS_CLIPBOARD_LIMIT],delivered[PASTE_CAPACITY];
static size_t shared_length,delivered_length,write_chunk=7;
static int again,kill_calls,last_error;
static struct {void *p;size_t n;} allocations[32];
static unsigned allocated;
void *allocate(size_t n){void *p=malloc(n);REQUIRE(p && allocated<32);allocations[allocated].p=p;allocations[allocated++].n=n;return p;}
void deallocate(void *p,size_t n){if(!p)return;unsigned i=0;while(i<allocated && allocations[i].p!=p)i++;REQUIRE(i<allocated && allocations[i].n==n);allocations[i]=allocations[--allocated];free(p);}
int S(int v){return v;}
Rect window_body(Window *w){(void)w;return (Rect){0,0,800,600};}
const char *error_text(long error){(void)error;return "error";}
int clampi(int64_t v,int a,int b){return v<a?a:v>b?b:(int)v;}
void copy_text(char *out,size_t cap,const char *in){if(!cap)return;size_t n=strlen(in);if(n>=cap)n=cap-1;memcpy(out,in,n);out[n]=0;}
void append_text(char *out,size_t cap,const char *in){size_t n=strlen(out);if(n<cap)copy_text(out+n,cap-n,in);}
void format_uint(char *out,uint64_t n){char b[32];unsigned i=0;do{b[i++]='0'+n%10;n/=10;}while(n);unsigned j=0;while(i)out[j++]=b[--i];out[j]=0;}
uint64_t now_ms(void){return clock_ms;}
void trace(const char *e,Window *w,const char *d){(void)e;(void)w;(void)d;}
void invalidate_window(Window *w){(void)w;}
void invalidate(Rect r){(void)r;}
Window *wm_focused(void){return &window;}
void dialog_notice(Window *w,const char *title,const char *message){desk.dialog.open=true;desk.dialog.owner=w;desk.dialog.action=NULL;copy_text(desk.dialog.title,sizeof(desk.dialog.title),title);copy_text(desk.dialog.message,sizeof(desk.dialog.message),message);}
void dialog_error(Window *w,const char *action,long error){last_error=error;dialog_notice(w,action,"error");}
void dialog_confirm(Window *w,const char *title,const char *message,DialogAction action){dialog_notice(w,title,message);desk.dialog.action=action;}
int ioctl(int fd,unsigned request,void *data){REQUIRE(fd==3);if(request==OS_PTY_GET_FOREGROUND){*(unsigned *)data=foreground;return 0;}if(request==TCGETS){*(struct os_termios *)data=mode;return 0;}return -E_NOTSUP;}
long write(int fd,const void *p,size_t n){REQUIRE(fd==3);if(again){again--;return -E_AGAIN;}if(n>write_chunk)n=write_chunk;REQUIRE(n<=sizeof(delivered)-delivered_length);memcpy(delivered+delivered_length,p,n);delivered_length+=n;return n;}
int close(int fd){(void)fd;return 0;}
long syscall6(long number,long a,long b,long c,long d,long e,long f){(void)e;(void)f;if(number==SYS_kill){kill_calls++;return 0;}if(number!=SYS_clipboard)return -E_NOTSUP;
 if(a==OS_CLIPBOARD_INFO){if(c!=sizeof(struct os_clipboard_info))return -E_INVAL;*(struct os_clipboard_info *)b=(struct os_clipboard_info){generation,shared_length,OS_CLIPBOARD_LIMIT};return 0;}
 if(a==OS_CLIPBOARD_READ){if(stale_reads){stale_reads--;return -E_AGAIN;}if(d && (uint64_t)d!=generation)return -E_AGAIN;if((size_t)c<shared_length)return -E_NOSPC;memcpy((void *)b,shared,shared_length);return shared_length;}
 if(a==OS_CLIPBOARD_WRITE){if(c<0 || c>OS_CLIPBOARD_LIMIT)return -E_2BIG;if(!text_utf8_valid((const char *)b,c))return -E_INVAL;memcpy(shared,(void *)b,c);shared_length=c;generation++;return c;}
 return -E_INVAL;
}
bool input_key(char *text,size_t cap,int *cursor,int *anchor,Key k){(void)text;(void)cap;(void)cursor;(void)anchor;(void)k;return false;}
bool terminal_is_preferences(Window *w){(void)w;return false;}
bool terminal_prefs_close(Window *w){(void)w;return true;}
void terminal_preferences_open(Window *w){(void)w;__builtin_trap();}
static Terminal *setup(void){memset(&desk,0,sizeof(desk));memset(&window,0,sizeof(window));window.alive=true;window.app=APP_TERMINAL;window.state=allocate(sizeof(Terminal));Terminal *t=window.state;memset(t,0,sizeof(*t));vt_init(&t->screen);t->pid=100;t->read_fd=t->write_fd=3;foreground=100;mode.lflag=ICANON|ISIG;delivered_length=0;again=0;kill_calls=0;last_error=0;clock_ms=1000;return t;}
static void finish(Terminal *t){t->pid=0;t->read_fd=t->write_fd=-1;terminal_close(&window);REQUIRE(!allocated && !window.state);}
static void set_copy(const char *text){REQUIRE(clipboard_write(text,strlen(text))==0);}
static void drain(Terminal *t){for(unsigned i=0;i<20000 && t->paste;i++){clock_ms++;input_flush(&window);paste_tick(&window);}REQUIRE(!t->paste);}
int main(void){
 Terminal *t=setup();set_copy("two");terminal_paste(&window);REQUIRE(t->paste_ready && !delivered_length);drain(t);REQUIRE(delivered_length==3 && !memcmp(delivered,"two",3));finish(t);
 t=setup();set_copy("echo FIRST\nprintf SECOND\nTAIL");terminal_paste(&window);REQUIRE(t->paste && !t->paste_ready && desk.dialog.open && !delivered_length);set_copy("CHANGED AFTER REVIEW");desk.dialog.open=false;paste_accept(&window,"");REQUIRE(t->paste_ready);again=1;paste_tick(&window);REQUIRE(!t->paste_offset && !delivered_length);drain(t);const char *expected="echo FIRST\nprintf SECOND\nTAIL";REQUIRE(delivered_length==strlen(expected) && !memcmp(delivered,expected,delivered_length));finish(t);
 t=setup();set_copy("a\nb");terminal_paste(&window);desk.dialog.open=false;paste_tick(&window);REQUIRE(!t->paste && !delivered_length);finish(t);
 t=setup();set_copy("first\nsecond\n");terminal_paste(&window);foreground=101;desk.dialog.open=false;paste_accept(&window,"");REQUIRE(!t->paste && !delivered_length);finish(t);
 t=setup();set_copy("first\nsecond\n");terminal_paste(&window);desk.dialog.open=false;paste_accept(&window,"");paste_tick(&window);size_t before=delivered_length;REQUIRE(before>0 && t->paste);foreground=101;paste_tick(&window);REQUIRE(!t->paste && delivered_length==before);finish(t);
 t=setup();t->screen.bracketed_paste=true;mode.lflag=ISIG;set_copy("small text");terminal_paste(&window);REQUIRE(t->raw_mode && t->paste_raw);again=2;drain(t);expected="\033[200~small text\033[201~";REQUIRE(delivered_length==strlen(expected) && !memcmp(delivered,expected,delivered_length));finish(t);
 t=setup();mode.lflag=ISIG;set_copy("raw");terminal_paste(&window);again=1;paste_tick(&window);clock_ms+=30001;again=1;paste_tick(&window);REQUIRE(!t->paste && !delivered_length);finish(t);
 t=setup();t->screen.cols=20;t->screen.rows=3;for(const char *p="first\r\nsecond";*p;p++)vt_byte(&t->screen,*p);t->selection_valid=true;t->selection_start_col=1;t->selection_end_row=1;t->selection_end_col=3;terminal_copy(&window);REQUIRE(shared_length==8 && !memcmp(shared,"irst\nsec",8));t->selection_start_col=0;t->selection_end_row=0;t->selection_end_col=1;t->screen.cells[0][0].ch=0x3b1;terminal_copy(&window);REQUIRE(shared_length==2 && (unsigned char)shared[0]==0xce && (unsigned char)shared[1]==0xb1);finish(t);
 t=setup();set_copy("line one\nline two\n");terminal_paste(&window);Key escape={.code=KEY_ESCAPE,.down=true};terminal_key(&window,escape);REQUIRE(!t->paste && !delivered_length);finish(t);
 t=setup();Key interrupt={.code=6,.ascii='c',.mods=MOD_CTRL,.down=true};terminal_key(&window,interrupt);REQUIRE(delivered_length==1 && delivered[0]==3 && !kill_calls);foreground=101;terminal_key(&window,interrupt);REQUIRE(delivered_length==2 && delivered[1]==3 && !kill_calls);finish(t);
 t=setup();set_copy("repeat\n");terminal_paste(&window);finish(t);
 /* A partial typing prefix must finish before a menu paste, even when the
    foreground program switches terminal mode just before the menu action. */
 t=setup();write_chunk=2;again=2;input_queue(&window,"pre",3);REQUIRE(t->input_length);
 mode.lflag=0;set_copy("fix");terminal_command(&window,3);REQUIRE(t->raw_mode && t->paste && t->paste_raw);
 paste_tick(&window);REQUIRE(!delivered_length && !t->paste_offset);
 drain(t);REQUIRE(!t->input_length && delivered_length==6 && !memcmp(delivered,"prefix",6));write_chunk=7;finish(t);
 t=setup();again=100;input_queue(&window,"pre",3);set_copy("fix");terminal_paste(&window);clock_ms+=30001;paste_tick(&window);REQUIRE(!t->paste && !delivered_length);finish(t);
 set_copy("snapshot");char *text;size_t length;stale_reads=4;REQUIRE(clipboard_read(&text,&length)==-E_AGAIN && !text && !length && !allocated);
 puts("PASS actual Terminal frozen review, cancellation, UTF-8 selection, partial/EAGAIN FIFO ordering, live mode, bracketed paste, timeout, foreground change, Ctrl+C routing and allocation cleanup");return 0;
}
