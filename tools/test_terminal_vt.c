/* Exercise the production VT parser/reflow without a GUI or guest filesystem. */
#include "../userland/desktop/terminal_screen.c"
#include "../userland/desktop/terminal_reflow.c"
Desktop desk;
void *malloc(size_t);void free(void *);
static bool fail_allocate;static size_t live;
void *allocate(size_t n){if(fail_allocate)return NULL;void *p=malloc(n);if(p)live+=n;return p;}
void deallocate(void *p,size_t n){if(p){live-=n;free(p);}}
int S(int n){return n;}
int clampi(int64_t n,int a,int b){return n<a?a:n>b?b:n;}
void copy_text(char *out,size_t n,const char *in){if(n){size_t z=MIN(n-1,strlen(in));memcpy(out,in,z);out[z]=0;}}
void append_text(char *out,size_t n,const char *in){size_t z=strlen(out);if(z<n)copy_text(out+z,n-z,in);}
void format_uint(char *out,uint64_t n){char tmp[32];unsigned z=0;do{tmp[z++]='0'+n%10;n/=10;}while(n);unsigned i=0;while(z)out[i++]=tmp[--z];out[i]=0;}
#define CHECK(c) do{if(!(c)){printf("FAIL VT line=%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static VtScreen t;
static void feed(const char *s){while(*s)vt_byte(&t,(unsigned char)*s++);}
static void text(char *out,size_t cap){size_t n=0;for(int r=0;r<=t.history_count+t.y;r++){const VtCell *p=vt_row(&t,r);int end=t.cols;if(!vt_wrapped(&t,r))while(end && (!p[end-1].ch || p[end-1].ch==' '))end--;for(int c=0;c<end && n+2<cap;c++)out[n++]=p[c].ch<128?p[c].ch:'?';if(!vt_wrapped(&t,r) && r<t.history_count+t.y && n+2<cap)out[n++]='\n';}out[n]=0;}
int main(void){
 vt_init(&t);CHECK(vt_resize(&t,5,3));feed("abcdefghijK");CHECK(t.wrapped[0] && t.wrapped[1]);CHECK(t.y==2 && t.x==1);feed("\r\nL\r\nM");CHECK(t.history_count==2);CHECK(vt_row(&t,0)[0].ch=='a');vt_scroll(&t,2);CHECK(vt_view_row(&t,0)==0);feed("\r\nN");CHECK(t.view_offset==3);
 vt_init(&t);CHECK(vt_resize(&t,8,6));const char *source="prompt$ abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz";feed(source);char before[4096],after[4096];text(before,sizeof(before));CHECK(!strcmp(before,source));CHECK(vt_resize(&t,13,8));text(after,sizeof(after));CHECK(!strcmp(before,after));CHECK(vt_resize(&t,5,4));text(after,sizeof(after));CHECK(!strcmp(before,after));CHECK(vt_resize(&t,20,10));text(after,sizeof(after));CHECK(!strcmp(before,after));CHECK(live==0);
 int cols=t.cols,rows=t.rows;fail_allocate=true;CHECK(!vt_resize(&t,cols+1,rows));CHECK(t.resize_failed && t.cols==cols && t.rows==rows);fail_allocate=false;CHECK(vt_resize(&t,cols+1,rows));CHECK(!t.resize_failed);
 vt_init(&t);vt_resize(&t,12,3);for(int i=0;i<600;i++)feed("x\r\n");CHECK(t.history_count==VT_HISTORY && t.history_generation>0);int old_history=t.history_count,old_x=t.x,old_y=t.y;feed("\033[?1049h");for(int i=0;i<20;i++)feed("alternate\r\n");CHECK(t.alternate_screen && t.history_count==old_history);feed("\033[?1049l");CHECK(!t.alternate_screen && t.x==old_x && t.y==old_y && t.history_count==old_history);
 feed("\033[?1049h");CHECK(vt_resize(&t,16,5));feed("\033[?1049l");CHECK(!t.alternate_screen && t.cols==16 && t.rows==5);
 vt_init(&t);vt_resize(&t,5,3);feed("abcde\rZ");CHECK(t.y==0 && t.x==1 && t.cells[0][0].ch=='Z');feed("\033[2;3H\033[6n");char reply[64]={0};size_t n=vt_response(&t,reply,sizeof(reply)-1);CHECK(n==6 && !memcmp(reply,"\033[2;3R",6));feed("\033[5n");CHECK(vt_response(&t,reply,2)==2 && vt_response(&t,reply+2,62)==2 && !memcmp(reply,"\033[0n",4));
 vt_init(&t);feed("\xf0\x9f\x98\x80");CHECK(t.cells[0][0].ch==0x1f600);char key[16];CHECK(vt_key(&t,(Key){.down=true,.ascii='c',.mods=MOD_CTRL},key,sizeof(key))==1 && key[0]==3);CHECK(vt_key(&t,(Key){.down=true,.code=KEY_ENTER},key,sizeof(key))==1 && key[0]=='\r');CHECK(vt_key(&t,(Key){.down=true,.code=KEY_UP},key,sizeof(key))==3 && !memcmp(key,"\033[A",3));CHECK(live==0);
 puts("PASS production VT wrap/reflow/height resize/history cap/anchored scrolling/alternate restoration/DSR/Unicode/key bytes and allocation-failure recovery (ASan/UBSan)");return 0;
}
