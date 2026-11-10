#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#define OK(x) do {if(!(x)){fprintf(stderr,"PRIVATE_LOOPBACK_FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);exit(1);}}while(0)
#define DENY(x,e) do{errno=0;OK((x)==-1 && errno==(e));}while(0)
static struct sockaddr_in address(unsigned port) {
    return (struct sockaddr_in){.sin_family=AF_INET,.sin_port=htons(port),.sin_addr={.s_addr=htonl(0x7f000001u)}};
}
static void ready(int fd,short events) {
    struct pollfd p={fd,events,0};OK(poll(&p,1,5000)==1 && (p.revents&events));
}
static void exact(int fd,void *bytes,size_t size,int sending) {
    unsigned char *p=bytes;
    while(size) {
        ssize_t n=sending?write(fd,p,size):read(fd,p,size);
        if(n<0 && (errno==EAGAIN || errno==EINTR)){ready(fd,sending?POLLOUT:POLLIN);continue;}
        OK(n>0 && (size_t)n<=size);p+=n;size-=n;
    }
}
static unsigned name_port(int fd,int peer) {
    struct sockaddr_in a={0};socklen_t size=sizeof(a);
    OK(!(peer?getpeername(fd,(void*)&a,&size):getsockname(fd,(void*)&a,&size)));
    OK(size==sizeof(a) && a.sin_family==AF_INET && a.sin_addr.s_addr==htonl(0x7f000001u));
    return ntohs(a.sin_port);
}
static int option(int fd,int level,int key) {
    int value=-1;socklen_t size=sizeof(value);OK(!getsockopt(fd,level,key,&value,&size)&&size==sizeof(value));return value;
}
static void vectors(void) {
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);OK(listener>=0);
    struct sockaddr_in a=address(0);OK(!bind(listener,(void*)&a,sizeof(a)) && !listen(listener,1));
    a=address(name_port(listener,0));int sender=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);OK(sender>=0);
    OK(!connect(sender,(void*)&a,sizeof(a)));int receiver=accept4(listener,NULL,NULL,SOCK_NONBLOCK);OK(receiver>=0);
    struct iovec *v=calloc(1025,sizeof(*v));unsigned char *source=malloc(131072),*target=malloc(131072);OK(v && source && target);
    for(unsigned i=0;i<131072;i++)source[i]=i%251;
    unsigned counts[]={17,64,1024};
    for(unsigned round=0;round<8;round++)for(unsigned n=0;n<3;n++) {
        unsigned count=counts[n];
        for(unsigned i=0;i<count;i++)v[i]=(struct iovec){source+i,1};
        OK(writev(sender,v,count)==count);
        for(unsigned i=0;i<count;i++)v[i]=(struct iovec){target+i,1};
        OK(readv(receiver,v,count)==count && !memcmp(source,target,count));
    }
    for(unsigned i=0;i<17;i++)v[i]=(struct iovec){source+i,1};
    DENY(syscall(SYS_writev,sender,(void*)1,17),EFAULT);v[16].iov_base=(void*)1;DENY(writev(sender,v,17),EFAULT);
    char byte;DENY(read(receiver,&byte,1),EAGAIN);
    v[16].iov_base=source+16;DENY(writev(sender,v,1025),EINVAL);DENY(read(receiver,&byte,1),EAGAIN);
    v[0].iov_len=16*1024*1024+1;DENY(writev(sender,v,17),EMSGSIZE);v[0].iov_len=1;
    OK(write(sender,"Q",1)==1);memset(target,0xa5,17);
    for(unsigned i=0;i<17;i++)v[i]=(struct iovec){target+i,1};
    v[16].iov_base=(void*)1;DENY(readv(receiver,v,17),EFAULT);
    for(unsigned i=0;i<17;i++)OK(target[i]==0xa5);
    OK(read(receiver,&byte,1)==1 && byte=='Q');
    for(unsigned i=0;i<1024;i++)v[i]=(struct iovec){(void*)1,0};
    OK(writev(sender,v,1024)==0 && readv(receiver,v,1024)==0);
    int pipes[2];OK(!pipe(pipes));DENY(writev(pipes[1],v,17),EINVAL);OK(!close(pipes[0])&&!close(pipes[1]));
    int fresh=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);OK(fresh>=0);DENY(writev(fresh,v,17),EINVAL);OK(!close(fresh));
    for(unsigned i=0;i<1024;i++)v[i]=(struct iovec){source+i*128,128};
    OK(writev(sender,v,1024)==65536);DENY(writev(sender,v,1024),EAGAIN);
    memset(target,0xa5,131072);for(unsigned i=0;i<1024;i++)v[i]=(struct iovec){target+i*128,128};
    OK(readv(receiver,v,1024)==65536 && !memcmp(source,target,65536));
    for(unsigned i=65536;i<131072;i++)OK(target[i]==0xa5);
    DENY(readv(receiver,v,1024),EAGAIN);OK(!shutdown(sender,SHUT_WR));OK(readv(receiver,v,1024)==0);
    signal(SIGPIPE,SIG_IGN);DENY(writev(sender,v,1024),EPIPE);
    free(v);free(source);free(target);OK(!close(sender)&&!close(receiver)&&!close(listener));
    puts("PRIVATE_VECTOR_PASS counts17_64_1024 exact_bytes zero_vectors fault_atomicity retained_queue count1025_and_16MiB_denied legacy_pipe_unconnected_bounds partial64KiB EAGAIN_EOF_EPIPE cleanup");
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);alarm(30);
    if(argc==3 && !strcmp(argv[1],"--root-probe")) {
        OK(getuid()==0);int fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);OK(fd>=0);
        unsigned port=0;OK(*argv[2]);
        for(const char *c=argv[2];*c;c++){OK(*c>='0'&&*c<='9'&&port<=6553);port=port*10+*c-'0';OK(port<=65535);}
        OK(port);struct sockaddr_in a=address(port);int result=connect(fd,(void*)&a,sizeof(a));
        if(result<0 && errno==EINPROGRESS){struct pollfd p={fd,POLLOUT,0};OK(poll(&p,1,5000)==1);OK(option(fd,SOL_SOCKET,SO_ERROR)==ECONNREFUSED);}
        else OK(result<0 && errno==ECONNREFUSED);
        OK(!close(fd));puts("PRIVATE_LOOPBACK_ROOT_ISOLATION_PASS");return 0;
    }
    OK(argc==1 && getuid()==1000 && getgid()==1000);
    OK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1 && prctl(PR_GET_SECCOMP,0,0,0,0)==2);
    DENY(socket(AF_INET6,SOCK_STREAM,0),EPERM);DENY(socket(AF_INET,SOCK_DGRAM,0),EPERM);
    DENY(socket(AF_INET,SOCK_RAW,0),EPERM);DENY(socket(AF_INET,SOCK_STREAM,IPPROTO_UDP),EPERM);
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,IPPROTO_TCP);OK(listener>=0);
    OK((fcntl(listener,F_GETFL)&O_NONBLOCK) && (fcntl(listener,F_GETFD)&FD_CLOEXEC));
    int yes=1;OK(!setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes)));
    OK(!setsockopt(listener,SOL_SOCKET,SO_KEEPALIVE,&yes,sizeof(yes)) && option(listener,SOL_SOCKET,SO_KEEPALIVE)==1);
    int keys[3]={TCP_KEEPIDLE,TCP_KEEPINTVL,TCP_KEEPCNT},values[3]={60,1,10};
    for(unsigned i=0;i<3;i++)OK(!setsockopt(listener,IPPROTO_TCP,keys[i],&values[i],sizeof(int))&&option(listener,IPPROTO_TCP,keys[i])==values[i]);
    int invalid=2;DENY(setsockopt(listener,SOL_SOCKET,SO_KEEPALIVE,&invalid,sizeof(invalid)),EINVAL);
    invalid=0;DENY(setsockopt(listener,IPPROTO_TCP,TCP_KEEPIDLE,&invalid,sizeof(invalid)),EINVAL);
    invalid=128;DENY(setsockopt(listener,IPPROTO_TCP,TCP_KEEPCNT,&invalid,sizeof(invalid)),EINVAL);
    DENY(setsockopt(listener,SOL_SOCKET,SO_KEEPALIVE,(void*)1,4),EFAULT);
    OK(option(listener,SOL_SOCKET,SO_KEEPALIVE)==1 && option(listener,IPPROTO_TCP,TCP_KEEPIDLE)==60);
    int tos=184;OK(!setsockopt(listener,IPPROTO_IP,IP_TOS,&tos,sizeof(tos)) && option(listener,IPPROTO_IP,IP_TOS)==tos);
    invalid=256;DENY(setsockopt(listener,IPPROTO_IP,IP_TOS,&invalid,sizeof(invalid)),EINVAL);
    invalid=-1;DENY(setsockopt(listener,IPPROTO_IP,IP_TOS,&invalid,sizeof(invalid)),EINVAL);
    OK(option(listener,IPPROTO_IP,IP_TOS)==tos);
    OK(option(listener,SOL_SOCKET,SO_REUSEADDR)==1 && option(listener,SOL_SOCKET,SO_DOMAIN)==AF_INET && option(listener,SOL_SOCKET,SO_PROTOCOL)==IPPROTO_TCP);
    struct sockaddr_in a=address(0);
    a.sin_addr.s_addr=INADDR_ANY;DENY(bind(listener,(void*)&a,sizeof(a)),EACCES);
    a.sin_addr.s_addr=htonl(0x08080808u);DENY(bind(listener,(void*)&a,sizeof(a)),EACCES);DENY(connect(listener,(void*)&a,sizeof(a)),EACCES);
    a.sin_addr.s_addr=htonl(0x7f000002u);DENY(bind(listener,(void*)&a,sizeof(a)),EACCES);
    a=address(0);DENY(bind(listener,(void*)1,sizeof(a)),EFAULT);DENY(bind(listener,(void*)&a,sizeof(a)-1),EINVAL);
    OK(!bind(listener,(void*)&a,sizeof(a)));unsigned port=name_port(listener,0);OK(port>=49152 && port<49184);
    OK(!listen(listener,1000) && option(listener,SOL_SOCKET,SO_ACCEPTCONN)==1);
    DENY(accept4(listener,NULL,NULL,SOCK_NONBLOCK),EAGAIN);
    int duplicate=socket(AF_INET,SOCK_STREAM,0);OK(duplicate>=0);a=address(port);
    DENY(bind(duplicate,(void*)&a,sizeof(a)),EADDRINUSE);OK(name_port(duplicate,0)==0);OK(!close(duplicate));
    pid_t child=fork();OK(child>=0);
    if(!child) {
        OK(!close(listener));int fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);OK(fd>=0);
        a=address(0);DENY(connect(fd,(void*)&a,sizeof(a)),ECONNREFUSED);a=address(port);
        OK(!connect(fd,(void*)&a,sizeof(a)) && name_port(fd,1)==port);unsigned local=name_port(fd,0);OK(local && local!=port);
        OK(!setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes)) && option(fd,IPPROTO_TCP,TCP_NODELAY)==1);
        unsigned char data=99,control[CMSG_SPACE(sizeof(int))]={0};struct iovec vector={&data,1};
        struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
        struct cmsghdr *c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&fd,sizeof(fd));
        DENY(sendmsg(fd,&message,MSG_NOSIGNAL),ENOTSUP);
        uint16_t wire=htons(local);exact(fd,&wire,sizeof(wire),1);
        unsigned char payload[65536];for(unsigned i=0;i<sizeof(payload);i++)payload[i]=i%251;
        exact(fd,payload,sizeof(payload),1);OK(!shutdown(fd,SHUT_WR));char response[2];exact(fd,response,2,0);OK(!memcmp(response,"OK",2));
        ready(fd,POLLIN);OK(read(fd,response,1)==0);OK(!close(fd));_exit(0);
    }
    ready(listener,POLLIN);socklen_t size=sizeof(a);DENY(accept4(listener,(void*)1,&size,SOCK_NONBLOCK),EFAULT);
    int accepted=accept4(listener,(void*)&a,&size,SOCK_NONBLOCK|SOCK_CLOEXEC);OK(accepted>=0 && size==sizeof(a));
    OK(option(accepted,SOL_SOCKET,SO_KEEPALIVE)==1);
    OK(option(accepted,IPPROTO_IP,IP_TOS)==tos);
    for(unsigned i=0;i<3;i++)OK(option(accepted,IPPROTO_TCP,keys[i])==values[i]);
    OK(a.sin_family==AF_INET && a.sin_addr.s_addr==htonl(0x7f000001u) && name_port(accepted,0)==port);
    uint16_t wire;exact(accepted,&wire,sizeof(wire),0);OK(ntohs(wire)==name_port(accepted,1));
    unsigned char payload[65536];exact(accepted,payload,sizeof(payload),0);for(unsigned i=0;i<sizeof(payload);i++)OK(payload[i]==i%251);
    ready(accepted,POLLIN);char byte;OK(read(accepted,&byte,1)==0);exact(accepted,"OK",2,1);OK(!shutdown(accepted,SHUT_WR));
    OK(!close(accepted) && !close(listener));int status;OK(waitpid(child,&status,0)==child && WIFEXITED(status)&&!WEXITSTATUS(status));
    vectors();
    for(unsigned pass=0;pass<2;pass++) {
        int slots[32];for(unsigned i=0;i<32;i++){slots[i]=socket(AF_INET,SOCK_STREAM,0);OK(slots[i]>=0);}
        DENY(socket(AF_INET,SOCK_STREAM,0),ENOBUFS);DENY(socket(AF_UNIX,SOCK_STREAM,0),ENOBUFS);
        OK(!close(slots[0]));int unix_fd=socket(AF_UNIX,SOCK_STREAM,0);OK(unix_fd>=0);OK(!close(unix_fd));
        for(unsigned i=1;i<32;i++)OK(!close(slots[i]));
    }
    puts("PRIVATE_LOOPBACK_NATIVE_PASS literal127_only ipv6_udp_raw_wan_any_denied exact65536 halfclose options accept_fault_atomicity no_SCM shared32quota refill2 cleanup");return 0;
}
