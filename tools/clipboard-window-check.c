/* Guest-only native-window clipboard authority regression. */
#include <uapi/window.h>
#include <uapi/clipboard.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
static int logfd;
static void note(const char *s){write(logfd,s,strlen(s));}
static int denied(void){struct os_clipboard_info i;errno=0;return syscall(COAST_SYSCALL_CLIPBOARD,OS_CLIPBOARD_INFO,&i,sizeof(i),0)==-1 && errno==EPERM;}
int main(int argc,char **argv){
 if(argc!=2)return 2;
 if(!strcmp(argv[1],"--terminal-info")){
  printf("CLIP_JOB pid=%d parent=%d group=%d uid=%d\n",getpid(),getppid(),getpgrp(),getuid());
  for(int fd=0;fd<128;fd++){struct termios t;int fg=-1;if(!tcgetattr(fd,&t)){ioctl(fd,TIOCGPGRP,&fg);printf("CLIP_JOB ttyfd=%d foreground=%d\n",fd,fg);}}
  struct os_clipboard_info i;errno=0;long r=syscall(COAST_SYSCALL_CLIPBOARD,OS_CLIPBOARD_INFO,&i,sizeof(i),0);printf("CLIP_JOB info=%ld errno=%d\n",r,errno);return 0;
 }
 for(int fd=0;fd<128;fd++)close(fd);
 logfd=open(argv[1],O_WRONLY|O_CREAT|O_TRUNC,0600);if(logfd<0)return 3;
 if(!denied()){note("FAIL ambient process group authorized clipboard\n");return 4;}
 note("CLIP_WINDOW_INITIAL_DENIED\n");
 struct os_window_info info={.version=OS_WINDOW_PIXEL_CLIENT_VERSION,.width=320,.height=200};
 strcpy(info.title,"Clipboard Authority Probe");
 if(syscall(OS_WINDOW_SYSCALL,OS_WINDOW_CREATE,&info,sizeof(info))){note("FAIL create\n");return 5;}
 static unsigned pixels[320*200];for(unsigned n=0;n<320*200;n++)pixels[n]=0xffdae9f7;
 struct os_window_frame frame={.id=info.id,.width=320,.height=200,.stride=320,.pixels=(uintptr_t)pixels};
 if(syscall(OS_WINDOW_SYSCALL,OS_WINDOW_PRESENT,&frame,sizeof(frame))){note("FAIL present\n");return 6;}
 int focused=0,passed=0;
 for(unsigned tick=0;tick<1500 && !passed;tick++){
  struct os_window_event events[32];struct os_window_events request={info.id,32,(uintptr_t)events};
  long count=syscall(OS_WINDOW_SYSCALL,OS_WINDOW_INPUT,&request,sizeof(request));
  if(count<0 && errno!=EAGAIN)break;
  for(long n=0;n<count;n++)if(events[n].type==OS_WINDOW_FOCUS){
   if(events[n].code){struct os_clipboard_info i;if(syscall(COAST_SYSCALL_CLIPBOARD,OS_CLIPBOARD_INFO,&i,sizeof(i),0)){note("FAIL focused read\n");return 7;}focused=1;note("CLIP_WINDOW_FOCUSED_ALLOWED\n");}
   else if(focused){if(!denied()){note("FAIL unfocused read allowed\n");return 8;}note("CLIP_WINDOW_UNFOCUSED_DENIED\nCLIP_WINDOW_CHECK_PASS\n");passed=1;}
  }
  usleep(10000);
 }
 struct os_window_token token={info.id,0};syscall(OS_WINDOW_SYSCALL,OS_WINDOW_CLIENT_CLOSE,&token,sizeof(token));
 if(!passed)note("FAIL focus handoff timed out\n");close(logfd);return passed?0:9;
}
