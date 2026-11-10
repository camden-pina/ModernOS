#define _GNU_SOURCE
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static const char *self;
#define REQUIRE(condition) do { if(!(condition)) { \
 fprintf(stderr,"CHECK_FAILED line=%d errno=%d\n",__LINE__,errno); return 1; } } while(0)

static int install(unsigned number,unsigned action,unsigned flags) {
    struct sock_filter instructions[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,number,0,1),
        BPF_STMT(BPF_RET|BPF_K,action),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program={.len=sizeof(instructions)/sizeof(instructions[0]),.filter=instructions};
    return syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,flags,&program);
}
static int nnp(void) {return prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0);}
static int not_privileged(void) {
    REQUIRE(getuid()!=0 && geteuid()!=0);
    REQUIRE(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==0);
    errno=0;REQUIRE(install(SYS_getppid,SECCOMP_RET_ERRNO|EBUSY,0)==-1 && errno==EACCES);
    return 0;
}
static int query_and_validation(void) {
    unsigned action=SECCOMP_RET_ERRNO;
    REQUIRE(syscall(SYS_seccomp,SECCOMP_GET_ACTION_AVAIL,0,&action)==0);
    action=0xbeef0000;errno=0;
    REQUIRE(syscall(SYS_seccomp,SECCOMP_GET_ACTION_AVAIL,0,&action)==-1 && errno==EOPNOTSUPP);
    action=SECCOMP_RET_ERRNO|EBUSY;errno=0;
    REQUIRE(syscall(SYS_seccomp,SECCOMP_GET_ACTION_AVAIL,0,&action)==-1 && errno==EOPNOTSUPP);
    REQUIRE(nnp()==0);
    struct sock_filter invalid[]={BPF_STMT(BPF_LD|BPF_W|BPF_MEM,0),BPF_STMT(BPF_RET|BPF_A,0)};
    struct sock_fprog program={.len=2,.filter=invalid};
    errno=0;REQUIRE(syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,0,&program)==-1 && errno==EINVAL);
    invalid[0]=(struct sock_filter)BPF_STMT(BPF_JMP|BPF_JA,99);
    errno=0;REQUIRE(syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,0,&program)==-1 && errno==EINVAL);
    program.len=0;errno=0;
    REQUIRE(syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,0,&program)==-1 && errno==EINVAL);
    return 0;
}
static int deny_and_stack(void) {
    REQUIRE(nnp()==0 && install(SYS_getppid,SECCOMP_RET_ERRNO|EBUSY,0)==0);
    errno=0;REQUIRE(syscall(SYS_getppid)==-1 && errno==EBUSY);
    REQUIRE(syscall(SYS_getpid)>0);
    REQUIRE(prctl(PR_GET_SECCOMP)==2);
    REQUIRE(install(SYS_getppid,SECCOMP_RET_ERRNO|EACCES,0)==0);
    errno=0;REQUIRE(syscall(SYS_getppid)==-1 && errno==EACCES);
    REQUIRE(install(SYS_getuid,SECCOMP_RET_ERRNO|EPERM,0)==0);
    errno=0;REQUIRE(syscall(SYS_getuid)==-1 && errno==EPERM);
    errno=0;REQUIRE(prctl(PR_SET_SECCOMP,SECCOMP_MODE_DISABLED)==-1 && errno==EINVAL);
    return 0;
}
static int inherit_fork_exec(void) {
    REQUIRE(nnp()==0 && install(SYS_getppid,SECCOMP_RET_ERRNO|EBUSY,0)==0);
    pid_t child=fork();REQUIRE(child>=0);
    if(!child) {
        errno=0;if(syscall(SYS_getppid)!=-1 || errno!=EBUSY)_exit(91);
        execl(self,self,"--inherited",NULL);_exit(92);
    }
    int status;REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    return 0;
}
static volatile sig_atomic_t trapped;
static void trap(int signal,siginfo_t *info,void *context) {
    if(signal==SIGSYS && info->si_code==SYS_SECCOMP && info->si_syscall==SYS_getppid &&
       info->si_arch==AUDIT_ARCH_X86_64)trapped=1;
    ((ucontext_t *)context)->uc_mcontext.gregs[REG_RAX]=777;
}
static int trap_case(void) {
    struct sigaction action={.sa_sigaction=trap,.sa_flags=SA_SIGINFO};
    REQUIRE(sigaction(SIGSYS,&action,NULL)==0 && nnp()==0);
    REQUIRE(install(SYS_getppid,SECCOMP_RET_TRAP|42,0)==0);
    REQUIRE(syscall(SYS_getppid)==777 && trapped);
    return 0;
}
static int killed(void) {
    REQUIRE(nnp()==0 && install(SYS_getppid,SECCOMP_RET_KILL_PROCESS,0)==0);
    syscall(SYS_getppid);return 1;
}
static int unknown_action(void) {
    REQUIRE(nnp()==0 && install(SYS_getppid,0xbeef0000,0)==0);
    syscall(SYS_getppid);return 1;
}
static int strict_success(void) {
    REQUIRE(syscall(SYS_seccomp,SECCOMP_SET_MODE_STRICT,0,NULL)==0);
    syscall(SYS_exit,0);return 1;
}
static int strict_failure(void) {
    REQUIRE(syscall(SYS_seccomp,SECCOMP_SET_MODE_STRICT,0,NULL)==0);
    syscall(SYS_getpid);return 1;
}
static atomic_int phase,thread_result;
static void *thread_probe(void *ignored) {
    (void)ignored;
    while(!atomic_load_explicit(&phase,memory_order_acquire))sched_yield();
    errno=0;
    atomic_store(&thread_result,syscall(SYS_getppid)==-1 && errno==EBUSY);
    return NULL;
}
static int tsync_case(void) {
    pthread_t thread;REQUIRE(pthread_create(&thread,NULL,thread_probe,NULL)==0);
    REQUIRE(nnp()==0 && install(SYS_getppid,SECCOMP_RET_ERRNO|EBUSY,SECCOMP_FILTER_FLAG_TSYNC)==0);
    atomic_store_explicit(&phase,1,memory_order_release);
    REQUIRE(pthread_join(thread,NULL)==0 && atomic_load(&thread_result)==1);
    return 0;
}
static int native_spawn(void) {
    REQUIRE(nnp()==0 && install(SYS_getppid,SECCOMP_RET_ERRNO|EBUSY,0)==0);
    char command[256];snprintf(command,sizeof(command),"%s --inherited",self);
    execl("/bin/coast-sh","coast-sh","-c",command,NULL);
    return 1;
}
#ifdef COAST_GUEST
/* Native extension numbers from include/uapi/abi.h, intentionally separate
   from libc's Linux SYS_* namespace. */
enum {COAST_spawn=400,COAST_spawn_io=404,COAST_spawnve=405,
      COAST_spawn_browser=407,COAST_spawn_web=409,COAST_power=413};
static int native_spawn_exec_denial(void) {
    REQUIRE(nnp()==0 && install(SYS_execve,SECCOMP_RET_ERRNO|EBUSY,0)==0);
    const char *args[]={self,"--inherited",NULL};
    unsigned native_calls[]={COAST_spawn,COAST_spawn_io,COAST_spawnve,COAST_spawn_browser,COAST_spawn_web};
    for(unsigned i=0;i<sizeof(native_calls)/sizeof(native_calls[0]);i++) {
        errno=0;REQUIRE(syscall(native_calls[i],self,args,NULL,0)==-1 && errno==EBUSY);
    }
    return 0;
}
static int native_spawn_clone_denial(void) {
    REQUIRE(nnp()==0 && install(SYS_clone,SECCOMP_RET_ERRNO|EBUSY,0)==0);
    const char *args[]={self,"--inherited",NULL};
    errno=0;REQUIRE(syscall(COAST_spawn,self,args)==-1 && errno==EBUSY);
    return 0;
}
static int native_power_denial(void) {
    REQUIRE(nnp()==0 && install(SYS_reboot,SECCOMP_RET_ERRNO|EBUSY,0)==0);
    /* Invalid flags keep the probe harmless even if filtering regresses. */
    errno=0;REQUIRE(syscall(COAST_power,1,0,1)==-1 && errno==EBUSY);return 0;
}
#endif
int main(int argc,char **argv) {
    self=argv[0];
    if(argc==2 && !strcmp(argv[1],"--inherited")) {
        errno=0;REQUIRE(syscall(SYS_getppid)==-1 && errno==EBUSY);
        REQUIRE(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1 && prctl(PR_GET_SECCOMP)==2);
        return 0;
    }
    struct {const char *name;int (*body)(void);int signal;} cases[]={
        {"unprivileged filters require no_new_privs",not_privileged,0},
        {"action discovery and invalid BPF rejection",query_and_validation,0},
        {"errno actions, stacking and irreversibility",deny_and_stack,0},
        {"fork and exec inherit filters and no_new_privs",inherit_fork_exec,0},
        {"SIGSYS carries Linux seccomp metadata and restores handler result",trap_case,0},
        {"kill-process action terminates with SIGSYS",killed,SIGSYS},
        {"unknown BPF return action fails closed with SIGSYS",unknown_action,SIGSYS},
        {"strict mode permits the exit syscall",strict_success,0},
        {"strict mode kills forbidden syscalls",strict_failure,SIGKILL},
        {"TSYNC attaches the filter to existing real pthreads",tsync_case,0},
#ifdef COAST_GUEST
        {"native Coast spawn cannot escape an inherited filter",native_spawn,0},
        {"every native spawn obeys Linux execve denial",native_spawn_exec_denial,0},
        {"native spawn obeys Linux clone denial",native_spawn_clone_denial,0},
        {"native power syscall obeys Linux reboot denial",native_power_denial,0},
#endif
    };
#ifndef COAST_GUEST
    (void)native_spawn;
#endif
    unsigned failures=0;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        fflush(NULL);
        pid_t child=fork();if(child<0)return 90;
        if(!child)_exit(cases[i].body());
        int status;if(waitpid(child,&status,0)!=child)return 91;
        int pass=cases[i].signal?(WIFSIGNALED(status) && WTERMSIG(status)==cases[i].signal):
                                  (WIFEXITED(status) && WEXITSTATUS(status)==0);
        printf("%s %s status=%d\n",pass?"PASS":"FAIL",cases[i].name,status);
        failures+=!pass;
    }
    printf("PACKAGE_SECCOMP_%s cases=%zu\n",failures?"FAIL":"PASS",sizeof(cases)/sizeof(cases[0]));
    return failures?1:0;
}
