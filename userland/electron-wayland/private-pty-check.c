#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <pty.h>
#include <utmp.h>
#include <termios.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { checks++;if(!(x)){fprintf(stderr,"PRIVATE_PTY_FAIL line=%u errno=%d\n",__LINE__,errno);exit(1);} } while(0)
static void readable(int fd) {struct pollfd p={fd,POLLIN,0};CHECK(poll(&p,1,5000)==1);CHECK(p.revents&POLLIN);}
static void read_exact(int fd,void *buffer,size_t size) {size_t at=0;while(at<size){readable(fd);ssize_t n=read(fd,(char*)buffer+at,size-at);CHECK(n>0);at+=n;}}
static void empty_session(void) {
    pid_t original_sid=getsid(0),original_group=getpgrp();
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(setsid()==getpid());CHECK(getsid(0)==getpid()&&getpgrp()==getpid());
        errno=0;CHECK(setsid()==-1&&errno==EPERM);
        errno=0;CHECK(open("/dev/tty",O_RDWR|O_NOCTTY)==-1&&errno==ENOENT);
        errno=0;CHECK(ioctl(0,TIOCSCTTY,0)==-1&&errno==EPERM);
        errno=0;CHECK(setpgid(0,original_group)==-1&&errno==EPERM);
        pid_t nested=fork();CHECK(nested>=0);
        if(!nested){CHECK(setsid()==getpid());CHECK(getsid(0)==getpid());_exit(0);}
        int status;CHECK(waitpid(nested,&status,0)==nested&&WIFEXITED(status)&&!WEXITSTATUS(status));_exit(0);
    }
    int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    CHECK(getsid(0)==original_sid&&getpgrp()==original_group);
    puts("PRIVATE_EMPTY_SESSION_PASS no_pty no_native_terminal no_group_rejoin nested_refund");
}
static void data_paths(void) {
    int m=posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC);CHECK(m>=0);CHECK(fcntl(m,F_GETFD)&FD_CLOEXEC);
    unsigned id;int lock;
    CHECK(ioctl(m,TIOCGPTN,&id)==0);CHECK(ioctl(m,TIOCGPTLCK,&lock)==0&&lock==1);
    char name[128];CHECK(ptsname_r(m,name,sizeof(name))==0);char expected[128];snprintf(expected,sizeof(expected),"/dev/pts/%u",id);CHECK(!strcmp(name,expected));
    errno=0;CHECK(open(name,O_RDWR|O_NOCTTY)==-1&&errno==EIO);
    errno=0;CHECK(ioctl(m,TIOCGPTPEER,O_RDWR|O_NOCTTY)==-1&&errno==EIO);
    errno=0;CHECK(ioctl(m,TIOCGPTN,(void*)1)==-1&&errno==EFAULT);
    errno=0;CHECK(ioctl(m,TIOCSPTLCK,(void*)1)==-1&&errno==EFAULT);
    CHECK(ioctl(m,TIOCGPTLCK,&lock)==0&&lock==1);
    char c='x';errno=0;CHECK(ioctl(m,TIOCSTI,&c)==-1&&errno==EPERM);
    errno=0;CHECK(ioctl(1,TCGETS,&(struct termios){0})==-1&&errno==EPERM);
    CHECK(grantpt(m)==0);CHECK(unlockpt(m)==0);CHECK(ioctl(m,TIOCGPTLCK,&lock)==0&&!lock);
    int s=ioctl(m,TIOCGPTPEER,O_RDWR|O_NOCTTY|O_CLOEXEC|O_NONBLOCK);CHECK(s>=0);CHECK(fcntl(s,F_GETFD)&FD_CLOEXEC);CHECK(fcntl(s,F_GETFL)&O_NONBLOCK);
    struct termios mode;CHECK(tcgetattr(s,&mode)==0);cfmakeraw(&mode);CHECK(tcsetattr(s,TCSANOW,&mode)==0);
    struct winsize size={.ws_row=33,.ws_col=111},actual;
    CHECK(ioctl(m,TIOCSWINSZ,&size)==0);CHECK(ioctl(s,TIOCGWINSZ,&actual)==0);CHECK(!memcmp(&size,&actual,sizeof(size)));
    size.ws_row=0;errno=0;CHECK(ioctl(m,TIOCSWINSZ,&size)==-1&&errno==EINVAL);
    CHECK(ioctl(s,TIOCGWINSZ,&actual)==0&&actual.ws_row==33);
    int poller=epoll_create1(EPOLL_CLOEXEC);CHECK(poller>=0);struct epoll_event event={.events=EPOLLIN,.data.u64=123};CHECK(epoll_ctl(poller,EPOLL_CTL_ADD,s,&event)==0);
    unsigned char input[16384],output[16384];for(unsigned i=0;i<sizeof(input);i++)input[i]=(i*37)&255;
    CHECK(write(m,input,sizeof(input))==sizeof(input));CHECK(epoll_wait(poller,&event,1,1000)==1&&event.data.u64==123);
    read_exact(s,output,sizeof(output));CHECK(!memcmp(input,output,sizeof(input)));
    CHECK(epoll_wait(poller,&event,1,0)==0);errno=0;CHECK(read(s,output,1)==-1&&errno==EAGAIN);
    CHECK(write(s,input,sizeof(input))==sizeof(input));read_exact(m,output,sizeof(output));CHECK(!memcmp(input,output,sizeof(input)));
    int reopened=open(name,O_RDWR|O_NOCTTY|O_NONBLOCK);CHECK(reopened>=0);int alias=dup(s);CHECK(alias>=0);close(s);
    CHECK(write(m,"retain",6)==6);readable(alias);CHECK(read(alias,output,6)==6&&!memcmp(output,"retain",6));
    close(alias);CHECK(write(m,"reopen",6)==6);readable(reopened);CHECK(read(reopened,output,6)==6&&!memcmp(output,"reopen",6));
    close(poller);close(m);CHECK(read(reopened,output,1)==0);close(reopened);
    errno=0;CHECK(open(name,O_RDWR|O_NOCTTY)==-1&&errno==ENOENT);
}
static void fork_shell(void) {
    int m,s;char name[128];struct winsize size={.ws_row=29,.ws_col=103};CHECK(openpty(&m,&s,name,NULL,&size)==0);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        close(m);
        if(login_tty(s)) {int error=errno;dprintf(s,"PRIVATE_LOGIN_FAIL errno=%d pid=%d pgid=%d sid=%d\n",error,getpid(),getpgrp(),getsid(0));_exit(112);}
        CHECK(getsid(0)==getpid());CHECK(tcgetsid(0)==getpid());CHECK(tcgetpgrp(0)==getpgrp());
        int ctty=open("/dev/tty",O_RDWR|O_NOCTTY);CHECK(ctty>=0);close(ctty);
        execl("/usr/bin/linux-bash","bash","--noprofile","--norc","-i",NULL);
        dprintf(2,"PRIVATE_SHELL_EXEC_FAIL errno=%d\n",errno);_exit(111);
    }
    close(s);
    const char *command="printf 'PRIVATE_SHELL_IO_PASS\\n'; exit\n";CHECK(write(m,command,strlen(command))==(ssize_t)strlen(command));
    char output[8192]={0};size_t count=0;
    while(count<sizeof(output)-1) {struct pollfd p={m,POLLIN,0};CHECK(poll(&p,1,5000)==1);ssize_t n=read(m,output+count,sizeof(output)-1-count);if(n<=0)break;count+=n;}
    printf("PRIVATE_PTY_SHELL_TRANSCRIPT %s\n",output);fflush(stdout);
    CHECK(strstr(output,"PRIVATE_SHELL_IO_PASS\r\n")!=NULL);
    int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);close(m);
}
int main(int argc,char **argv) {
    struct utsname os;if(uname(&os)||strcmp(os.sysname,"Coast"))return 77;
    if(argc==2&&!strcmp(argv[1],"--denied")) {errno=0;CHECK(posix_openpt(O_RDWR|O_NOCTTY)==-1);errno=0;CHECK(setsid()==-1&&errno==EPERM);puts("PRIVATE_PTY_AMBIENT_DENIED");return 0;}
    if(argc==2&&!strcmp(argv[1],"--winsize")) {struct winsize size;CHECK(getuid()==1000);CHECK(ioctl(0,TIOCGWINSZ,&size)==0);printf("%u %u\n",size.ws_row,size.ws_col);return 0;}
    if(argc==2&&!strcmp(argv[1],"--detached-session")) {CHECK(getuid()==1000);CHECK(getsid(0)==getpid()&&getpgrp()==getpid());errno=0;CHECK(open("/dev/tty",O_RDWR|O_NOCTTY)==-1&&errno==ENOENT);puts("PRIVATE_DETACHED_SESSION_PASS");return 0;}
    CHECK(getuid()==1000&&getgid()==1000);errno=0;CHECK(open("/dev/ptmx",O_PATH)==-1&&errno==EINVAL);
    errno=0;CHECK(open("/dev/ptmx",O_RDWR|O_CREAT,0600)==-1&&errno==EINVAL);
    empty_session();data_paths();fork_shell();
    for(unsigned round=0;round<3;round++) {int masters[16];for(unsigned i=0;i<16;i++){masters[i]=posix_openpt(O_RDWR|O_NOCTTY);CHECK(masters[i]>=0);}errno=0;CHECK(posix_openpt(O_RDWR|O_NOCTTY)==-1&&errno==ENOSPC);for(unsigned i=0;i<16;i++)CHECK(close(masters[i])==0);}
    printf("PRIVATE_PTY_PASS checks=%u private_paths lock peer_fd raw_io epoll exact_bytes dup reopen session controlling_terminal Bash winsize pool16 refund3 cleanup\n",checks);return 0;
}
