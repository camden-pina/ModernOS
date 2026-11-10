#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define OK(x) do {if (!(x)) {fprintf(stderr,"MMAP_HINT_FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);exit(1);}checks++;} while(0)
static void distinct(char *q,char *p,size_t n) {
 OK(q!=MAP_FAILED && !((uintptr_t)q&4095));
 OK((uintptr_t)q+n<=(uintptr_t)p || (uintptr_t)p+12288<=(uintptr_t)q);
}
int main(int argc,char **argv) {
 struct utsname os; if(uname(&os)||strcmp(os.sysname,"Coast"))return 2;
 int flags=MAP_PRIVATE|MAP_ANONYMOUS;char *p=mmap(NULL,12288,PROT_READ|PROT_WRITE,flags,-1,0);OK(p!=MAP_FAILED);
 p[0]=11;p[4096]=22;p[8192]=33;
 if(argc>1&&!strcmp(argv[1],"--baseline")) {
  errno=0;OK(mmap(p+4096,4096,PROT_READ|PROT_WRITE,flags,-1,0)==MAP_FAILED&&errno==ENOMEM);
  errno=0;OK(mmap(p+17,4096,PROT_READ|PROT_WRITE,flags,-1,0)==MAP_FAILED&&errno==ENOMEM);
  OK(p[0]==11&&p[4096]==22&&p[8192]==33);OK(munmap(p,12288)==0);
  printf("MMAP_HINT_BASELINE_PASS checks=%u occupied/unaligned non-fixed hints rejected\n",checks);return 0;
 }
 char *q=mmap(p+4096,4096,PROT_READ|PROT_WRITE,flags,-1,0);distinct(q,p,4096);q[0]=44;OK(p[0]==11&&p[4096]==22&&p[8192]==33);OK(munmap(q,4096)==0);
 q=mmap(p+123,4096,PROT_READ|PROT_WRITE,flags,-1,0);distinct(q,p,4096);OK(munmap(q,4096)==0);
 q=mmap(p-4096,8192,PROT_READ|PROT_WRITE,flags,-1,0);distinct(q,p,8192);OK(p[0]==11);OK(munmap(q,8192)==0);
 char *free_hint=mmap(NULL,8192,PROT_READ|PROT_WRITE,flags,-1,0);OK(free_hint!=MAP_FAILED);OK(munmap(free_hint,8192)==0);
 q=mmap(free_hint+123,8192,PROT_READ|PROT_WRITE,flags,-1,0);OK(q==free_hint);q[0]=55;OK(q[0]==55);OK(munmap(q,8192)==0);
 void *invalid[]={(void*)1,(void*)UINTPTR_MAX,(void*)0x7fffffe00000ULL};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
  q=mmap(invalid[i],4096,PROT_READ|PROT_WRITE,flags,-1,0);distinct(q,p,4096);q[0]=66;OK(q[0]==66);OK(munmap(q,4096)==0);
 }
 OK(mmap(p+4096,4096,PROT_READ|PROT_WRITE,flags|MAP_FIXED,-1,0)==p+4096);OK(p[4096]==0&&p[0]==11&&p[8192]==33);
 errno=0;OK(mmap(p+17,4096,PROT_READ|PROT_WRITE,flags|MAP_FIXED,-1,0)==MAP_FAILED&&errno==ENOMEM);
 errno=0;OK(mmap((void*)1,4096,PROT_READ|PROT_WRITE,flags|MAP_FIXED,-1,0)==MAP_FAILED&&errno==ENOMEM);
 errno=0;OK(mmap(NULL,512ULL*1024*1024+4096,PROT_NONE,flags,-1,0)==MAP_FAILED&&errno==EINVAL);
 errno=0;OK(mmap(p,4096,PROT_READ,flags|0x80000000u,-1,0)==MAP_FAILED&&errno==EINVAL);
 errno=0;OK(mmap(p,4096,PROT_READ|PROT_WRITE|PROT_EXEC,flags,-1,0)==MAP_FAILED&&errno==EINVAL);
 const unsigned char code[]={0xb8,42,0,0,0,0xc3};memcpy(p,code,sizeof(code));OK(mprotect(p,4096,PROT_READ|PROT_EXEC)==0);
 int(*fn)(void)=(int(*)(void))(uintptr_t)p;OK(fn()==42);
 errno=0;OK(mprotect(p,4096,PROT_READ|PROT_WRITE|PROT_EXEC)==-1&&errno==EINVAL);OK(fn()==42);
 OK(mprotect(p,4096,PROT_READ|PROT_WRITE)==0);p[0]=11;OK(p[0]==11&&p[8192]==33);OK(munmap(p,12288)==0);
 printf("MMAP_HINT_PASS checks=%u occupied/unaligned/invalid hint fallback, fixed semantics, cap and W^X guards\n",checks);return 0;
}
