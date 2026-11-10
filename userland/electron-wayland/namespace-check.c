#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/auxv.h>
#include <linux/capability.h>
#include <sched.h>
#include <grp.h>
#include <pthread.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
static int peer=-1;
static pid_t supervisor;
static int single_live_mode;
#define OK(x) do { __atomic_fetch_add(&checks,1,__ATOMIC_RELAXED);if(!(x)){fprintf(stderr,"C2_FAIL line=%d errno=%d check=%s\n",__LINE__,errno,#x);_exit(111);} } while(0)
#define DENY(x,e) do { errno=0;long z=(x);OK(z==-1 && errno==(e)); } while(0)
static void write_exact(const char *path,const char *text) {
    int fd=open(path,O_WRONLY|O_CLOEXEC);OK(fd>=0);
    size_t n=strlen(text);OK(write(fd,text,n)==(ssize_t)n);OK(!close(fd));
}
static void read_exact(const char *path,const char *expected) {
    char text[128];int fd=open(path,O_RDONLY|O_CLOEXEC);OK(fd>=0);
    ssize_t n=read(fd,text,sizeof(text));OK(n==(ssize_t)strlen(expected) && !memcmp(text,expected,n));
    OK(read(fd,text,sizeof(text))==0 && close(fd)==0);
}
static void ids(unsigned id) {
    unsigned a=0,b=0,c=0;OK(getuid()==id && geteuid()==id && getgid()==id && getegid()==id);
    OK(!getresuid(&a,&b,&c) && a==id && b==id && c==id);
    OK(!getresgid(&a,&b,&c) && a==id && b==id && c==id);
}
static uint64_t cap_mask(void) {
    struct __user_cap_header_struct h={.version=_LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct data[2];OK(!syscall(SYS_capget,&h,data));
    return data[0].permitted|((uint64_t)data[1].permitted<<32);
}
static void cap_drop(void) {
    struct __user_cap_header_struct h={.version=_LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct data[2]={{0},{0}};
    OK(!syscall(SYS_capset,&h,data) && !cap_mask());
    data[0].permitted=data[0].effective=1u<<CAP_SYS_ADMIN;
    DENY(syscall(SYS_capset,&h,data),EPERM);OK(!cap_mask());
    DENY(prctl(PR_CAPBSET_DROP,CAP_SYS_ADMIN,0,0,0),EPERM);
}
static uint64_t ns_identity(void) {
    char text[64];ssize_t n=readlink("/proc/self/ns/user",text,sizeof(text)-1);OK(n>7 && n<(ssize_t)sizeof(text));text[n]=0;
    uint64_t id=0;OK(!strncmp(text,"user:[",6) && text[n-1]==']');
    for(ssize_t i=6;i<n-1;i++){OK(text[i]>='0' && text[i]<='9' && id<=(UINT64_MAX-(unsigned)(text[i]-'0'))/10);id=id*10+(text[i]-'0');}
    OK(id);return id;
}
static void map_self(void) {
    write_exact("/proc/self/setgroups","deny");
    write_exact("/proc/self/gid_map","1000 1000 1\n");
    write_exact("/proc/self/uid_map","1000 1000 1\n");
    ids(1000);read_exact("/proc/self/setgroups","deny\n");
    read_exact("/proc/self/uid_map","1000 1000 1\n");read_exact("/proc/self/gid_map","1000 1000 1\n");
    DENY(syscall(SYS_setgroups,0,NULL),EPERM);
}
static void wait_child(pid_t pid) {
    int st=-1;OK(pid>0 && waitpid(pid,&st,0)==pid && WIFEXITED(st) && !WEXITSTATUS(st));
}
static void peer_id(unsigned id,pid_t pid) {
    struct ucred c;socklen_t n=sizeof(c);OK(!getsockopt(peer,SOL_SOCKET,SO_PEERCRED,&c,&n) && n==sizeof(c));
    OK(c.uid==id && c.gid==id && c.pid==pid);
}
static void common_denials(void) {
    int channel[2],enable=1;OK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,channel));
    OK(!setsockopt(channel[0],SOL_SOCKET,SO_PASSCRED,&enable,sizeof(enable)));
    close(channel[0]);close(channel[1]);
    DENY(open("namespace-root-secret",O_RDONLY),EACCES);
    DENY(syscall(SYS_setresuid,0,0,0),EPERM);DENY(syscall(SYS_setresgid,0,0,0),EPERM);
    DENY(kill(supervisor,0),EPERM);OK(!kill(getpid(),0));
    DENY(mount(NULL,"/","tmpfs",0,NULL),EPERM);
    DENY(syscall(402,0,NULL,0),EPERM);
    DENY(syscall(SYS_unshare,CLONE_NEWUSER|CLONE_NEWNET),ENOTSUP);
    DENY(syscall(SYS_setns,-1,CLONE_NEWUSER),ENOSYS);
    DENY(syscall(SYS_seccomp,0,0,NULL),ENOTSUP);
    OK(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));OK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);
    DENY(access("/proc/self/ns/pid",F_OK),ENOENT);DENY(access("/proc/self/ns/net",F_OK),ENOENT);
}
static void probe_child(void) {
    ids(65534);uint64_t first=ns_identity();OK(first!=1);
    read_exact("/proc/self/uid_map","");read_exact("/proc/self/gid_map","");
    peer_id(65534,supervisor);common_denials();
    map_self();peer_id(65534,supervisor);cap_drop();
    OK(!syscall(SYS_unshare,CLONE_NEWUSER));ids(65534);OK(ns_identity()!=first);
    _exit(0);
}
static int running,stop,drop_in_thread,context_pressure;
static void *worker(void *unused) {
    (void)unused;
    if(drop_in_thread){OK(cap_mask()!=0);cap_drop();DENY(syscall(SYS_unshare,CLONE_NEWUSER),EPERM);}
    __atomic_fetch_add(&running,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&stop,__ATOMIC_ACQUIRE)) {
        if(context_pressure)syscall(SYS_getuid);
        else usleep(1000);
    }
    return NULL;
}
static void threaded_probe(void) {
    context_pressure=1;
    pthread_t thread;OK(!pthread_create(&thread,NULL,worker,NULL));
    while(!__atomic_load_n(&running,__ATOMIC_ACQUIRE))usleep(1000);
    struct stat st;OK(!stat("/proc/self/task",&st) && st.st_nlink==4);
    pid_t p=-1;for(unsigned i=0;i<100;i++){p=syscall(SYS_clone,CLONE_NEWUSER|SIGCHLD,NULL,NULL,NULL,0);if(p>=0 || errno!=EAGAIN)break;usleep(1000);}
    if(!p)probe_child();
    wait_child(p);
    ids(1000);OK(ns_identity()==1 && !cap_mask());
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);OK(!pthread_join(thread,NULL));usleep(10000);
    context_pressure=0;
    OK(!stat("/proc/self/task",&st) && st.st_nlink==3);
    puts("C2_THREADED_PARENT_REAL_TWO_LEVEL_PROBE_PASS");
}
static void quotas(void) {
    int gate[2],ack[2];OK(!pipe(gate) && !pipe(ack));pid_t pids[31];
    for(unsigned i=0;i<31;i++) {
        pid_t p=syscall(SYS_clone,CLONE_NEWUSER|SIGCHLD,NULL,NULL,NULL,0);
        if(!p){
            close(gate[1]);close(ack[0]);
            if(!i){map_self();OK(!syscall(SYS_unshare,CLONE_NEWUSER));}
            OK(write(ack[1],"Y",1)==1);char c;OK(read(gate[0],&c,1)==1);_exit(0);
        }
        OK(p>0);pids[i]=p;char c;OK(read(ack[0],&c,1)==1 && c=='Y');
    }
    DENY(syscall(SYS_unshare,CLONE_NEWUSER),ENOSPC);ids(1000);OK(ns_identity()==1);
    DENY(syscall(SYS_clone,CLONE_NEWUSER|SIGCHLD,NULL,NULL,NULL,0),ENOSPC);
    OK(!kill(pids[30],SIGKILL));int st;OK(waitpid(pids[30],&st,0)==pids[30] && WIFSIGNALED(st));
    pid_t p=syscall(SYS_clone,CLONE_NEWUSER|SIGCHLD,NULL,NULL,NULL,0);if(!p)_exit(0);wait_child(p);
    char bytes[30];memset(bytes,'Y',sizeof(bytes));OK(write(gate[1],bytes,sizeof(bytes))==sizeof(bytes));
    for(unsigned i=0;i<30;i++)wait_child(pids[i]);
    close(gate[0]);close(gate[1]);close(ack[0]);close(ack[1]);
    puts("C2_SHARED_32_NAMESPACE_32_PROCESS_BOUND_AND_REFUND_PASS");

    __atomic_store_n(&running,0,__ATOMIC_RELEASE);__atomic_store_n(&stop,0,__ATOMIC_RELEASE);
    pthread_attr_t attr;OK(!pthread_attr_init(&attr) && !pthread_attr_setstacksize(&attr,65536));pthread_t threads[205],extra;
    for(unsigned i=0;i<205;i++)OK(!pthread_create(&threads[i],&attr,worker,NULL));
    while(__atomic_load_n(&running,__ATOMIC_ACQUIRE)!=205)usleep(1000);
    struct stat s;OK(!stat("/proc/self/task",&s) && s.st_nlink==208);
    int e=pthread_create(&extra,&attr,worker,NULL);OK(e==ENOSPC || e==EAGAIN);
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);
    for(unsigned i=0;i<205;i++)OK(!pthread_join(threads[i],NULL));
    OK(!pthread_attr_destroy(&attr));usleep(10000);
    OK(!stat("/proc/self/task",&s) && s.st_nlink==3);
    puts("C2_SHARED_206_TASK_BOUND_THREAD_METADATA_AND_REFUND_PASS");
}
static void send_right(int fd) {
    char byte='N';struct iovec io={&byte,1};char buf[CMSG_SPACE(sizeof(int))]={0};
    struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=buf,.msg_controllen=sizeof(buf)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&m);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&fd,sizeof(fd));
    OK(sendmsg(peer,&m,0)==1);
}
static void full_namespace(void) {
    OK(!syscall(SYS_unshare,CLONE_NEWUSER));ids(65534);
    OK(!syscall(SYS_setgroups,0,NULL));
    struct __user_cap_header_struct h={.version=_LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct caps[2];OK(!syscall(SYS_capget,&h,caps));OK(cap_mask()==((1ULL<<41)-1));
    DENY(syscall(SYS_capget,(void *)1,caps),EFAULT);
    DENY(syscall(SYS_capget,&h,(void *)1),EFAULT);
    h.pid=supervisor;DENY(syscall(SYS_capget,&h,caps),EPERM);h.pid=0;
    h.version=1;OK(!syscall(SYS_capget,&h,NULL) && h.version==_LINUX_CAPABILITY_VERSION_3);
    OK(prctl(PR_CAPBSET_READ,CAP_SYS_ADMIN,0,0,0)==1);
    OK(prctl(PR_CAPBSET_READ,CAP_SYS_ADMIN,0xdead,0xbeef,0x1234)==1);
    OK(!prctl(PR_CAPBSET_DROP,CAP_SYS_ADMIN,0xdead,0xbeef,0x1234));OK(!prctl(PR_CAPBSET_READ,CAP_SYS_ADMIN,0xdead,0xbeef,0x1234));
    DENY(prctl(PR_CAPBSET_READ,41,0,0,0),EINVAL);
    int uid=open("/proc/self/uid_map",O_WRONLY),gid=open("/proc/self/gid_map",O_WRONLY);OK(uid>=0 && gid>=0);
    DENY(write(gid,"1000 1000 1\n",12),EPERM);
    DENY(write(uid,"0 1000 1\n",9),EPERM);
    DENY(write(uid,"1000 0 1\n",9),EPERM);
    DENY(write(uid,"1000 1000 2\n",12),EPERM);
    DENY(write(uid,"1000 1000 1\n1001 1001 1\n",24),EPERM);
    DENY(write(uid,"1000",4),EPERM);
    char large[65];memset(large,'1',sizeof(large));DENY(write(uid,large,sizeof(large)),EINVAL);
    OK(lseek(uid,1,SEEK_SET)==1);DENY(write(uid,"1000 1000 1\n",12),EINVAL);OK(!lseek(uid,0,SEEK_SET));
    DENY(syscall(SYS_write,uid,(void *)1,12),EFAULT);
    map_self();common_denials();
    DENY(write(uid,"1000 1000 1\n",12),EPERM);DENY(write(gid,"1000 1000 1\n",12),EPERM);
    struct stat st;OK(!stat("namespace-root-secret",&st) && st.st_uid==65534 && st.st_gid==65534);
    OK(!stat("/proc/self/uid_map",&st) && st.st_uid==1000);
    int pair[2];OK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));struct ucred c;socklen_t len=sizeof(c);
    OK(!getsockopt(pair[0],SOL_SOCKET,SO_PEERCRED,&c,&len) && c.uid==1000 && c.pid==getpid());close(pair[0]);close(pair[1]);

    drop_in_thread=1;__atomic_store_n(&stop,0,__ATOMIC_RELEASE);__atomic_store_n(&running,0,__ATOMIC_RELEASE);
    pthread_t t;OK(!pthread_create(&t,NULL,worker,NULL));while(!__atomic_load_n(&running,__ATOMIC_ACQUIRE))usleep(1000);
    OK(cap_mask()==((1ULL<<41)-1));DENY(syscall(SYS_unshare,CLONE_NEWUSER),EPERM);
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);OK(!pthread_join(t,NULL));usleep(10000);
    puts("C2_PER_THREAD_CAP_DROP_NO_PARENT_OR_OUTER_AUTHORITY_PASS");
    char *argv[]={"/bin/namespace-check","--exec",NULL};pid_t p=fork();if(!p){execv(argv[0],argv);_exit(112);}wait_child(p);
    OK(cap_mask()==((1ULL<<41)-1));
    char *bad[]={"namespace-bad-elf",NULL};DENY(execv(bad[0],bad),ENOEXEC);OK(cap_mask()==((1ULL<<41)-1));
    int old=open("/proc/self/uid_map",O_RDONLY);OK(old>=0);uint64_t first=ns_identity();
    for(unsigned depth=1;depth<4;depth++) {
        cap_drop();OK(!syscall(SYS_unshare,CLONE_NEWUSER));ids(65534);OK(ns_identity()!=first);map_self();
    }
    uint64_t last=ns_identity();DENY(syscall(SYS_unshare,CLONE_NEWUSER),ENOSPC);OK(ns_identity()==last);ids(1000);
    char text[64];OK(read(old,text,sizeof(text))==12 && !memcmp(text,"1000 1000 1\n",12));close(old);
    OK(!lseek(uid,0,SEEK_SET));DENY(write(uid,"1000 1000 1\n",12),EPERM);close(uid);close(gid);
    int ns=open("/proc/self/ns/user",O_RDONLY);OK(ns>=0);send_right(ns);close(ns);
    cap_drop();common_denials();
    puts("C2_MAPPING_DAC_OBSERVER_IDS_EXEC_DEPTH_STALE_FD_AND_RIGHTS_PASS");
}
static void subject(int basic) {
    checks=0;
    OK(!syscall(SYS_setgroups,0,NULL) && !syscall(SYS_setresgid,1000,1000,1000) && !syscall(SYS_setresuid,1000,1000,1000));ids(1000);
    peer_id(0,supervisor);OK(ns_identity()==1 && !cap_mask());
    if(single_live_mode) {
        pthread_t t;OK(!pthread_create(&t,NULL,worker,NULL));
        while(!__atomic_load_n(&running,__ATOMIC_ACQUIRE))usleep(1000);
        __atomic_store_n(&stop,1,__ATOMIC_RELEASE);OK(!pthread_join(t,NULL));
        /* A joined/zombie thread's retained reference is not a live user. */
        OK(!syscall(SYS_unshare,CLONE_NEWUSER));ids(65534);map_self();
        int fd=open("/proc/self/ns/user",O_RDONLY);OK(fd>=0);send_right(fd);close(fd);
        puts("C2_SINGLE_LIVE_PRIVATE_UNSHARE_AFTER_JOIN_PASS");_exit(0);
    }
    threaded_probe();
    if(!basic)quotas();
    full_namespace();
    printf("C2_NAMESPACE_FOUNDATION_PASS subject_checks=%u full_quotas=%d outer_identity1000_scoped_caps_no_sandbox_bypass\n",checks,!basic);
    _exit(0);
}
int main(int argc,char **argv) {
    struct utsname os;if(uname(&os)||strcmp(os.sysname,"Coast"))return 2;
    setvbuf(stdout,NULL,_IONBF,0);alarm(150);
    if(argc==2 && !strcmp(argv[1],"--exec")) {
        ids(1000);OK(!cap_mask());OK(getauxval(AT_UID)==1000 && getauxval(AT_EUID)==1000 && getauxval(AT_GID)==1000 && getauxval(AT_EGID)==1000);
        DENY(syscall(SYS_setresuid,0,0,0),EPERM);puts("C2_EXEC_IDS_AUXV_AND_NO_CAP_REGAIN_PASS");return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--off")) {
        struct __user_cap_header_struct h={.version=_LINUX_CAPABILITY_VERSION_3};struct __user_cap_data_struct c[2];
        DENY(syscall(SYS_capget,&h,c),ENOSYS);DENY(syscall(SYS_unshare,CLONE_NEWUSER),ENOSYS);
        DENY(syscall(SYS_clone,CLONE_NEWUSER|SIGCHLD,NULL,NULL,NULL,0),ENOTSUP);
        OK(!getuid());puts("C2_DEFAULT_OFF_PASS no_namespace_or_capability_transition");return 0;
    }
    int basic=argc==2 && !strcmp(argv[1],"--basic");
    single_live_mode=argc==2 && !strcmp(argv[1],"--single-live");
    if(argc!=1 && !basic && !single_live_mode)return 2;
    OK(!getuid());supervisor=getpid();
    struct __user_cap_header_struct h={.version=_LINUX_CAPABILITY_VERSION_3};struct __user_cap_data_struct cap[2];
    DENY(syscall(SYS_capget,&h,cap),EPERM);DENY(syscall(SYS_unshare,CLONE_NEWUSER),EPERM);
    OK(!mkdir("/home",0755) || errno==EEXIST);OK(!mkdir("/home/port-test",0700) || errno==EEXIST);
    OK(!chown("/home/port-test",1000,1000) && !chmod("/home/port-test",0700) && !chdir("/home/port-test"));
    int fd=open("namespace-root-secret",O_CREAT|O_TRUNC|O_WRONLY,0600);OK(fd>=0 && write(fd,"keep",4)==4 && !close(fd));
    fd=open("namespace-bad-elf",O_CREAT|O_TRUNC|O_WRONLY,0700);OK(fd>=0 && write(fd,"bad",3)==3 && !fchown(fd,1000,1000) && !close(fd));
    int pair[2],in[2],out[2];OK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair) && !pipe(in) && !pipe(out));
    pid_t p=fork();OK(p>=0);
    if(!p) {
        OK(dup2(in[0],0)==0 && dup2(out[1],1)==1 && dup2(out[1],2)==2 && dup2(pair[1],3)==3);
        for(int i=4;i<128;i++)close(i);
        peer=3;subject(basic);
    }
    close(pair[1]);close(in[0]);close(in[1]);close(out[1]);
    for(;;){char b[4096];ssize_t n=read(out[0],b,sizeof(b));if(n<=0)break;OK(write(1,b,n)==n);}close(out[0]);wait_child(p);
    char byte;struct iovec io={&byte,1};char control[CMSG_SPACE(sizeof(int))];struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    OK(recvmsg(pair[0],&m,0)==1 && byte=='N' && (m.msg_flags&MSG_CTRUNC) && !CMSG_FIRSTHDR(&m));close(pair[0]);
    OK(!getuid() && !getgid());puts("C2_ROOT_PARENT_UNCHANGED_AND_PROC_RIGHT_IMPORT_DENIED_PASS");return 0;
}
