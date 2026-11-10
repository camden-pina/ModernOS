#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <uapi/account.h>
#include <uapi/clipboard.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* Native QA operations use the same socket syscalls as Chromium. They do not
 * grant credentials, attach a desktop window or run inside the browser. */
static int large_read(const char *port_text) {
    char *end; long port=strtol(port_text,&end,10);
    if(*end || port<1 || port>65535)return 20;
    int fd=socket(AF_INET,SOCK_STREAM,0), out=-1;
    if(fd<0)return 21;
    struct timeval timeout={.tv_sec=30};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(port)};
    inet_pton(AF_INET,"10.0.2.2",&address.sin_addr);
    if(connect(fd,(void *)&address,sizeof(address)))return 22;
    const char request[]="GET /bulk.css HTTP/1.1\r\nHost: coast-fixture\r\nConnection: close\r\n\r\n";
    if(write(fd,request,sizeof(request)-1)!=(ssize_t)sizeof(request)-1)return 23;
    char header[4096]; size_t used=0;
    while(used+1<sizeof(header)) {
        if(read(fd,header+used,1)!=1)return 24;
        used++;header[used]=0;
        if(used>=4 && !memcmp(header+used-4,"\r\n\r\n",4))break;
    }
    if(!strstr(header," 200 ") || !strstr(header,"Content-Length: 8388608\r\n"))return 25;
    /* Let the advertised receive window fill before the first >64 KiB read. */
    sleep(1);
    char *buffer=malloc(1024*1024); if(!buffer)return 26;
    out=open("/tmp/chromium-large-read.bin",O_CREAT|O_TRUNC|O_WRONLY,0600);
    if(out<0)return 27;
    size_t total=0,largest=0;
    while(total<8388608) {
        ssize_t n=read(fd,buffer,1024*1024);
        if(n<=0)return 28;
        if((size_t)n>largest)largest=n;
        size_t written=0;
        while(written<(size_t)n) {
            ssize_t part=write(out,buffer+written,n-written);
            if(part<=0)return 29;
            written+=part;
        }
        total+=n;
    }
    close(out);close(fd);free(buffer);
    if(total!=8388608 || largest<=65535) {printf("BROWSER_LARGE_READ_FAIL bytes=%zu max_read=%zu\n",total,largest);return 30;}
    printf("BROWSER_LARGE_READ_PASS bytes=%zu max_read=%zu\n",total,largest);
    return 0;
}
static int tcp_pool_check(void) {
    int fds[128];
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(9)};
    inet_pton(AF_INET,"127.0.0.1",&address.sin_addr);
    for(unsigned i=0;i<128;i++) {
        fds[i]=socket(AF_INET,SOCK_STREAM,0);
        if(fds[i]<0)return 31;
        if(connect(fds[i],(void *)&address,sizeof(address))!=-1 || errno!=ECONNREFUSED)return 32;
    }
    errno=0;
    int extra=socket(AF_INET,SOCK_STREAM,0);
    if(extra!=-1 || errno!=ENOMEM)return 33;
    extra=socket(AF_INET,SOCK_DGRAM,0);
    if(extra<0)return 34;
    close(extra);close(fds[0]);
    fds[0]=socket(AF_INET,SOCK_STREAM,0);
    if(fds[0]<0)return 35;
    for(unsigned i=0;i<128;i++)close(fds[i]);
    puts("BROWSER_TCP_POOL_PASS bounded_128 refund_reuse udp_independent");
    return 0;
}

int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--large-read"))return large_read(argv[2]);
    if(argc==2 && !strcmp(argv[1],"--tcp-pool-check"))return tcp_pool_check();
    if(argc!=1)return 36;
    struct os_account_request q={.uid=OS_ACCOUNT_FIRST};
    if(syscall(418,OS_ACCOUNT_BROWSER_LAUNCH,&q)!=-1 || errno!=EPERM)return 1;
    struct os_clipboard_info clipboard;
    errno=0;
    if(syscall(414,OS_CLIPBOARD_INFO,&clipboard,sizeof(clipboard),0)!=-1 || errno!=EPERM)return 10;
    const char *path="/home/port-test/runtime/creat-check";
    int created=syscall(85,path,0600); /* Linux creat, used by Chromium. */
    if(created<0 || write(created,"abc",3)!=3 || close(created))return 6;
    created=syscall(85,path,0600);struct stat st;
    if(created<0 || fstat(created,&st) || st.st_size || (st.st_mode&0777)!=0600 || close(created) || unlink(path))return 7;
    errno=0;
    if(syscall(85,"/etc/browser-creat-denied",0600)!=-1 || errno!=EACCES)return 8;
    errno=0;if(syscall(85,NULL,0600)!=-1 || errno!=EFAULT)return 9;
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if(fd<0)return 2;
    close(fd);
    /* Inherit the actual charged browser domain, then enter a new user
       namespace. The inherited marker must not grant that namespace IP. */
    if(unshare(CLONE_NEWUSER))return 3;
    errno=0;fd=socket(AF_INET,SOCK_DGRAM,0);
    if(fd>=0){close(fd);return 4;}
    if(errno!=EPERM && errno!=EPROTONOSUPPORT && errno!=EAFNOSUPPORT)return 5;
    puts("BROWSER_NETWORK_CHECK_PASS inherited_main_network namespace_denied unprivileged_grant_denied creat_truncate_dac runtime_clipboard_denied");
    return 0;
}
