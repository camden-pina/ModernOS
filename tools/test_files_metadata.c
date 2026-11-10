#define open metadata_open
#define read metadata_read
#define close metadata_close
#include "../userland/desktop/file_manager.c"
#undef open
#undef read
#undef close
Desktop desk;
void *malloc(size_t);void free(void *);
static char saved[160000];static size_t offset;static int fail_write;
void *allocate(size_t n){return malloc(n);}
void deallocate(void *p,size_t n){(void)n;free(p);}
void copy_text(char *out,size_t n,const char *in){if(!n)return;size_t length=strlen(in);if(length>=n)length=n-1;memcpy(out,in,length);out[length]=0;}
void append_text(char *out,size_t n,const char *in){size_t length=strlen(out);if(length<n)copy_text(out+length,n-length,in);}
int metadata_open(const char *path,int flags,unsigned mode){(void)path;(void)flags;(void)mode;offset=0;return 9;}
long metadata_read(int fd,void *out,size_t n){(void)fd;size_t left=strlen(saved)-offset;if(n>left)n=left;memcpy(out,saved+offset,n);offset+=n;return n;}
int metadata_close(int fd){(void)fd;return 0;}
int preferences_write(const char *path,const char *data){(void)path;if(fail_write)return -E_IO;copy_text(saved,sizeof(saved),data);return 0;}
int profiles_prepare(void){return 0;}
#define CHECK(x) do {if(!(x)){printf("FAIL metadata %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 copy_text(desk.config,sizeof(desk.config),"/home/user/.config/profile.ini");marks_load();
 FileMark *m=mark_for("/home/user/odd \nfolder/\xc3\xa9",true);CHECK(m);m->color=6;m->favorite=true;
 copy_text(m->tag,sizeof(m->tag),"project \t \n\xc3\xa9");CHECK(!marks_save());
 mark_file[0]=0;marks_load();CHECK(mark_count==1);
 m=mark_for("/home/user/odd \nfolder/\xc3\xa9",false);CHECK(m&&m->color==6&&m->favorite&&!strcmp(m->tag,"project \t \n\xc3\xa9"));
 marks_moved("/home/user/odd \nfolder","/home/user/Renamed");
 CHECK(mark_for("/home/user/Renamed/\xc3\xa9",false));
 CHECK(!mark_for("/home/user/odd \nfolder/\xc3\xa9",false));
 CHECK(mark_for("/home/user/Renamed sibling",true));marks_moved("/home/user/Renamed","/home/user/Moved");
 CHECK(mark_for("/home/user/Renamed sibling",false));
 mark_count=0;for(int i=0;i<MARK_LIMIT;i++){char path[32]="/x/";path[3]='a'+i/26;path[4]='a'+i%26;path[5]=0;m=mark_for(path,true);CHECK(m);m->color=1;}
 CHECK(!mark_for("/full",true));marks[50].color=0;CHECK(mark_for("/reused",true));
 CHECK(!mark_for("relative",true));
 const char *bad[]={"81 2f \n","10 zz \n","10 00 \n","10 2f 1\n","10 2f 00\n","10 2f ","10 2f\n"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){copy_text(saved,sizeof(saved),bad[i]);mark_file[0]=0;marks_load();CHECK(!mark_count);}
 puts("PASS production annotation save/reload, UTF-8 and delimiter round trips, subtree moves, prefix isolation, limits/reuse, malformed input (ASan/UBSan)");
 return 0;
}
