#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"PRIVATE_STREAM_FAIL line=%u errno=%d expression=%s\n",__LINE__,errno,#x);exit(1);}}while(0)
static struct sockaddr_un address(const char *path) {
    struct sockaddr_un a={.sun_family=AF_UNIX};strcpy(a.sun_path,path);return a;
}
static int endpoint(void){int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);CHECK(fd>=0);return fd;}
static void refused(int fd,const char *path,int expected) {
    struct sockaddr_un a=address(path);errno=0;
    CHECK(bind(fd,(void*)&a,sizeof(a))==-1 && errno==expected);
}
static volatile sig_atomic_t signal_hits;
static void stream_signal(int sig) { (void)sig;signal_hits++; }
static void signal_ready_stream(void) {
    struct sigaction action={.sa_handler=stream_signal},previous;
    sigemptyset(&action.sa_mask);CHECK(sigaction(SIGUSR1,&action,&previous)==0);
    int pair[2];CHECK(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair)==0);
    pid_t parent=getpid(),child=fork();CHECK(child>=0);
    if(!child) {
        close(pair[0]);close(pair[1]);
        for(unsigned i=0;i<256;i++) {usleep(1000);if(kill(parent,SIGUSR1))_exit(1);}
        _exit(0);
    }
    /* Ready nonblocking operations must complete without EINTR, including
       while another task queues handled signals during the syscall. This
       exercises the same send(MSG_NOSIGNAL) path as Mojo's data channel. */
    for(unsigned i=0;i<16384;i++) {
        unsigned char sent=(unsigned char)i,received=0;
        CHECK(send(pair[0],&sent,1,MSG_NOSIGNAL)==1);
        CHECK(recv(pair[1],&received,1,0)==1 && received==sent);
        errno=0;CHECK(recv(pair[1],&received,1,MSG_DONTWAIT)==-1 && errno==EAGAIN);
    }
    int status;pid_t joined;
    do {joined=waitpid(child,&status,0);} while(joined<0 && errno==EINTR);
    CHECK(joined==child && WIFEXITED(status) && WEXITSTATUS(status)==0 && signal_hits>0);
    CHECK(close(pair[0])==0 && close(pair[1])==0);
    /* A genuinely blocking empty receive must still be interruptible. */
    CHECK(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair)==0);
    child=fork();CHECK(child>=0);
    if(!child) {close(pair[0]);usleep(10000);int result=kill(parent,SIGUSR1);usleep(10000);close(pair[1]);_exit(result?1:0);}
    CHECK(close(pair[1])==0);unsigned char byte;
    errno=0;CHECK(recv(pair[0],&byte,1,0)==-1 && errno==EINTR);
    do {joined=waitpid(child,&status,0);} while(joined<0 && errno==EINTR);
    CHECK(joined==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(close(pair[0])==0 && sigaction(SIGUSR1,&previous,NULL)==0);
    printf("UNIX_SIGNAL_READY_PASS messages=16384 signals=%d blocking_eintr=1\n",signal_hits);
}
int main(int argc,char **argv) {
    CHECK(getuid()==1000 && getgid()==1000);
    if(argc==2 && !strcmp(argv[1],"--signals-only")) {signal_ready_stream();return 0;}
    const char *path="/home/port-test/runtime/private-stream-check.sock";
    struct sockaddr_un a=address(path);unlink(path);
    int server=endpoint();
    CHECK(bind(server,(void*)1,sizeof(a))==-1 && errno==EFAULT);
    refused(server,"/tmp/private-stream-forbidden.sock",EACCES);
    struct sockaddr_un abstract={.sun_family=AF_UNIX};memcpy(abstract.sun_path+1,"private-stream",14);
    CHECK(bind(server,(void*)&abstract,sizeof(abstract))==-1 && errno==EACCES);
    unlink("/home/port-test/runtime/escape");
    CHECK(symlink("/tmp","/home/port-test/runtime/escape")==0);
    refused(server,"/home/port-test/runtime/escape/forbidden.sock",EACCES);
    CHECK(unlink("/home/port-test/runtime/escape")==0);
    CHECK(socket(AF_INET,SOCK_STREAM,0)==-1 && errno==EPERM);
    CHECK(socket(AF_UNIX,SOCK_DGRAM,0)==-1 && errno==EPERM);
    CHECK(bind(server,(void*)&a,sizeof(a))==0);
    struct stat st;CHECK(lstat(path,&st)==0 && S_ISSOCK(st.st_mode) && st.st_uid==1000 && (st.st_mode&0777)==0600);
    CHECK(listen(server,1)==0);
    int duplicate=endpoint();refused(duplicate,path,EADDRINUSE);CHECK(close(duplicate)==0);
    int first=endpoint(),second=endpoint();
    CHECK(connect(first,(void*)&a,sizeof(a))==0);
    CHECK(connect(second,(void*)&a,sizeof(a))==-1 && errno==EAGAIN);
    struct sockaddr_un peer;socklen_t len=sizeof(peer);
    CHECK(accept4(server,(void*)&peer,(void*)1,SOCK_NONBLOCK)==-1 && errno==EFAULT);
    CHECK(accept4(server,NULL,NULL,0x123)==-1 && errno==EINVAL);
    struct pollfd event={.fd=server,.events=POLLIN};CHECK(poll(&event,1,0)==1 && (event.revents&POLLIN));
    int accepted=accept4(server,(void*)&peer,&len,SOCK_NONBLOCK|SOCK_CLOEXEC);CHECK(accepted>=0 && peer.sun_family==AF_UNIX && len==2);
    CHECK((fcntl(accepted,F_GETFL)&O_NONBLOCK)!=0 && (fcntl(accepted,F_GETFD)&FD_CLOEXEC)!=0);
    CHECK(accept4(server,NULL,NULL,SOCK_NONBLOCK)==-1 && errno==EAGAIN);
    struct ucred cred;len=sizeof(cred);CHECK(getsockopt(first,SOL_SOCKET,SO_PEERCRED,&cred,&len)==0 && cred.pid==getpid() && cred.uid==1000 && cred.gid==1000);
    len=sizeof(peer);CHECK(getpeername(first,(void*)&peer,&len)==0 && !strcmp(peer.sun_path,path));
    CHECK(write(first,"request",7)==7);char bytes[16]={0};CHECK(read(accepted,bytes,sizeof(bytes))==7 && !strcmp(bytes,"request"));
    CHECK(write(accepted,"reply",5)==5);memset(bytes,0,sizeof(bytes));CHECK(read(first,bytes,sizeof(bytes))==5 && !strcmp(bytes,"reply"));
    CHECK(shutdown(first,SHUT_WR)==0 && read(accepted,bytes,sizeof(bytes))==0);
    CHECK(write(accepted,"tail",4)==4 && read(first,bytes,sizeof(bytes))==4);
    CHECK(close(accepted)==0 && close(first)==0 && close(second)==0);
    /* A listener cannot enter SCM_RIGHTS cycles through its accept queue. */
    int pair[2];CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    char c='x',control[CMSG_SPACE(sizeof(int))]={0};struct iovec iov={&c,1};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    struct cmsghdr *header=CMSG_FIRSTHDR(&msg);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(header),&server,sizeof(server));
    CHECK(sendmsg(pair[0],&msg,0)==-1 && errno==EOPNOTSUPP);CHECK(close(pair[0])==0 && close(pair[1])==0);
    /* Unlinking/rebinding selects the new inode, preserving the old listener. */
    CHECK(unlink(path)==0);int replacement=endpoint();CHECK(bind(replacement,(void*)&a,sizeof(a))==0 && listen(replacement,1)==0);
    int client=endpoint();CHECK(connect(client,(void*)&a,sizeof(a))==0);
    CHECK(accept4(server,NULL,NULL,SOCK_NONBLOCK)==-1 && errno==EAGAIN);
    accepted=accept4(replacement,NULL,NULL,SOCK_NONBLOCK);CHECK(accepted>=0);
    CHECK(close(client)==0 && close(accepted)==0 && close(replacement)==0 && close(server)==0);CHECK(unlink(path)==0);
    /* Closing a listener with an unaccepted connection closes the peer. */
    server=endpoint();CHECK(bind(server,(void*)&a,sizeof(a))==0 && listen(server,1)==0);
    client=endpoint();CHECK(connect(client,(void*)&a,sizeof(a))==0);CHECK(close(server)==0);
    CHECK(read(client,bytes,sizeof(bytes))==0);CHECK(close(client)==0 && unlink(path)==0);
    server=endpoint();CHECK(bind(server,(void*)&a,sizeof(a))==0 && listen(server,1)==0);
    pid_t parent=getpid(),child=fork();CHECK(child>=0);
    if(!child) {
        close(server);int connection=endpoint();CHECK(connect(connection,(void*)&a,sizeof(a))==0);
        struct ucred remote;socklen_t size=sizeof(remote);
        CHECK(getsockopt(connection,SOL_SOCKET,SO_PEERCRED,&remote,&size)==0 && remote.pid==parent);
        CHECK(write(connection,"child",5)==5);close(connection);_exit(0);
    }
    event=(struct pollfd){.fd=server,.events=POLLIN};CHECK(poll(&event,1,5000)==1 && (event.revents&POLLIN));
    accepted=accept4(server,NULL,NULL,0);CHECK(accepted>=0);len=sizeof(cred);
    CHECK(getsockopt(accepted,SOL_SOCKET,SO_PEERCRED,&cred,&len)==0 && cred.pid==child && cred.uid==1000);
    memset(bytes,0,sizeof(bytes));CHECK(read(accepted,bytes,sizeof(bytes))==5 && !strcmp(bytes,"child"));
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(close(accepted)==0 && close(server)==0 && unlink(path)==0);
    signal_ready_stream();
    for(unsigned round=0;round<3;round++) {
        int fds[32];for(unsigned i=0;i<32;i++)fds[i]=endpoint();
        CHECK(socket(AF_UNIX,SOCK_STREAM,0)==-1 && errno==ENOBUFS);
        for(unsigned i=0;i<32;i++)CHECK(close(fds[i])==0);
    }
    puts("PRIVATE_STREAM_PASS path/inode/permissions/fault-rollback/backlog/accept/flags/credentials/duplex/halfclose/rights-cycle/cleanup/bounds");
    printf("PRIVATE_STREAM_CHECKS %u\n",checks);return 0;
}
