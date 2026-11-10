#ifndef COAST_ACCOUNT_SECRET_H
#define COAST_ACCOUNT_SECRET_H
#include <libc.h>
static struct os_termios account_saved;
static volatile int account_restore;
static void account_erase(void *p,size_t n) {volatile unsigned char *b=p;while(n--)*b++=0;}
static void account_interrupted(int sig) {if(account_restore)ioctl(0,TCSETS,&account_saved);exit(128+sig);}
__attribute__((naked)) static void account_sigreturn(void) {__asm__ volatile("mov $15,%rax\nsyscall\nud2");}
static int account_secret(char out[128],const char *prompt) {
 if(ioctl(0,TCGETS,&account_saved))return -E_NOTTY;
 struct os_sigaction sa={.handler=(uintptr_t)account_interrupted,.flags=SA_RESTORER,.restorer=(uintptr_t)account_sigreturn};
 for(unsigned sig=1;sig<=15;sig++)if(sig==1 || sig==2 || sig==15)syscall6(SYS_rt_sigaction,sig,(long)&sa,0,8,0,0);
 struct os_termios mode=account_saved;mode.lflag&=~ECHO;
 int e=ioctl(0,TCSETS,&mode);if(e)return e;account_restore=1;
 write_all(2,prompt,strlen(prompt));unsigned used=0;
 for(;;) {char c;long n=read(0,&c,1);if(n<=0){e=n<0?n:-E_IO;break;}if(c=='\n' || c=='\r')break;if(used>=127){e=-E_2BIG;continue;}out[used++]=c;}
 out[used]=0;ioctl(0,TCSETS,&account_saved);account_restore=0;write_all(2,"\n",1);return e;
}
#endif
