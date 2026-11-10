#define _GNU_SOURCE
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <poll.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Guest QA only: the editor's ordinary account, NNP and filter before the
 * exact canonical companion exec. Root is only the supervising test process. */
static unsigned long long now(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (unsigned long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int home_dir(const char *path) {
    struct stat st;
    if(mkdir(path,0700) && errno!=EEXIST)return -1;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    int error=fstat(fd,&st) || !S_ISDIR(st.st_mode) || fchown(fd,1000,1000) || fchmod(fd,0700);
    close(fd);return error;
}
int main(int argc,char **argv) {
    struct utsname os;
    if(uname(&os) || strcmp(os.sysname,"Coast") || getuid()!=0 || argc<2 || argc>32)return 2;
    setvbuf(stdout,NULL,_IONBF,0);
    if(mkdir("/home",0755) && errno!=EEXIST)return 3;
    if(home_dir("/home/port-test") || home_dir("/home/port-test/runtime") || chdir("/home/port-test"))return 3;
    if(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0))return 4;
    int input[2],output[2];if(pipe(input) || pipe(output))return 5;
    pid_t child=fork();if(child<0)return 6;
    if(!child) {
        if(dup2(input[0],0)!=0 || dup2(output[1],1)!=1 || dup2(output[1],2)!=2)_exit(110);
        for(int fd=3;fd<256;fd++)close(fd);
        if(syscall(SYS_setgroups,0,NULL) || syscall(SYS_setresgid,1000,1000,1000) ||
           syscall(SYS_setresuid,1000,1000,1000)) {
            printf("NATIVE_NODE_CREDENTIAL_ERROR errno=%d\n",errno);_exit(111);
        }
        if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))_exit(112);
        struct sock_filter code[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,8888,0,1),
            BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|77),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
        struct sock_fprog filter={4,code};
        if(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&filter) ||
           getuid()!=1000 || getgid()!=1000 || prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)!=1 ||
           prctl(PR_GET_SECCOMP,0,0,0,0)!=2)_exit(113);
        unsetenv("NODE_OPTIONS");unsetenv("NODE_PATH");unsetenv("ELECTRON_RUN_AS_NODE");
        unsetenv("LD_PRELOAD");unsetenv("LD_LIBRARY_PATH");
        if(setenv("HOME","/home/port-test",1) || setenv("TMPDIR","/home/port-test/runtime",1) ||
           setenv("UV_THREADPOOL_SIZE","2",1))_exit(114);
        char *args[33]={"/opt/electron-43.7.3/node"};
        for(int i=1;i<argc;i++)args[i]=argv[i];
        puts("NATIVE_NODE_LAUNCH uid=1000 gid=1000 nnp=1 filter=2 canonical=1");
        execv(args[0],args);printf("NATIVE_NODE_EXEC_ERROR errno=%d\n",errno);_exit(115);
    }
    close(input[0]);close(input[1]);close(output[1]);
    unsigned long long start=now(),exited=0;unsigned helpers=0;
    int status=0,reaped=0,complete=0,timed_out=0,eof=0,io_failed=0,truncated=0;
    size_t captured=0;const size_t output_limit=1024u*1024;
    while(!reaped || !complete || !eof) {
        struct pollfd event={eof?-1:output[0],POLLIN,0};
        if(poll(&event,1,10)>0 && (event.revents&(POLLIN|POLLHUP))) {
            char buffer[4096];ssize_t count=read(output[0],buffer,sizeof(buffer));
            if(!count)eof=1;
            else if(count<0 && errno!=EINTR){eof=1;io_failed=1;}
            else if(count>0) {
                size_t remaining=output_limit-captured;
                size_t visible=(size_t)count<remaining?(size_t)count:remaining;
                ssize_t at=0;
                while(at<(ssize_t)visible) {
                    ssize_t written=write(1,buffer+at,visible-at);
                    if(written<0 && errno==EINTR)continue;
                    if(written<=0){io_failed=1;break;}
                    at+=written;
                }
                captured+=visible;
                if(visible<(size_t)count && !truncated){puts("NATIVE_NODE_OUTPUT_TRUNCATED limit=1048576 draining=1");truncated=1;}
            }
        }
        if(!reaped && waitpid(child,&status,WNOHANG)==child){reaped=1;exited=now();}
        if(reaped) {
            int helper_status;pid_t helper;
            while((helper=waitpid(-1,&helper_status,WNOHANG))>0)helpers++;
            complete=helper<0 && errno==ECHILD;
            if((!complete || !eof) && now()-exited>10000){timed_out=1;break;}
        }
        if(!reaped && now()-start>90000){kill(child,SIGKILL);waitpid(child,&status,0);reaped=1;exited=now();timed_out=1;}
        struct timespec pause={0,10000000};nanosleep(&pause,NULL);
    }
    close(output[0]);
    printf("NATIVE_NODE_WAIT status=%d exit=%d signal=%d descendants_complete=%u helpers=%u timeout=%u truncated=%u bytes=%zu\n",
        status,WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0,
        complete,helpers,timed_out,truncated,captured);
    return timed_out?124:(io_failed||truncated)?126:WIFEXITED(status)?WEXITSTATUS(status):125;
}
