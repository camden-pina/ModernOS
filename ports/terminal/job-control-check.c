#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <termios.h>
static pid_t owned[16];static unsigned checks;static volatile sig_atomic_t child_signals;static int beat_fd;
static unsigned long long now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))_exit(90);return (unsigned long long)t.tv_sec*1000000+t.tv_nsec/1000;}
static void nap(unsigned ms){unsigned long long end=now()+ms*1000ULL;while(now()<end){struct timespec t={0,1000000};nanosleep(&t,NULL);}}
static void cleanup(void){for(unsigned i=0;i<16;i++)if(owned[i]>1){kill(owned[i],SIGKILL);while(waitpid(owned[i],NULL,0)<0&&errno==EINTR){}owned[i]=0;}}
static void fail(int line,const char *expr){fprintf(stderr,"JOB_CHECK_FAIL line=%d errno=%d %s\n",line,errno,expr);cleanup();exit(1);}
#define OK(x) do{if(!(x))fail(__LINE__,#x);checks++;}while(0)
static void remember(pid_t p){for(unsigned i=0;i<16;i++)if(!owned[i]){owned[i]=p;return;}fail(__LINE__,"owned child capacity");}
static void forgotten(pid_t p){for(unsigned i=0;i<16;i++)if(owned[i]==p)owned[i]=0;}
static void chld(int sig){(void)sig;child_signals++;}
static void caught(int sig){(void)sig;char c='H';(void)write(beat_fd,&c,1);}
static _Thread_local unsigned tls_value;
static void *ticks(void *arg){unsigned tag=(unsigned)(unsigned long)arg;tls_value=tag;volatile double value=tag+0.5;for(;;){value=value*2;sched_yield();value=value/2;if(value!=tag+0.5||tls_value!=tag){(void)write(beat_fd,"!",1);_exit(77);}char c='A'+tag%20;if(write(beat_fd,&c,1)<0&&errno!=EINTR)_exit(78);struct timespec t={0,5000000};nanosleep(&t,NULL);}return NULL;}
struct actor{pid_t pid;int beat,control;};
static struct actor actor(int mode){int beat[2],control[2];OK(!pipe(beat)&&!pipe(control));OK(fcntl(beat[0],F_SETFL,O_NONBLOCK)==0);pid_t pid=fork();OK(pid>=0);if(!pid){close(beat[0]);close(control[1]);beat_fd=beat[1];if(fcntl(control[0],F_SETFL,O_NONBLOCK)<0)_exit(71);
 if(mode==1)signal(SIGCONT,SIG_IGN);
 if(mode==2){sigset_t set;sigemptyset(&set);sigaddset(&set,SIGCONT);sigprocmask(SIG_BLOCK,&set,NULL);}
 if(mode==3){sigset_t set;sigemptyset(&set);sigaddset(&set,SIGSTOP);sigaddset(&set,SIGTSTP);sigprocmask(SIG_BLOCK,&set,NULL);}
 if(mode==4){struct sigaction a={0};a.sa_handler=caught;sigemptyset(&a.sa_mask);if(sigaction(SIGTSTP,&a,NULL))_exit(72);}
 if(mode==5)signal(SIGTSTP,SIG_IGN);
 if(mode==6||mode==7){pthread_t a,b;if(pthread_create(&a,NULL,ticks,(void*)2UL)||pthread_create(&b,NULL,ticks,(void*)3UL))_exit(73);if(mode==7){sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGTSTP);sigprocmask(SIG_BLOCK,&mask,NULL);}ticks((void*)1UL);}
 for(;;){char cmd;if(read(control[0],&cmd,1)==1&&cmd=='U'){sigset_t set;sigemptyset(&set);sigaddset(&set,SIGTSTP);sigprocmask(SIG_UNBLOCK,&set,NULL);}if(write(beat_fd,"T",1)<0&&errno!=EINTR)_exit(74);struct timespec t={0,5000000};nanosleep(&t,NULL);}_exit(0);}
 close(beat[1]);close(control[0]);remember(pid);return(struct actor){pid,beat[0],control[1]};}
static int event(pid_t pid,int options){unsigned long long until=now()+3000000;int status=0;for(;;){pid_t r=waitpid(pid,&status,options|WNOHANG);if(r==pid)return status;if(r<0&&errno!=EINTR)fail(__LINE__,"wait event");if(now()>until)fail(__LINE__,"wait event timeout");nap(1);}}
static void heartbeat(struct actor a){char bytes[128];unsigned long long until=now()+3000000;for(;;){ssize_t n=read(a.beat,bytes,sizeof(bytes));if(n>0){for(ssize_t i=0;i<n;i++)OK(bytes[i]!='!');return;}if(!n)fail(__LINE__,"child exited before heartbeat");if(errno!=EAGAIN&&errno!=EINTR)fail(__LINE__,"heartbeat read");if(now()>until)fail(__LINE__,"heartbeat timeout");nap(1);}}
static void stopped(struct actor a,int sig){int status=event(a.pid,WUNTRACED);OK(WIFSTOPPED(status)&&WSTOPSIG(status)==sig);char bytes[256];while(read(a.beat,bytes,sizeof(bytes))>0){}nap(70);OK(read(a.beat,bytes,sizeof(bytes))<0&&errno==EAGAIN);OK(waitpid(a.pid,&status,WNOHANG|WUNTRACED)==0);OK(kill(a.pid,0)==0);}
static void finish(struct actor a){OK(kill(a.pid,SIGKILL)==0);int status=event(a.pid,0);OK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);forgotten(a.pid);close(a.beat);close(a.control);}
static int tty_check(const char *option) {
 struct termios mode;OK(tcgetattr(0,&mode)==0);
 if(!strcmp(option,"--tty-info")){printf("JOB_TTY pid=%d group=%d sid=%d foreground=%d vsusp=%u isig=%u canonical=%u\n",getpid(),getpgrp(),getsid(0),tcgetpgrp(0),mode.c_cc[VSUSP],!!(mode.c_lflag&ISIG),!!(mode.c_lflag&ICANON));return 0;}
 if(!strcmp(option,"--background-mode")){int e;do{e=tcsetattr(0,TCSANOW,&mode);}while(e<0&&errno==EINTR);OK(e==0);puts("R9_MODE_FOREGROUND_OK");return 0;}
 if(!strcmp(option,"--deny-mode")){signal(SIGTTOU,SIG_IGN);errno=0;OK(tcsetattr(0,TCSANOW,&mode)<0&&(errno==EIO||errno==EPERM));puts("R9_BACKGROUND_MODE_DENIED");return 0;}
 if(!strcmp(option,"--deny-read")){signal(SIGTTIN,SIG_IGN);char c;errno=0;OK(read(0,&c,1)<0&&(errno==EIO||errno==EPERM));puts("R9_BACKGROUND_READ_DENIED");return 0;}
 if(!strcmp(option,"--tostop")){
  struct termios original=mode;mode.c_lflag|=TOSTOP;OK(tcsetattr(0,TCSANOW,&mode)==0);
  int gate[2];OK(pipe(gate)==0);pid_t child=fork();OK(child>=0);
  if(!child){close(gate[1]);char c;if(read(gate[0],&c,1)!=1)_exit(75);close(gate[0]);const char *text="R9_TOSTOP_CHILD_OUTPUT\n";if(write(1,text,strlen(text))!=(ssize_t)strlen(text))_exit(76);_exit(0);}
  close(gate[0]);remember(child);OK(setpgid(child,child)==0);OK(write(gate[1],"x",1)==1);close(gate[1]);int status=event(child,WUNTRACED);OK(WIFSTOPPED(status)&&WSTOPSIG(status)==SIGTTOU);OK(tcsetpgrp(0,child)==0);OK(kill(child,SIGCONT)==0);status=event(child,0);OK(WIFEXITED(status)&&WEXITSTATUS(status)==0);forgotten(child);OK(tcsetpgrp(0,getpgrp())==0);OK(tcsetattr(0,TCSANOW,&original)==0);puts("R9_TOSTOP_RESTORED");return 0;
 }
 return 2;
}
static void timing_stops(void) {
 for(int which=0;which<3;which++) {
  pid_t pid=fork();OK(pid>=0);
  if(!pid) {
   struct timespec delay={0,500000000};unsigned long long start=now();int e;
   if(which==2){clock_gettime(CLOCK_MONOTONIC,&delay);delay.tv_nsec+=500000000;if(delay.tv_nsec>=1000000000){delay.tv_sec++;delay.tv_nsec-=1000000000;}}
   if(which)e=clock_nanosleep(CLOCK_MONOTONIC,which==2?TIMER_ABSTIME:0,&delay,NULL);
   else e=nanosleep(&delay,NULL);
   unsigned long long elapsed=now()-start;
   if(e||elapsed<490000||elapsed>1800000){fprintf(stderr,"JOB_SLEEP_FAIL mode=%d e=%d errno=%d us=%llu\n",which,e,errno,elapsed);_exit(79);}
   _exit(0);
  }
  remember(pid);nap(40);OK(kill(pid,SIGTSTP)==0);int status=event(pid,WUNTRACED);OK(WIFSTOPPED(status)&&WSTOPSIG(status)==SIGTSTP);nap(70);OK(kill(pid,SIGCONT)==0);OK(WIFCONTINUED(event(pid,WCONTINUED)));nap(40);OK(kill(pid,SIGSTOP)==0);status=event(pid,WUNTRACED);OK(WIFSTOPPED(status)&&WSTOPSIG(status)==SIGSTOP);nap(70);OK(kill(pid,SIGCONT)==0);OK(WIFCONTINUED(event(pid,WCONTINUED)));status=event(pid,0);OK(WIFEXITED(status)&&WEXITSTATUS(status)==0);forgotten(pid);
 }
 puts("JOB_CASE nanosleep and relative/absolute clock sleep retain deadlines across repeated stops passed");
}
int main(int argc,char **argv){struct utsname os;if(uname(&os)||strcmp(os.sysname,"Coast")){fputs("Coast guest only\n",stderr);return 2;}atexit(cleanup);setvbuf(stdout,NULL,_IONBF,0);
 if(argc>1&&strcmp(argv[1],"--baseline"))return tty_check(argv[1]);
 struct actor a=actor(0);heartbeat(a);
 if(argc>1&&!strcmp(argv[1],"--baseline")){errno=0;OK(kill(a.pid,SIGSTOP)<0&&errno==ENOTSUP);errno=0;OK(kill(a.pid,SIGTSTP)<0&&errno==ENOTSUP);int status;OK(waitpid(a.pid,&status,WNOHANG|WUNTRACED|WCONTINUED)==0);finish(a);puts("JOB_CONTROL_BASELINE_PASS stop signals return ENOTSUP; no stopped wait event");return 0;}
 struct sigaction action={0};action.sa_handler=chld;sigemptyset(&action.sa_mask);OK(sigaction(SIGCHLD,&action,NULL)==0);
 OK(kill(a.pid,SIGSTOP)==0);stopped(a,SIGSTOP);OK(kill(a.pid,SIGCONT)==0);OK(WIFCONTINUED(event(a.pid,WCONTINUED)));heartbeat(a);OK(kill(a.pid,SIGTSTP)==0);stopped(a,SIGTSTP);finish(a);puts("JOB_CASE stop/continue/wait consumption/kill-stopped passed");
 for(int mode=1;mode<=2;mode++){a=actor(mode);heartbeat(a);OK(kill(a.pid,SIGSTOP)==0);stopped(a,SIGSTOP);OK(kill(a.pid,SIGCONT)==0);OK(WIFCONTINUED(event(a.pid,WCONTINUED)));heartbeat(a);finish(a);}puts("JOB_CASE ignored and blocked SIGCONT still resume passed");
 a=actor(3);heartbeat(a);OK(kill(a.pid,SIGTSTP)==0);nap(50);int status;OK(waitpid(a.pid,&status,WNOHANG|WUNTRACED)==0);heartbeat(a);OK(kill(a.pid,SIGSTOP)==0);stopped(a,SIGSTOP);OK(kill(a.pid,SIGCONT)==0);OK(WIFCONTINUED(event(a.pid,WCONTINUED)));OK(write(a.control,"U",1)==1);nap(70);OK(waitpid(a.pid,&status,WNOHANG|WUNTRACED)==0);heartbeat(a);finish(a);puts("JOB_CASE SIGSTOP cannot be masked; CONT discards masked stop passed");
 for(int mode=4;mode<=5;mode++){a=actor(mode);heartbeat(a);OK(kill(a.pid,SIGTSTP)==0);nap(60);OK(waitpid(a.pid,&status,WNOHANG|WUNTRACED)==0);heartbeat(a);finish(a);}puts("JOB_CASE caught/ignored TSTP not converted into stop passed");
 action.sa_flags=SA_NOCLDSTOP;OK(sigaction(SIGCHLD,&action,NULL)==0);a=actor(0);heartbeat(a);child_signals=0;OK(kill(a.pid,SIGSTOP)==0);stopped(a,SIGSTOP);OK(kill(a.pid,SIGCONT)==0);OK(WIFCONTINUED(event(a.pid,WCONTINUED)));nap(30);OK(child_signals==0);finish(a);nap(30);OK(child_signals>0);action.sa_flags=0;OK(sigaction(SIGCHLD,&action,NULL)==0);puts("JOB_CASE SA_NOCLDSTOP suppresses notifications but not wait events passed");
 for(int mode=6;mode<=7;mode++){a=actor(mode);heartbeat(a);for(int i=0;i<12;i++){int sig=mode==7?SIGTSTP:SIGSTOP;OK(kill(a.pid,sig)==0);stopped(a,sig);OK(kill(a.pid,SIGCONT)==0);OK(WIFCONTINUED(event(a.pid,WCONTINUED)));heartbeat(a);}finish(a);}puts("JOB_CASE threaded group safe stop rendezvous, unblocked-thread routing, TLS/FPU and repeated resume passed");
 timing_stops();
 errno=0;OK(kill(1,SIGSTOP)<0&&errno==EPERM);
 printf("JOB_CONTROL_PRIMITIVES_PASS checks=%u\n",checks);return 0;}
