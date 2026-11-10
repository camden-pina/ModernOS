#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <termios.h>
#include <sys/wait.h>
#include <signal.h>
static long raw(long n){long r;__asm__ volatile("syscall":"=a"(r):"a"(n):"rcx","r11","memory");return r;}
static void report(const char *name){
 printf("TTY_PROBE %s pid=%d ppid=%d pgid=%d sid=%d uid_raw=%ld euid_raw=%ld uid_libc=%u euid_libc=%u\n",name,getpid(),getppid(),getpgrp(),getsid(0),raw(SYS_getuid),raw(SYS_geteuid),getuid(),geteuid());
 for(int fd=0;fd<3;fd++) {struct termios mode={0};pid_t fg=-1;errno=0;int e=tcgetattr(fd,&mode),why=errno;errno=0;int g=ioctl(fd,TIOCGPGRP,&fg);printf("TTY_PROBE fd=%d termios=%d errno=%d lflag=%lx fg_ioctl=%d errno=%d foreground=%d\n",fd,e,why,(unsigned long)mode.c_lflag,g,errno,fg);}
 fflush(stdout);
}
int main(int argc,char **argv){
 report("before");
 if(argc>1 && !strcmp(argv[1],"--deny-foreground")) {
  pid_t before=tcgetpgrp(0),own=getpgrp();errno=0;int result=tcsetpgrp(0,own),e=errno;
  printf("GUARD_PROBE own=%d before=%d after=%d result=%d errno=%d\n",own,before,tcgetpgrp(0),result,e);
  if(result!=-1 || e!=EPERM || tcgetpgrp(0)!=before)return 30;
  puts("GUARD_REJECTED_FOREGROUND_TAKEOVER");return 0;
 }
 if(argc>1 && !strcmp(argv[1],"--groups")) {
  int self=setpgid(0,0);printf("GROUP_PROBE self_setpgid=%d errno=%d\n",self,errno);fflush(stdout);
  pid_t child=fork();if(child<0){perror("fork");return 22;}
  if(!child){setpgid(0,0);sleep(5);_exit(0);}
  int result=setpgid(child,child),e=errno;printf("GROUP_PROBE parent_setpgid=%d errno=%d child=%d\n",result,e,child);
  errno=0;result=tcsetpgrp(0,child);e=errno;printf("GROUP_PROBE transfer=%d errno=%d observed=%d\n",result,e,tcgetpgrp(0));
  errno=0;result=tcsetpgrp(0,getpgrp());e=errno;printf("GROUP_PROBE restore=%d errno=%d observed=%d\n",result,e,tcgetpgrp(0));fflush(stdout);
  kill(child,SIGTERM);waitpid(child,NULL,0);return 0;
 }
 if(argc>2 && !strcmp(argv[1],"--session")) {
  errno=0;int result=setpgid(0,0);printf("TTY_PROBE self_setpgid result=%d errno=%d\n",result,errno);fflush(stdout);
  if(result)return 20;
  report("after_group");execvp(argv[2],argv+2);perror("TTY_PROBE exec");return 21;
 }
 return 0;
}
