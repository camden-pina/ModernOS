#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static const char *program;
static char parent_pid[32];
static atomic_uint failures;
static atomic_uint retries;
static volatile long shared_result[4];
static _Thread_local unsigned cookie;

static int spawn_retry(pid_t *pid,const char *path,const posix_spawn_file_actions_t *actions,char *const args[]) {
    int error;
    for(unsigned attempt=0;;attempt++) {
        error=posix_spawn(pid,path,actions,NULL,args,environ);
        if(error!=EAGAIN || attempt==2999)return error;
        atomic_fetch_add(&retries,1);
        struct timespec delay={0,10000000};nanosleep(&delay,NULL);
    }
}

static int write_all(int fd, const void *data, size_t size) {
    const char *bytes=data;
    while(size) {
        ssize_t n=write(fd,bytes,size);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return 1;
        bytes+=n;size-=(size_t)n;
    }
    return 0;
}

static void *worker(void *argument) {
    unsigned id=(unsigned)(uintptr_t)argument;
    cookie=0x434f4100u+id;
    char before[256], after[256], number[16];
    int pipefd[2];
    volatile unsigned stage=0;
    if(!getcwd(before,sizeof(before)))goto fail;
    snprintf(number,sizeof(number),"%u",id);
    for(unsigned round=0;round<6;round++) {
        stage=10+round;
        if(pipe2(pipefd,O_CLOEXEC))goto fail;
        posix_spawn_file_actions_t actions;
        if(posix_spawn_file_actions_init(&actions))goto fail_pipe;
        int error=posix_spawn_file_actions_addopen(&actions,0,"/dev/null",O_RDONLY,0);
        if(!error)error=posix_spawn_file_actions_adddup2(&actions,pipefd[1],1);
        if(!error)error=posix_spawn_file_actions_adddup2(&actions,pipefd[1],2);
        if(!error)error=posix_spawn_file_actions_addclose(&actions,pipefd[0]);
        if(!error)error=posix_spawn_file_actions_addclose(&actions,pipefd[1]);
        if(!error)error=posix_spawn_file_actions_addchdir_np(&actions,"/tmp");
        char *args[]={(char *)program,"--child",number,parent_pid,NULL};
        pid_t pid=-1;
        if(!error)error=spawn_retry(&pid,program,&actions,args);
        posix_spawn_file_actions_destroy(&actions);
        close(pipefd[1]);
        if(error){close(pipefd[0]);fprintf(stderr,"worker %u spawn error %d\n",id,error);goto fail;}
        uint64_t sum=0,count=0;
        unsigned char bytes[4096];
        for(;;) {
            ssize_t n=read(pipefd[0],bytes,sizeof(bytes));
            if(n<0 && errno==EINTR)continue;
            if(n<0){close(pipefd[0]);goto fail;}
            if(!n)break;
            count+=(size_t)n;
            for(ssize_t i=0;i<n;i++)sum+=bytes[i];
        }
        close(pipefd[0]);
        int status;
        pid_t waited=waitpid(pid,&status,0);
        if(waited!=pid || !WIFEXITED(status) || WEXITSTATUS(status) ||
           count!=65536 || sum!=32768ULL*(65+id)+32768ULL*(97+id)) {
            fprintf(stderr,"worker %u child=%ld waited=%ld status=%d count=%llu sum=%llu\n",id,
                (long)pid,(long)waited,status,(unsigned long long)count,(unsigned long long)sum);
            goto fail;
        }
        if(!getcwd(after,sizeof(after)) || strcmp(before,after) || cookie!=0x434f4100u+id)goto fail;
        char *missing[]={(char *)"/tmp/coast-no-such-spawn-program",NULL};
        error=spawn_retry(&pid,missing[0],NULL,missing);
        if(error!=ENOENT){fprintf(stderr,"missing exec returned %d\n",error);goto fail;}
    }
    stage=20;errno=0;
    pid_t plain=fork();
    if(!plain)_exit(99);
    if(plain>0){waitpid(plain,NULL,0);goto fail;}
    if(errno!=ENOTSUP && errno!=EAGAIN)goto fail;
    stage=21;shared_result[id]=-123;
    pid_t temporary;
    for(volatile unsigned attempt=0;;attempt++) {
        temporary=vfork();
        if(temporary>=0 || errno!=EAGAIN || attempt==2999)break;
        atomic_fetch_add(&retries,1);
        struct timespec delay={0,10000000};nanosleep(&delay,NULL);
    }
    if(!temporary) {
        shared_result[id]=syscall(SYS_mmap,0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(shared_result[id]==-1)shared_result[id]=-errno;
        _exit(0);
    }
    if(temporary<0 || waitpid(temporary,NULL,0)!=temporary || shared_result[id]!=-ENOTSUP) {
        fprintf(stderr,"worker %u vfork=%ld child mmap=%ld errno=%d\n",id,(long)temporary,shared_result[id],errno);
        goto fail;
    }
    return NULL;
fail_pipe:
    close(pipefd[0]);close(pipefd[1]);
fail:
    fprintf(stderr,"worker %u failed at stage %u errno=%d\n",id,stage,errno);
    atomic_fetch_add(&failures,1);
    return NULL;
}

static unsigned descriptors(void) {
    unsigned count=0;
    for(int fd=0;fd<256;fd++)if(fcntl(fd,F_GETFD)>=0)count++;
    return count;
}

int main(int argc,char **argv) {
    if(argc==4 && !strcmp(argv[1],"--child")) {
        char cwd[256], input;
        if(!getcwd(cwd,sizeof(cwd)) || strcmp(cwd,"/tmp") ||
           getppid()!=(pid_t)atoi(argv[3]) || read(0,&input,1)!=0) {
            fprintf(stderr,"child cwd=%s parent=%ld expected=%s errno=%d\n",cwd,(long)getppid(),argv[3],errno);
            return 2;
        }
        unsigned char data[4096];
        memset(data,'A'+atoi(argv[2]),sizeof(data));
        for(unsigned i=0;i<8;i++)if(write_all(1,data,sizeof(data)))return 3;
        memset(data,'a'+atoi(argv[2]),sizeof(data));
        for(unsigned i=0;i<8;i++)if(write_all(2,data,sizeof(data)))return 4;
        return 0;
    }
    program=argv[0];
    snprintf(parent_pid,sizeof(parent_pid),"%ld",(long)getpid());
    unsigned before=descriptors();
    pthread_t threads[4];
    for(unsigned i=0;i<4;i++)if(pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)i))return 5;
    for(unsigned i=0;i<4;i++)if(pthread_join(threads[i],NULL))return 6;
    int status;
    unsigned after=descriptors();
    pid_t remaining=waitpid(-1,&status,WNOHANG);
    if(failures || after!=before || remaining!=-1 || errno!=ECHILD) {
        fprintf(stderr,"failures=%u descriptors=%u/%u remaining=%ld errno=%d\n",
            (unsigned)failures,before,after,(long)remaining,errno);
        return 7;
    }
    puts("NATIVE_SPAWN_PASS parallel workers, stdout/stderr bytes, cwd, TLS, parent identity, failed exec, vfork VM guard and cleanup");
    printf("Temporary EAGAIN retries=%u\n",(unsigned)retries);
    return 0;
}
