#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL ipc-check:%d: %s (errno=%d)\n",__LINE__,#x,errno); exit(1); } } while (0)

static void wait_ok(pid_t pid) {
    int status;
    CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void byte(int fd, char value) {
    CHECK(write(fd,&value,1)==1);
}

static char get(int fd) {
    char value;
    CHECK(read(fd,&value,1)==1);
    return value;
}

static void send_rights(int fd, const int *rights, unsigned count, const char *text) {
    unsigned char control[CMSG_SPACE(64 * sizeof(int))] = {0};
    struct iovec iov = {.iov_base = (void *)text, .iov_len = strlen(text)};
    struct msghdr h = {.msg_iov = &iov,
                       .msg_iovlen = 1,
                       .msg_control = control,
                       .msg_controllen = CMSG_SPACE(count * sizeof(int))};
    struct cmsghdr *c = CMSG_FIRSTHDR(&h);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(count * sizeof(int));
    memcpy(CMSG_DATA(c), rights, count * sizeof(int));
    CHECK(sendmsg(fd,&h,MSG_NOSIGNAL)==(ssize_t)iov.iov_len);
}

static int receive_right(int fd, int flags, size_t capacity, int *out_flags, char *text) {
    unsigned char control[CMSG_SPACE(64 * sizeof(int))] = {0};
    struct iovec iov = {.iov_base = text, .iov_len = 16};
    struct msghdr h = {
        .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = capacity};
    CHECK(recvmsg(fd,&h,flags)>0);
    *out_flags = h.msg_flags;
    struct cmsghdr *c = CMSG_FIRSTHDR(&h);
    if (!c)
        return -1;
    CHECK(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len==CMSG_LEN(sizeof(int)));
    int received;
    memcpy(&received, CMSG_DATA(c), sizeof(received));
    return received;
}

static void memory_objects(void) {
    int fd = memfd_create("coherence", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(fd>=0);
    CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC);
    CHECK(ftruncate(fd,8192)==0);
    volatile uint32_t *a = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(a!=MAP_FAILED);
    volatile uint32_t *b = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(b!=MAP_FAILED);
    a[0] = 0x12345;
    CHECK(b[0]==a[0]);
    uint32_t word = 0x98765;
    CHECK(pwrite(fd,&word,4,4)==4 && a[1]==word && b[1]==word);
    CHECK(lseek(fd,0,SEEK_CUR)==0);
    CHECK(pread(fd,&word,4,0)==4 && word==a[0]);
    uint32_t *private = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(private!=MAP_FAILED);
    private[0] = 73;
    CHECK(a[0]==0x12345);
    int pair[2];
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    pid_t child = fork();
    CHECK(child>=0);
    if (!child) {
        close(pair[0]);
        CHECK(close(fd)==0);
        a[2] = 0xdecaf;
        byte(pair[1], 'S');
        CHECK(get(pair[1])=='P');
        CHECK(a[3]==0xbeef);
        CHECK(munmap((void *)a,8192)==0);
        CHECK(munmap((void *)b,8192)==0);
        _exit(0);
    }

    close(pair[1]);
    CHECK(get(pair[0])=='S');
    CHECK(a[2]==0xdecaf);
    a[3] = 0xbeef;
    byte(pair[0], 'P');
    wait_ok(child);
    close(pair[0]);
    errno = 0;
    CHECK(ftruncate(fd,4096)==-1 && errno==EBUSY);
    CHECK(ftruncate(fd,12288)==0 && a[0]==0x12345);
    unsigned char *third = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 8192);
    CHECK(third!=MAP_FAILED);
    for (int i = 0; i < 4096; i++)
        CHECK(third[i]==0);
    errno = 0;
    CHECK(fcntl(fd,F_ADD_SEALS,F_SEAL_WRITE)==-1 && errno==EBUSY);
    errno = 0;
    CHECK(mremap((void *)a,8192,12288,0)==MAP_FAILED && errno==ENOTSUP);
    CHECK(munmap((void *)b,8192)==0 && munmap(third,4096)==0);
    CHECK(mprotect((void *)a,8192,PROT_READ)==0);
    CHECK(fcntl(fd,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK)==0);
    CHECK(fcntl(fd,F_GET_SEALS)==(F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK));
    errno = 0;
    CHECK(pwrite(fd,&word,4,0)==-1 && errno==EPERM);
    CHECK(mprotect((void *)a,8192,PROT_READ|PROT_WRITE)==-1);
    CHECK(mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0)==MAP_FAILED);
    errno = 0;
    CHECK(ftruncate(fd,16384)==-1 && errno==EPERM);
    errno = 0;
    CHECK(ftruncate(fd,4096)==-1 && errno==EPERM);
    struct stat st;
    CHECK(fstat(fd,&st)==0 && st.st_size==12288);
    CHECK(close(fd)==0 && a[2]==0xdecaf && a[3]==0xbeef);
    CHECK(munmap((void *)a,8192)==0 && munmap(private,8192)==0);
    fd = memfd_create("bounds", 0);
    CHECK(fd>=0);
    errno = 0;
    CHECK(fcntl(fd,F_ADD_SEALS,F_SEAL_WRITE)==-1 && errno==EPERM);
    errno = 0;
    CHECK(ftruncate(fd,128*1024*1024+1)==-1 && errno==EFBIG);
    close(fd);
    puts(
        "PASS coherent memfd aliases, cross-process writes, private copies, growth, seals and close/unmap lifetime");
}

static void records(void) {
    for (int type = SOCK_DGRAM; type <= SOCK_SEQPACKET; type += SOCK_SEQPACKET - SOCK_DGRAM) {
        int fd[2];
        CHECK(socketpair(AF_UNIX,type|SOCK_NONBLOCK|SOCK_CLOEXEC,0,fd)==0);
        int value = 0;
        socklen_t length = sizeof(value);
        CHECK(getsockopt(fd[0],SOL_SOCKET,SO_TYPE,&value,&length)==0 && value==type);
        CHECK(fcntl(fd[0],F_GETFD)&FD_CLOEXEC);
        CHECK(fcntl(fd[0],F_GETFL)&O_NONBLOCK);
        struct sockaddr_un_name {
            sa_family_t family;
        } name;
        length = sizeof(name);
        CHECK(getsockname(fd[0],(void *)&name,&length)==0 && name.family==AF_UNIX && length==2);
        errno = 0;
        CHECK(recv(fd[0],&value,1,0)==-1 && errno==EAGAIN);
        struct iovec v[2] = {{"ab", 2}, {"cdef", 4}};
        CHECK(writev(fd[0],v,2)==6);
        CHECK(send(fd[0],"second",6,MSG_NOSIGNAL)==6);
        CHECK(ioctl(fd[1],FIONREAD,&value)==0 && value==6);
        char text[20] = {0};
        CHECK(recv(fd[1],text,2,MSG_PEEK|MSG_TRUNC)==6 && !memcmp(text,"ab",2));
        struct iovec out = {text, 2};
        struct msghdr h = {.msg_iov = &out, .msg_iovlen = 1};
        CHECK(recvmsg(fd[1],&h,0)==2 && (h.msg_flags&MSG_TRUNC));
        if (type == SOCK_SEQPACKET)
            CHECK(h.msg_flags&MSG_EOR);
        CHECK(read(fd[1],text,sizeof(text))==6 && !memcmp(text,"second",6));
        for (int i = 0; i < 32; i++)
            CHECK(send(fd[0],"x",1,MSG_NOSIGNAL)==1);
        errno = 0;
        CHECK(send(fd[0],"x",1,MSG_NOSIGNAL)==-1 && errno==EAGAIN);
        CHECK(recv(fd[1],text,1,0)==1);
        CHECK(send(fd[0],"y",1,MSG_NOSIGNAL)==1);
        CHECK(shutdown(fd[1],SHUT_RD)==0);
        errno = 0;
        CHECK(send(fd[0],"x",1,MSG_NOSIGNAL)==-1 && errno==EPIPE);
        CHECK(shutdown(fd[0],SHUT_WR)==0);
        CHECK(read(fd[1],text,1)==0);
        close(fd[0]);
        close(fd[1]);
    }

    puts(
        "PASS Unix datagram/seqpacket records, vectors, peek/truncation, pollable bounds and shutdown");
}

static void descriptors(void) {
    int pair[2];
    CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair)==0);
    int fd = memfd_create("rights", MFD_ALLOW_SEALING);
    CHECK(fd>=0 && ftruncate(fd,4096)==0);
    CHECK(write(fd,"real-memory",11)==11);
    send_rights(pair[0], &fd, 1, "payload");
    close(fd);
    char text[20] = {0};
    int flags;
    fd = receive_right(pair[1], MSG_PEEK | MSG_CMSG_CLOEXEC, CMSG_SPACE(sizeof(int)), &flags, text);
    CHECK(fd>=0 && (fcntl(fd,F_GETFD)&FD_CLOEXEC) && !memcmp(text,"payload",7));
    close(fd);
    fd = receive_right(pair[1], MSG_CMSG_CLOEXEC, CMSG_SPACE(sizeof(int)), &flags, text);
    CHECK(fd>=0 && !(flags&MSG_CTRUNC));
    char data[12] = {0};
    CHECK(pread(fd,data,11,0)==11 && !strcmp(data,"real-memory"));
    int dup = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    CHECK(dup>=0);
    int both[2] = {fd, dup};
    send_rights(pair[0], both, 2, "truncated");
    close(fd);
    close(dup);
    fd = receive_right(pair[1], 0, CMSG_LEN(sizeof(int)), &flags, text);
    CHECK(fd>=0 && (flags&MSG_CTRUNC));
    close(fd);
    fd = open("/dev/null", O_RDWR);
    CHECK(fd>=0);
    send_rights(pair[0], &fd, 1, "discard");
    close(fd);
    CHECK(read(pair[1],text,sizeof(text))==7);
    unsigned char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {"bad", 3};
    struct msghdr h = {.msg_iov = &iov,
                       .msg_iovlen = 1,
                       .msg_control = control,
                       .msg_controllen = sizeof(control)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&h);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    int invalid = 9999;
    memcpy(CMSG_DATA(c), &invalid, sizeof(invalid));
    errno = 0;
    CHECK(sendmsg(pair[0],&h,MSG_NOSIGNAL)==-1 && errno==EBADF);
    memcpy(CMSG_DATA(c), &pair[0], sizeof(int));
    errno = 0;
    CHECK(sendmsg(pair[0],&h,MSG_NOSIGNAL)==-1 && errno==ELOOP);
    close(pair[0]);
    close(pair[1]);
    for (int i = 0; i < 600; i++) {
        CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair)==0);
        fd = memfd_create("queued-close", 0);
        CHECK(fd>=0 && ftruncate(fd,4096)==0);
        send_rights(pair[0], &fd, 1, "queued");
        close(fd);
        close(pair[0]);
        close(pair[1]);
    }

    puts(
        "PASS SCM_RIGHTS references, CLOEXEC, peek, ancillary truncation, invalid/cyclic rights and 600 queued-close lifetimes");
}

struct reader {
    int fd;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int entered;
    char result;
};

static void *blocked(void *arg) {
    struct reader *r = arg;
    CHECK(pthread_mutex_lock(&r->lock)==0);
    r->entered = 1;
    CHECK(pthread_cond_signal(&r->cond)==0 && pthread_mutex_unlock(&r->lock)==0);
    r->result = get(r->fd);
    return NULL;
}

static volatile sig_atomic_t interrupted;

static void on_interrupt(int signal) {
    (void)signal;
    interrupted++;
}

static void *interrupt_epoll(void *arg) {
    usleep(10000);
    CHECK(pthread_kill(*(pthread_t *)arg,SIGUSR1)==0);
    return NULL;
}

static void pinning(void) {
    int pair[2];
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    struct reader r = {
        .fd = pair[1], .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};
    pthread_t thread;
    CHECK(pthread_create(&thread,NULL,blocked,&r)==0);
    CHECK(pthread_mutex_lock(&r.lock)==0);
    while (!r.entered)
        CHECK(pthread_cond_wait(&r.cond,&r.lock)==0);
    CHECK(pthread_mutex_unlock(&r.lock)==0);
    usleep(20000);
    CHECK(close(pair[1])==0);
    int replacement = open("/dev/null", O_RDONLY);
    CHECK(replacement==pair[1]);
    byte(pair[0], 'K');
    CHECK(pthread_join(thread,NULL)==0 && r.result=='K');
    char text;
    CHECK(read(replacement,&text,1)==0);
    struct pollfd p = {pair[0], POLLIN, 0};
    CHECK(poll(&p,1,0)==1 && (p.revents&POLLHUP));
    close(replacement);
    close(pair[0]);
    puts("PASS blocked Unix I/O retains its endpoint across sibling close/reuse");
}

static void epoll_events(void) {
    int ep = epoll_create1(EPOLL_CLOEXEC), pair[2];
    CHECK(ep>=0 && (fcntl(ep,F_GETFD)&FD_CLOEXEC));
    CHECK(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,pair)==0);
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = 0x123456789abcdef0ULL}, events[2];
    CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,pair[1],&event)==0);
    errno = 0;
    CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,pair[1],&event)==-1 && errno==EEXIST);
    CHECK(epoll_wait(ep,events,2,2)==0);
    byte(pair[0], 'E');
    CHECK(epoll_wait(ep,events,2,20)==1 && events[0].data.u64==event.data.u64 && (events[0].events&EPOLLIN));
    CHECK(epoll_wait(ep,events,2,0)==1);
    CHECK(get(pair[1])=='E');
    CHECK(epoll_wait(ep,events,2,0)==0);
    event.events = EPOLLIN | EPOLLONESHOT;
    CHECK(epoll_ctl(ep,EPOLL_CTL_MOD,pair[1],&event)==0);
    byte(pair[0], 'O');
    CHECK(epoll_wait(ep,events,2,0)==1 && epoll_wait(ep,events,2,0)==0);
    CHECK(epoll_ctl(ep,EPOLL_CTL_MOD,pair[1],&event)==0 && epoll_wait(ep,events,2,0)==1);
    CHECK(get(pair[1])=='O');
    event.events = EPOLLIN;
    CHECK(epoll_ctl(ep,EPOLL_CTL_MOD,pair[1],&event)==0);
    int duplicate = dup(pair[1]);
    CHECK(duplicate>=0 && close(pair[1])==0);
    int reused = open("/dev/null", O_RDONLY);
    CHECK(reused==pair[1]);
    byte(pair[0], 'D');
    CHECK(epoll_wait(ep,events,2,0)==1 && get(duplicate)=='D');
    errno = 0;
    CHECK(epoll_ctl(ep,EPOLL_CTL_MOD,reused,&event)==-1 && errno==EPERM);
    close(duplicate);
    CHECK(epoll_wait(ep,events,2,0)==0);
    close(reused);
    close(pair[0]);
    int fd = memfd_create("non-pollable", 0);
    CHECK(fd>=0);
    errno = 0;
    CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event)==-1 && errno==EPERM);
    close(fd);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    event.events = EPOLLIN | EPOLLET;
    errno = 0;
    CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,pair[1],&event)==-1 && errno==ENOTSUP);
    event.events = EPOLLIN;
    CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,pair[1],&event)==0);
    close(pair[0]);
    CHECK(epoll_pwait(ep,events,2,20,NULL)==1 && (events[0].events&EPOLLHUP));
    close(ep);
    close(pair[1]);
    ep = epoll_create1(0);
    CHECK(ep>=0);
    struct sigaction action = {.sa_handler = on_interrupt};
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1,&action,NULL)==0);
    sigset_t blocked_mask, old_mask, temporary, after;
    sigemptyset(&blocked_mask);
    sigaddset(&blocked_mask, SIGUSR1);
    sigemptyset(&temporary);
    CHECK(pthread_sigmask(SIG_BLOCK,&blocked_mask,&old_mask)==0);
    pthread_t owner = pthread_self(), thread;
    CHECK(pthread_create(&thread,NULL,interrupt_epoll,&owner)==0);
    errno = 0;
    CHECK(epoll_pwait(ep,events,2,1000,&temporary)==-1 && errno==EINTR && interrupted==1);
    CHECK(pthread_join(thread,NULL)==0 && pthread_sigmask(SIG_BLOCK,NULL,&after)==0 && sigismember(&after,SIGUSR1));
    CHECK(pthread_sigmask(SIG_SETMASK,&old_mask,NULL)==0);
    close(ep);
    puts(
        "PASS epoll level/one-shot readiness, timeouts, duplicate/close/reuse, hangup and rejected unsupported edge mode");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(80);
    memory_objects();
    records();
    descriptors();
    pinning();
    epoll_events();
    puts("IPC CHECK PASS");
    return 0;
}
