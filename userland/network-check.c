#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr,"NETWORK CHECK FAIL line %d: %s (errno=%d)\n",__LINE__,#c,errno); exit(1); } } while(0)

static struct sockaddr_in loopback(unsigned port) {
    return (struct sockaddr_in){
        .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {htonl(INADDR_LOOPBACK)}};
}

static struct sockaddr_in bound(int socket) {
    struct sockaddr_in address = loopback(0);
    socklen_t length = sizeof(address);
    REQUIRE(bind(socket,(void *)&address,length)==0);
    REQUIRE(getsockname(socket,(void *)&address,&length)==0);
    REQUIRE(length==sizeof(address) && address.sin_port!=0);
    return address;
}

static void write_complete(int socket, const void *data, size_t bytes) {
    const unsigned char *at = data;
    while (bytes) {
        ssize_t n = send(socket, at, bytes, MSG_NOSIGNAL);
        REQUIRE(n>0);
        at += n;
        bytes -= n;
    }
}

static void counters(void) {
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    REQUIRE(fd>=0);
    REQUIRE(fcntl(fd,F_GETFD)&FD_CLOEXEC);
    uint64_t value = 0;
    errno = 0;
    REQUIRE(read(fd,&value,8)==-1 && errno==EAGAIN);
    value = UINT64_MAX;
    errno = 0;
    REQUIRE(write(fd,&value,8)==-1 && errno==EINVAL);
    value = 5;
    errno = 0;
    REQUIRE(write(fd,&value,7)==-1 && errno==EINVAL);
    REQUIRE(eventfd_write(fd,value)==0);
    struct pollfd p = {fd, POLLIN | POLLOUT, 0};
    REQUIRE(poll(&p,1,0)==1 && p.revents==(POLLIN|POLLOUT));
    uint64_t values[2] = {0};
    REQUIRE(read(fd,values,sizeof(values))==8 && values[0]==5);
    value = UINT64_MAX - 1;
    REQUIRE(eventfd_write(fd,value)==0);
    REQUIRE(poll(&p,1,0)==1 && p.revents==POLLIN);
    errno = 0;
    REQUIRE(eventfd_write(fd,1)==-1 && errno==EAGAIN);
    REQUIRE(eventfd_read(fd,&value)==0 && value==UINT64_MAX-1);
    errno = 0;
    REQUIRE(pread(fd,&value,8,0)==-1 && errno==ESPIPE);
    int semaphore = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    REQUIRE(semaphore>=0);
    REQUIRE(eventfd_read(semaphore,&value)==0 && value==1);
    REQUIRE(eventfd_read(semaphore,&value)==0 && value==1);
    errno = 0;
    REQUIRE(eventfd_read(semaphore,&value)==-1 && errno==EAGAIN);
    close(semaphore);
    REQUIRE(fcntl(fd,F_SETFL,0)==0);
    pid_t child = fork();
    REQUIRE(child>=0);
    if (!child) {
        usleep(10000);
        REQUIRE(eventfd_write(fd,7)==0);
        close(fd);
        _exit(0);
    }

    REQUIRE(eventfd_read(fd,&value)==0 && value==7);
    int status;
    REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    value = 13;
    struct iovec vectors[] = {{&value, 4}, {(char *)&value + 4, 4}};
    REQUIRE(writev(fd,vectors,2)==8);
    value = 0;
    REQUIRE(readv(fd,vectors,2)==8 && value==13);
    close(fd);
    errno = 0;
    REQUIRE(eventfd(0,2)==-1 && errno==EINVAL);
    puts(
        "EVENTFD CHECK PASS: counters, semaphore, overflow, blocking, poll, fork and vectored I/O");
}

int main(void) {
    counters();
    unsigned char random1[256], random2[256];
    REQUIRE(getrandom(random1,sizeof(random1),0)==sizeof(random1));
    REQUIRE(getrandom(random2,sizeof(random2),0)==sizeof(random2));
    REQUIRE(memcmp(random1,random2,sizeof(random1))!=0);
    errno = 0;
    REQUIRE(getrandom(random1,1,4)==-1 && errno==EINVAL);
    errno = 0;
    REQUIRE(socket(AF_UNIX,SOCK_STREAM,0)==-1 && errno==EAFNOSUPPORT);
    int udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(udp>=0 && sender>=0);
    REQUIRE(fcntl(udp,F_GETFD)&FD_CLOEXEC);
    struct sockaddr_in destination = bound(udp), source = {0};
    socklen_t length = sizeof(source);
    unsigned char data[17000], received[17000];
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (i * 31 + 7) & 255;
    REQUIRE(sendto(sender,data,sizeof(data),0,(void *)&destination,sizeof(destination))==sizeof(data));
    struct pollfd poller = {udp, POLLIN, 0};
    REQUIRE(poll(&poller,1,3000)==1 && (poller.revents&POLLIN));
    REQUIRE(recvfrom(udp,received,3,MSG_PEEK,(void *)&source,&length)==3);
    REQUIRE(!memcmp(data,received,3));
    int available = 0;
    REQUIRE(ioctl(udp,FIONREAD,&available)==0 && available==sizeof(data));
    REQUIRE(recvfrom(udp,received,16,MSG_TRUNC,(void *)&source,&length)==sizeof(data));
    REQUIRE(length==sizeof(source) && source.sin_family==AF_INET && source.sin_port);
    errno = 0;
    REQUIRE(recv(udp,received,1,0)==-1 && errno==EAGAIN);
    struct iovec parts[] = {{data, 5}, {data + 5, 7}};
    struct msghdr message = {.msg_name = &destination,
                             .msg_namelen = sizeof(destination),
                             .msg_iov = parts,
                             .msg_iovlen = 2};
    REQUIRE(sendmsg(sender,&message,MSG_NOSIGNAL)==12);
    REQUIRE(poll(&poller,1,3000)==1);
    struct iovec output[] = {{received, 3}, {received + 3, 5}};
    message = (struct msghdr){
        .msg_name = &source, .msg_namelen = sizeof(source), .msg_iov = output, .msg_iovlen = 2};
    REQUIRE(recvmsg(udp,&message,0)==8 && (message.msg_flags&MSG_TRUNC));
    REQUIRE(!memcmp(data,received,8) && message.msg_namelen==sizeof(source));
    REQUIRE(sendto(sender,data,0,0,(void *)&destination,sizeof(destination))==0);
    REQUIRE(poll(&poller,1,3000)==1 && recv(udp,received,sizeof(received),0)==0);
    close(udp);
    close(sender);
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    REQUIRE(listener>=0);
    destination = bound(listener);
    REQUIRE(listen(listener,4)==0);
    REQUIRE(dup2(listener,70)==70);
    pid_t child = fork();
    REQUIRE(child>=0);
    if (!child) {
        close(listener);
        close(70);
        int client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        REQUIRE(client>=0);
        int result = connect(client, (void *)&destination, sizeof(destination));
        REQUIRE(result==0 || (result<0 && errno==EINPROGRESS));
        poller = (struct pollfd){client, POLLOUT, 0};
        REQUIRE(poll(&poller,1,3000)==1);
        int error = -1;
        socklen_t option_size = sizeof(error);
        REQUIRE(getsockopt(client,SOL_SOCKET,SO_ERROR,&error,&option_size)==0 && error==0);
        REQUIRE(fcntl(client,F_SETFL,0)==0);
        for (unsigned i = 0; i < 4; i++)
            write_complete(client, data, sizeof(data));
        REQUIRE(shutdown(client,SHUT_WR)==0);
        REQUIRE(recv(client,received,8,MSG_WAITALL)==8 && !memcmp(received,"TCP_PASS",8));
        REQUIRE(recv(client,received,1,0)==0);
        close(client);
        _exit(0);
    }

    fd_set readers;
    FD_ZERO(&readers);
    FD_SET(70, &readers);
    struct timeval timeout = {3, 0};
    REQUIRE(select(71,&readers,NULL,NULL,&timeout)==1 && FD_ISSET(70,&readers));
    int peer = accept4(listener, (void *)&source, &length, SOCK_CLOEXEC);
    REQUIRE(peer>=0);
    REQUIRE(fcntl(peer,F_GETFD)&FD_CLOEXEC);
    size_t done = 0;
    while (done < sizeof(data) * 4) {
        ssize_t n = read(peer, received, sizeof(received));
        REQUIRE(n>0);
        for (ssize_t i = 0; i < n; i++)
            REQUIRE(received[i]==data[(done+i)%sizeof(data)]);
        done += n;
    }

    REQUIRE(read(peer,received,1)==0);
    output[0] = (struct iovec){"TCP_", 4};
    output[1] = (struct iovec){"PASS", 4};
    REQUIRE(writev(peer,output,2)==8);
    REQUIRE(shutdown(peer,SHUT_WR)==0);
    close(peer);
    close(listener);
    close(70);
    int status = -1;
    REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    /* A reset during a SYN is a refused connection, not a successful connect
       or an established-connection reset. SO_ERROR must report it once. */
    int refused = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    REQUIRE(refused>=0);
    errno = 0;
    REQUIRE(connect(refused,(void *)&destination,sizeof(destination))<0 && errno==EINPROGRESS);
    poller = (struct pollfd){refused, POLLOUT, 0};
    REQUIRE(poll(&poller,1,3000)==1);
    int error = -1;
    socklen_t option_size = sizeof(error);
    REQUIRE(getsockopt(refused,SOL_SOCKET,SO_ERROR,&error,&option_size)==0 && error==ECONNREFUSED);
    REQUIRE(getsockopt(refused,SOL_SOCKET,SO_ERROR,&error,&option_size)==0 && error==0);
    close(refused);
    puts(
        "NETWORK CHECK PASS: entropy, UDP datagrams/peek/truncation/sendmsg/recvmsg, TCP/accept/connect/EOF, nonblocking/poll/select/fork");
    return 0;
}
