#define _GNU_SOURCE
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"INOTIFY_FAIL line=%u errno=%d\n",__LINE__,errno);exit(1);}}while(0)
static unsigned char bytes[16384];
static ssize_t collect(int fd) {ssize_t n=read(fd,bytes,sizeof(bytes));CHECK(n>=0);return n;}
static int has(ssize_t size,int wd,unsigned mask,const char *name) {
    for(ssize_t at=0;at<size;) {
        struct inotify_event *e=(void *)(bytes+at);CHECK(size-at>=16);CHECK(e->len<=256&&at+16+e->len<=size);
        if(e->wd==wd && (e->mask&mask)==mask && (!name || (e->len && !strcmp(e->name,name))))return 1;
        at+=16+e->len;
    }
    return 0;
}
static void create(const char *path) {int f=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(f>=0);CHECK(close(f)==0);}
int main(void) {
    struct utsname os;if(uname(&os)||strcmp(os.sysname,"Coast"))return 77;
    CHECK(getuid()==1000);const char *dir="/home/port-test/runtime/watch-check";
    CHECK(mkdir(dir,0700)==0);CHECK(chdir(dir)==0);
    int fd=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);CHECK(fd>=0);
    CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC);CHECK(fcntl(fd,F_GETFL)&O_NONBLOCK);
    errno=0;CHECK(inotify_init1(8)==-1&&errno==EINVAL);
    errno=0;CHECK(inotify_add_watch(fd,"/tmp/inotify-root-private",IN_CREATE)==-1&&(errno==EACCES||errno==EPERM));
    CHECK(symlink("/tmp/inotify-root-private","private-link")==0);
    errno=0;CHECK(inotify_add_watch(fd,"private-link",IN_CREATE)==-1&&(errno==EACCES||errno==EPERM));
    CHECK(unlink("private-link")==0);
    errno=0;CHECK(inotify_add_watch(fd,(void *)1,IN_CREATE)==-1&&errno==EFAULT);
    errno=0;CHECK(inotify_add_watch(fd,".",0)==-1&&errno==EINVAL);
    int wd=inotify_add_watch(fd,".",IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO);CHECK(wd>0);
    CHECK(inotify_add_watch(fd,".",IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO)==wd);
    errno=0;CHECK(inotify_add_watch(fd,".",IN_CREATE|IN_MASK_CREATE)==-1&&errno==EEXIST);
    int ep=epoll_create1(EPOLL_CLOEXEC);CHECK(ep>=0);struct epoll_event ev={.events=EPOLLIN,.data.u64=123};
    CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&ev)==0);CHECK(epoll_wait(ep,&ev,1,0)==0);
    create("a");CHECK(epoll_wait(ep,&ev,1,0)==1&&ev.data.u64==123);
    errno=0;CHECK(read(fd,bytes,1)==-1&&errno==EINVAL);
    errno=0;CHECK(syscall(SYS_read,fd,(void *)1,sizeof(bytes))==-1&&errno==EFAULT);
    CHECK(has(collect(fd),wd,IN_CREATE,"a"));CHECK(epoll_wait(ep,&ev,1,0)==0);
    int file_wd=inotify_add_watch(fd,"a",IN_MODIFY|IN_ATTRIB|IN_MOVE_SELF|IN_DELETE_SELF);CHECK(file_wd>0);
    errno=0;CHECK(inotify_add_watch(fd,"a",IN_CREATE|IN_ONLYDIR)==-1&&errno==ENOTDIR);
    int f=open("a",O_RDWR);CHECK(f>=0);CHECK(write(f,"actual watch bytes",18)==18);
    CHECK(has(collect(fd),file_wd,IN_MODIFY,NULL));CHECK(ftruncate(f,0)==0);
    CHECK(has(collect(fd),file_wd,IN_MODIFY,NULL));CHECK(fchmod(f,0640)==0);
    CHECK(has(collect(fd),file_wd,IN_ATTRIB,NULL));CHECK(close(f)==0);
    f=open("a",O_WRONLY);CHECK(f>=0);CHECK(write(f,"Z",1)==1);CHECK(close(f)==0);collect(fd);
    ssize_t n;
    CHECK(inotify_add_watch(fd,"a",IN_ACCESS|IN_OPEN|IN_CLOSE_NOWRITE)==file_wd);
    f=open("a",O_RDONLY);CHECK(f>=0);char sample;CHECK(read(f,&sample,1)==1&&sample=='Z');CHECK(close(f)==0);
    n=collect(fd);CHECK(has(n,file_wd,IN_ACCESS,NULL));CHECK(has(n,file_wd,IN_OPEN,NULL));CHECK(has(n,file_wd,IN_CLOSE_NOWRITE,NULL));
    CHECK(inotify_add_watch(fd,"a",IN_MODIFY)==file_wd);
    f=open("a",O_RDONLY);CHECK(f>=0);CHECK(read(f,&sample,1)==1);CHECK(close(f)==0);
    errno=0;CHECK(read(fd,bytes,sizeof(bytes))==-1&&errno==EAGAIN);
    CHECK(inotify_add_watch(fd,"a",IN_ATTRIB|IN_MOVE_SELF|IN_DELETE_SELF|IN_MASK_ADD)==file_wd);
    CHECK(rename("a","b")==0);n=collect(fd);
    CHECK(has(n,wd,IN_MOVED_FROM,"a"));CHECK(has(n,wd,IN_MOVED_TO,"b"));CHECK(has(n,file_wd,IN_MOVE_SELF,NULL));
    unsigned from=0,to=0;for(ssize_t at=0;at<n;) {struct inotify_event *e=(void *)(bytes+at);if(e->mask&IN_MOVED_FROM)from=e->cookie;if(e->mask&IN_MOVED_TO)to=e->cookie;at+=16+e->len;}
    CHECK(from&&from==to);CHECK(unlink("b")==0);n=collect(fd);
    CHECK(has(n,wd,IN_DELETE,"b"));CHECK(has(n,file_wd,IN_DELETE_SELF,NULL));CHECK(has(n,file_wd,IN_IGNORED,NULL));
    errno=0;CHECK(inotify_rm_watch(fd,file_wd)==-1&&errno==EINVAL);
    CHECK(inotify_rm_watch(fd,wd)==0);CHECK(has(collect(fd),wd,IN_IGNORED,NULL));
    wd=inotify_add_watch(fd,".",IN_CREATE|IN_ONESHOT);CHECK(wd>file_wd);
    create("once");n=collect(fd);CHECK(has(n,wd,IN_CREATE,"once"));CHECK(has(n,wd,IN_IGNORED,NULL));
    CHECK(unlink("once")==0);errno=0;CHECK(read(fd,bytes,sizeof(bytes))==-1&&errno==EAGAIN);
    wd=inotify_add_watch(fd,".",IN_CREATE|IN_DELETE);CHECK(wd>0);
    int alias=dup(fd);CHECK(alias>=0);CHECK(close(fd)==0);fd=alias;
    for(unsigned i=0;i<600;i++){char name[32];snprintf(name,sizeof(name),"overflow%u",i);create(name);CHECK(unlink(name)==0);}
    n=collect(fd);CHECK(has(n,-1,IN_Q_OVERFLOW,NULL));
    create("after-overflow");CHECK(has(collect(fd),wd,IN_CREATE,"after-overflow"));CHECK(unlink("after-overflow")==0);collect(fd);
    CHECK(inotify_rm_watch(fd,wd)==0);collect(fd);
    errno=0;CHECK(lseek(fd,0,SEEK_SET)==-1&&errno==ESPIPE);
    errno=0;CHECK(write(fd,"x",1)==-1&&errno==EBADF);
    CHECK(close(ep)==0);CHECK(close(fd)==0);
    unsigned baseline=0;
    for(unsigned cycle=0;cycle<3;cycle++) {
        int fds[17];unsigned count=0;
        while(count<17 && (fds[count]=inotify_init1(IN_NONBLOCK))>=0)count++;
        CHECK(count>0&&count<=16&&errno==ENOSPC);if(!cycle)baseline=count;else CHECK(count==baseline);
        for(unsigned i=0;i<count;i++)CHECK(close(fds[i])==0);
    }
    CHECK(chdir("/home/port-test/runtime")==0);CHECK(rmdir(dir)==0);
    printf("INOTIFY_PASS checks=%u real_events rename_cookie inode_lifetime epoll atomic_read_failure permissions oneshot overflow dup instance_refund3\n",checks);return 0;
}
