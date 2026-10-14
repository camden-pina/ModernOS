#define _GNU_SOURCE
#include "include/web_process.h"
#include "include/audio_protocol.h"
#include "include/memory_usage.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL web-capability-check:%d: %s (errno=%d)\n",__LINE__,#x,errno); exit(1); } } while (0)

static struct os_web_identity identity;
static _Thread_local int tls_value = 27;
static volatile int private_value = 91;
static atomic_int stop_worker;
static _Thread_local volatile sig_atomic_t signal_hits;

static void self_signal(int signal) {
    if (signal == SIGUSR1)
        ++signal_hits;
}

static void check_self_signals(void) {
    struct sigaction action = {.sa_handler = self_signal}, previous;
    CHECK(!sigemptyset(&action.sa_mask) && !sigaction(SIGUSR1,&action,&previous));
    signal_hits = 0;
    CHECK(!raise(SIGUSR1) && signal_hits==1);
    CHECK(!syscall(SYS_tkill,syscall(SYS_gettid),SIGUSR1) && signal_hits==2);
    CHECK(!sigaction(SIGUSR1,&previous,NULL));
}

static void read_pipe_bytes(int fd, unsigned char *bytes, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = read(fd, bytes + offset, size - offset);
        CHECK(n>0);
        offset += n;
    }
}

static void check_pipe_capacity(void) {
    int ends[2];
    CHECK(!pipe2(ends,O_NONBLOCK|O_CLOEXEC));
    CHECK(fcntl(ends[0],F_GETPIPE_SZ)==16384);
    unsigned char *bytes = malloc(65536), *received = malloc(65536);
    CHECK(bytes && received);
    for (unsigned i = 0; i < 65536; i++)
        bytes[i] = (unsigned char)(i * 37 + i / 256);
    CHECK(write(ends[1],bytes,12000)==12000);
    read_pipe_bytes(ends[0], received, 8000);
    CHECK(!memcmp(bytes,received,8000));
    CHECK(write(ends[1],bytes+12000,12000)==12000);
    errno = 0;
    CHECK(fcntl(ends[1],F_SETPIPE_SZ,4096)==-1 && errno==EBUSY);
    CHECK(fcntl(ends[0],F_GETPIPE_SZ)==16384);
    CHECK(fcntl(ends[1],F_SETPIPE_SZ,50000)==65536 && fcntl(ends[0],F_GETPIPE_SZ)==65536);
    read_pipe_bytes(ends[0], received, 16000);
    CHECK(!memcmp(bytes+8000,received,16000));
    CHECK(write(ends[1],bytes,65536)==65536);
    errno = 0;
    CHECK(write(ends[1],bytes,1)==-1 && errno==EAGAIN);
    CHECK(read(ends[0],received,1000)==1000 && !memcmp(bytes,received,1000));
    errno = 0;
    CHECK(write(ends[1],bytes,4096)==-1 && errno==EAGAIN);
    read_pipe_bytes(ends[0], received, 64536);
    CHECK(!memcmp(bytes+1000,received,64536));
    CHECK(fcntl(ends[0],F_SETPIPE_SZ,1)==4096);
    CHECK(fcntl(ends[1],F_SETPIPE_SZ,256*1024)==256*1024);
    errno = 0;
    CHECK(fcntl(ends[0],F_SETPIPE_SZ,256*1024+1)==-1 && errno==EPERM);
    errno = 0;
    CHECK(syscall(SYS_fcntl,ends[1],F_SETPIPE_SZ,UINT64_MAX)==-1 && errno==EPERM);
    CHECK(!close(ends[1]) && read(ends[0],received,1)==0 && !close(ends[0]));
    errno = 0;
    CHECK(fcntl(0,F_GETPIPE_SZ)==-1 && errno==EINVAL);
    free(bytes);
    free(received);
    puts("PASS bounded pipe capacity, wrapped data, backpressure and atomic writes");
}

static void identify(void) {
    CHECK(syscall(MODERNOS_WEB_IDENTITY,&identity)==0 && identity.process==(unsigned)getpid());
}

static void wait_ok(int pid) {
    int status;
    CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void no_extra_fds(void) {
    for (int i = 3; i < 128; i++) {
        errno = 0;
        CHECK(fcntl(i,F_GETFD)==-1 && errno==EBADF);
    }
    CHECK(getenv("INHERITED_WEB_SECRET")==NULL && !strcmp(getenv("PATH"),"/bin"));
}

static int start(unsigned role, const char *path, char *const args[], int ipc, int out,
                 const char *origin) {
    struct os_spawn_web spec = {.role = role,
                                .ipc_fd = ipc,
                                .stdout_fd = out,
                                .stderr_fd = out,
                                .origin = (uintptr_t)origin,
                                .home = role == OS_WEB_BROKER ? (uintptr_t)"/home/user" : 0};
    return syscall(MODERNOS_SPAWN_WEB, path, args, &spec);
}

static void send_caps(int fd, const int *rights, unsigned count, const void *text, size_t length) {
    unsigned char control[CMSG_SPACE(4 * sizeof(int))] = {0};
    struct iovec v = {(void *)text, length};
    struct msghdr h = {.msg_iov = &v, .msg_iovlen = 1};
    if (count) {
        h.msg_control = control;
        h.msg_controllen = CMSG_SPACE(count * sizeof(int));
        struct cmsghdr *c = CMSG_FIRSTHDR(&h);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(count * sizeof(int));
        memcpy(CMSG_DATA(c), rights, count * sizeof(int));
    }

    CHECK(sendmsg(fd,&h,MSG_NOSIGNAL)==(ssize_t)length);
}

static int recv_cap(void *text, size_t length, int *flags) {
    unsigned char control[CMSG_SPACE(4 * sizeof(int))] = {0};
    struct iovec v = {text, length};
    struct msghdr h = {
        .msg_iov = &v, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    CHECK(recvmsg(0,&h,MSG_CMSG_CLOEXEC)==(ssize_t)length);
    *flags = h.msg_flags;
    struct cmsghdr *c = CMSG_FIRSTHDR(&h);
    if (!c)
        return -1;
    CHECK(c->cmsg_len==CMSG_LEN(sizeof(int)) && c->cmsg_type==SCM_RIGHTS);
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof(fd));
    return fd;
}

static void receive_owned(int channel, int owned[2]) {
    unsigned char control[CMSG_SPACE(4 * sizeof(int))] = {0};
    char reply = 0;
    struct iovec v = {&reply, 1};
    struct msghdr h = {
        .msg_iov = &v, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    CHECK(recvmsg(channel,&h,MSG_CMSG_CLOEXEC)==1 && reply=='A' && !(h.msg_flags&MSG_CTRUNC));
    struct cmsghdr *c = CMSG_FIRSTHDR(&h);
    CHECK(c && c->cmsg_len==CMSG_LEN(2*sizeof(int)));
    memcpy(owned, CMSG_DATA(c), 2 * sizeof(int));
}

static void renderer_denials(void) {
    check_self_signals();
    errno = 0;
    CHECK(syscall(MODERNOS_AUDIO,OS_AUDIO_RESET,0,0)==-1 && errno==EPERM);
    errno = 0;
    CHECK(fcntl(1,F_SETPIPE_SZ,256*1024)==-1 && errno==EPERM);
    errno = 0;
    CHECK(socket(AF_INET,SOCK_STREAM,0)==-1 && errno==EPERM);
    errno = 0;
    CHECK(open("/etc/ssl/cert.pem",O_RDONLY)==-1 && errno==EACCES);
    errno = 0;
    CHECK(open("/tmp/web-private",O_RDONLY)==-1 && errno==EACCES);
    errno = 0;
    CHECK(open("/home/user/Downloads/escape",O_WRONLY|O_CREAT,0600)==-1 && errno==EACCES);
    errno = 0;
    CHECK(fork()==-1 && errno==EPERM);
    errno = 0;
    CHECK(syscall(SYS_clone,0,NULL,NULL,NULL,NULL)==-1 && errno==EPERM);
    errno = 0;
    CHECK(syscall(402,0,0,0)==-1 && errno==EPERM);
    errno = 0;
    CHECK(syscall(403,0,0)==-1 && errno==EPERM);
    errno = 0;
    CHECK(syscall(408,getpid(),"/tmp/web-private")==-1 && errno==EPERM);
    errno = 0;
    CHECK(syscall(MODERNOS_SPAWN_WEB,0,0,0)==-1 && errno==EPERM);
    errno = 0;
    CHECK(syscall(SYS_execve,"/bin/hello",NULL,NULL)==-1 && errno==EPERM);
    errno = 0;
    CHECK(syscall(SYS_mount,0,0,0,0,0)==-1 && errno==EPERM);
    struct os_memory_usage memory;
    CHECK(!syscall(MODERNOS_MEMORY_USAGE,&memory));
    CHECK(memory.mapped_bytes && memory.peak_mapped_bytes>=memory.mapped_bytes && memory.limit_bytes==768ULL*1024*1024);
    struct rlimit limit, legacy;
    CHECK(!getrlimit(RLIMIT_STACK,&limit) && limit.rlim_cur==8*1024*1024 && limit.rlim_max==limit.rlim_cur);
    CHECK(!syscall(SYS_getrlimit,RLIMIT_STACK,&legacy) && !memcmp(&legacy,&limit,sizeof(limit)));
    CHECK(!syscall(SYS_prlimit64,getpid(),RLIMIT_AS,NULL,&limit) && limit.rlim_cur==memory.limit_bytes && limit.rlim_max==limit.rlim_cur);
    CHECK(!syscall(SYS_prlimit64,0,RLIMIT_STACK,NULL,NULL));
    errno = 0;
    CHECK(syscall(SYS_prlimit64,0,RLIMIT_STACK,NULL,(void *)1)==-1 && errno==EFAULT);
    errno = 0;
    CHECK(syscall(SYS_prlimit64,0,16,NULL,&limit)==-1 && errno==EINVAL);
    errno = 0;
    CHECK(syscall(SYS_prlimit64,getppid(),RLIMIT_STACK,NULL,&limit)==-1 && errno==EPERM);
    errno = 0;
    CHECK(syscall(SYS_prlimit64,0,RLIMIT_AS,&limit,NULL)==-1 && errno==EPERM);
    size_t held = (memory.limit_bytes / 3) & ~4095ULL;
    void *guard = mmap(NULL, held, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(guard!=MAP_FAILED);
    errno = 0;
    CHECK(mmap(NULL,memory.limit_bytes-held,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED && errno==ENOMEM);
    CHECK(!munmap(guard,held));
}

static void *renderer_thread(void *arg) {
    (void)arg;
    struct os_web_identity own;
    CHECK(tls_value==27);
    tls_value = 89;
    CHECK(syscall(MODERNOS_WEB_IDENTITY,&own)==0 && own.role==OS_WEB_RENDERER);
    CHECK(own.domain==identity.domain && !strcmp(own.origin,identity.origin));
    renderer_denials();
    return NULL;
}

struct offer {
    uint32_t word, other_pid;
};

static int renderer(const char *origin) {
    identify();
    CHECK(identity.role==OS_WEB_RENDERER && !strcmp(identity.origin,origin));
    CHECK(identity.domain!=identity.process && !strcmp(getenv("HOME"),"/"));
    no_extra_fds();
    renderer_denials();
    int quota_a = memfd_create("quota-a", 0), quota_b = memfd_create("quota-b", 0);
    CHECK(quota_a>=0 && quota_b>=0 && ftruncate(quota_a,96*1024*1024)==0);
    errno = 0;
    CHECK(ftruncate(quota_b,33*1024*1024)==-1 && errno==ENOMEM);
    CHECK(close(quota_a)==0 && ftruncate(quota_b,33*1024*1024)==0 && close(quota_b)==0);
    CHECK(private_value==91);
    private_value = identity.process;
    pthread_t thread;
    CHECK(pthread_create(&thread,NULL,renderer_thread,NULL)==0);
    CHECK(pthread_join(thread,NULL)==0 && tls_value==27);
    CHECK(write(0,&identity,sizeof(identity))==sizeof(identity));
    struct offer offer;
    int flags;
    int fd = recv_cap(&offer, sizeof(offer), &flags);
    CHECK(fd>=0 && (flags&MSG_CTRUNC));
    CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC);
    errno = 0;
    CHECK(kill(offer.other_pid,SIGTERM)==-1 && errno==EPERM);
    errno = 0;
    CHECK(kill(offer.other_pid,SIGKILL)==-1 && errno==EPERM);
    errno = 0;
    CHECK(kill(getppid(),SIGKILL)==-1 && errno==EPERM);
    errno = 0;
    CHECK(syscall(SYS_tkill,offer.other_pid,0)==-1 && errno==EPERM);
    uint32_t *pixels = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(pixels!=MAP_FAILED);
    CHECK(pixels[0]==offer.word);
    pixels[1] = identity.process;
    close(fd);
    CHECK(pixels[0]==offer.word && private_value==(int)identity.process);
    int channel[2];
    CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET,0,channel)==0);
    int counter = eventfd(13, EFD_CLOEXEC);
    CHECK(counter>=0);
    int owned[] = {counter, channel[1]};
    send_caps(0, owned, 2, "A", 1);
    close(counter);
    close(channel[0]);
    close(channel[1]);
    fd = recv_cap(&offer, sizeof(offer), &flags);
    CHECK(fd==-1 && (flags&MSG_CTRUNC));
    CHECK(pixels[0]==offer.word && pixels[1]==identity.process);
    struct os_web_identity after;
    CHECK(syscall(MODERNOS_WEB_IDENTITY,&after)==0 && !memcmp(&identity,&after,sizeof(after)));
    CHECK(munmap(pixels,4096)==0);
    CHECK(write(0,"B",1)==1);
    return 0;
}

static void *broker_worker(void *arg) {
    (void)arg;
    struct os_web_identity own;
    CHECK(syscall(MODERNOS_WEB_IDENTITY,&own)==0 && own.role==OS_WEB_BROKER);
    check_self_signals();
    while (!atomic_load(&stop_worker))
        usleep(1000);
    return NULL;
}

static void copy_output(int fd) {
    char text[512];
    ssize_t n;
    while ((n = read(fd, text, sizeof(text))) > 0)
        CHECK(write(1,text,n)==n);
    CHECK(n==0);
    close(fd);
}

static void run_component(const char *path, const char *file) {
    int ipc[2], out[2];
    CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,ipc)==0 && pipe(out)==0);
    char *args[] = {(char *)path, (char *)file, NULL};
    int pid = start(OS_WEB_RENDERER, path, args, ipc[1], out[1], "https://component.example:443");
    CHECK(pid>0);
    close(ipc[1]);
    close(out[1]);
    copy_output(out[0]);
    wait_ok(pid);
    close(ipc[0]);
}

static int broker(void) {
    identify();
    CHECK(identity.role==OS_WEB_BROKER && identity.domain==identity.process && !identity.origin[0]);
    no_extra_fds();
    errno = 0;
    CHECK(open("/tmp/web-private",O_RDONLY)==-1 && errno==EACCES);
    CHECK(mkdir("/home/user/.config/webkit",0700)==0 || errno==EEXIST);
    int cookie = open("/home/user/.config/webkit/test-cookie", O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(cookie>=0);
    CHECK(write(cookie,"broker",6)==6);
    close(cookie);
    int net = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(net>=0);
    int ca = open("/etc/ssl/cert.pem", O_RDONLY);
    CHECK(ca>=0);
    pthread_t worker;
    CHECK(pthread_create(&worker,NULL,broker_worker,NULL)==0);
    int ipc[2][2], out[2], pid[2], objects[2], owned[2][2];
    uint32_t *map[2];
    out[0] = -1;
    out[1] = dup(1);
    CHECK(out[1]>=0);
    const char *origins[] = {"https://alpha.example:443", "https://beta.example:443"};
    for (int i = 0; i < 2; i++)
        CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,ipc[i])==0);
    const char *invalid[] = {"https://UPPER.example:443",  "https://a.example/",
                             "https://a.example:0",        "https://a.example:65536",
                             "https://a.example:0443",     "https://a.example.:443",
                             "https://a.example:443/path", "file:///tmp/x"};
    char *testargs[] = {"web-capability-check", "--renderer", (char *)origins[0], NULL};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        errno = 0;
        CHECK(start(OS_WEB_RENDERER,"/bin/web-capability-check",testargs,ipc[0][1],out[1],invalid[i])==-1 && errno==EINVAL);
    }
    errno = 0;
    CHECK(start(OS_WEB_RENDERER,"/bin/hello",testargs,ipc[0][1],out[1],origins[0])==-1 && errno==EPERM);
    for (int i = 0; i < 2; i++) {
        char *args[] = {"web-capability-check", "--renderer", (char *)origins[i], NULL};
        pid[i] = start(OS_WEB_RENDERER, "/bin/web-capability-check", args, ipc[i][1], out[1],
                       origins[i]);
        CHECK(pid[i]>0);
        objects[i] = memfd_create("origin-surface", MFD_ALLOW_SEALING);
        CHECK(objects[i]>=0 && ftruncate(objects[i],4096)==0);
        map[i] = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, objects[i], 0);
        CHECK(map[i]!=MAP_FAILED);
        map[i][0] = 111 + i;
    }

    CHECK(pid[0]!=pid[1]);
    errno = 0;
    CHECK(kill(pid[0],SIGTERM)==-1 && errno==EPERM);
    errno = 0;
    CHECK(kill(getppid(),SIGKILL)==-1 && errno==EPERM);
    errno = 0;
    CHECK(kill(-1,SIGKILL)==-1 && errno==EPERM);
    for (int i = 0; i < 2; i++) {
        struct os_web_identity child;
        CHECK(read(ipc[i][0],&child,sizeof(child))==sizeof(child));
        CHECK(child.process==(unsigned)pid[i] && child.domain==identity.domain && !strcmp(child.origin,origins[i]));
        struct offer offer = {111 + i, pid[!i]};
        int rights[] = {objects[i], net, ca};
        send_caps(ipc[i][0], rights, 3, &offer, sizeof(offer));
    }

    for (int i = 0; i < 2; i++) {
        receive_owned(ipc[i][0], owned[i]);
        CHECK(map[i][1]==(unsigned)pid[i]);
    }
    for (int i = 0; i < 2; i++) {
        struct offer offer = {111 + i, pid[!i]};
        int foreign[] = {objects[!i], ipc[!i][1], owned[!i][0], owned[!i][1]};
        send_caps(ipc[i][0], foreign, 4, &offer, sizeof(offer));
        char reply;
        CHECK(read(ipc[i][0],&reply,1)==1 && reply=='B');
    }

    for (int i = 0; i < 2; i++)
        wait_ok(pid[i]);
    close(out[1]);
    for (int i = 0; i < 2; i++) {
        close(ipc[i][0]);
        close(ipc[i][1]);
        close(objects[i]);
        close(owned[i][0]);
        close(owned[i][1]);
        CHECK(munmap(map[i],4096)==0);
    }

    close(net);
    close(ca);
    puts(
        "PASS immutable origins, separate VMs, denied renderer escapes/foreign signals and cross-origin memory/channel grants");
    int cleanup_ipc[2];
    CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,cleanup_ipc)==0);
    char *cleanup_args[] = {"web-capability-check", "--cleanup-renderer", NULL};
    int cleanup = start(OS_WEB_RENDERER, "/bin/web-capability-check", cleanup_args, cleanup_ipc[1],
                        1, "https://cleanup.example:443");
    CHECK(cleanup>0);
    close(cleanup_ipc[1]);
    char ready;
    CHECK(read(cleanup_ipc[0],&ready,1)==1 && ready=='R');
    CHECK(kill(cleanup,SIGKILL)==0);
    int cleanup_status;
    CHECK(waitpid(cleanup,&cleanup_status,0)==cleanup && WIFSIGNALED(cleanup_status) && WTERMSIG(cleanup_status)==SIGKILL);
    close(cleanup_ipc[0]);
    puts("PASS scoped broker renderer cleanup; foreign and process-group kills denied");
    run_component("/bin/jsc", "/usr/share/modern-browser/engine-check.js");
    run_component("/bin/jsc", "/usr/share/modern-browser/agent-check.js");
    run_component("/bin/glib-check", NULL);
    atomic_store(&stop_worker, 1);
    CHECK(pthread_join(worker,NULL)==0);
    puts(
        "PASS threaded broker launches fresh sandboxed ELF helpers and genuine JavaScriptCore/agents");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(150);
    if (argc >= 2 && !strcmp(argv[1], "--broker"))
        return broker();
    if (argc == 3 && !strcmp(argv[1], "--renderer"))
        return renderer(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--cleanup-renderer")) {
        identify();
        CHECK(identity.role==OS_WEB_RENDERER);
        CHECK(write(0,"R",1)==1);
        for (;;)
            pause();
    }

    identify();
    CHECK(identity.role==OS_WEB_NONE);
    check_pipe_capacity();
    CHECK(setenv("INHERITED_WEB_SECRET","must-not-inherit",1)==0);
    CHECK(mkdir("/home",0755)==0 || errno==EEXIST);
    CHECK(mkdir("/home/user",0700)==0 || errno==EEXIST);
    CHECK(mkdir("/home/user/.config",0700)==0 || errno==EEXIST);
    int secret = open("/tmp/web-private", O_CREAT | O_RDWR | O_EXCL, 0600);
    CHECK(secret>=0);
    CHECK(write(secret,"private",7)==7);
    int ipc[2], out[2];
    CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,ipc)==0 && pipe(out)==0);
    char *args[] = {"web-capability-check", "--broker", NULL};
    int pid = start(OS_WEB_BROKER, "/bin/web-capability-check", args, ipc[1], out[1], NULL);
    CHECK(pid>0);
    close(ipc[1]);
    close(out[1]);
    copy_output(out[0]);
    wait_ok(pid);
    close(ipc[0]);
    close(secret);
    CHECK(unlink("/tmp/web-private")==0);
    puts("WEB CAPABILITY CHECK PASS");
    return 0;
}
