#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
static unsigned checks;
#define OK(x) do{__atomic_fetch_add(&checks,1,__ATOMIC_RELAXED);if(!(x)){fprintf(stderr,"F1_FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(111);}}while(0)
#define DENY(x,e) do{errno=0;long v_=(x);OK(v_==-1&&errno==(e));}while(0)
static void enable(void){OK(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));}
static long put(struct sock_filter *b,unsigned n,unsigned flags){struct sock_fprog p={.len=n,.filter=b};return syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,flags,&p);}
static void rule(int nr,uint32_t result,unsigned flags){struct sock_filter b[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,nr,0,1),BPF_STMT(BPF_RET|BPF_K,result),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};OK(!put(b,4,flags));}
static void joined(pid_t p,int sig){int s;OK(p>0&&waitpid(p,&s,0)==p);if(sig)OK(WIFSIGNALED(s)&&WTERMSIG(s)==sig);else OK(WIFEXITED(s)&&!WEXITSTATUS(s));}
static void denied(void){DENY(setresuid(0,0,0),EPERM);DENY(setresgid(0,0,0),EPERM);DENY(open("/dev/tty",O_RDWR),EACCES);}
static void basic(void){
 OK(prctl(PR_GET_SECCOMP)==0);struct sock_filter one[]={BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 DENY(put(one,1,0),EACCES);DENY(syscall(SYS_seccomp,1,0,NULL),EFAULT);DENY(syscall(SYS_seccomp,0,0,NULL),ENOTSUP);DENY(syscall(SYS_seccomp,1,2,NULL),EINVAL);
 DENY(prctl(PR_SET_SECCOMP,1,0,0,0),EINVAL);enable();
 struct sock_fprog p={1,one};OK(!prctl(PR_SET_SECCOMP,2,&p,0x1234,0x5678));OK(prctl(PR_GET_SECCOMP)==2);
 DENY(syscall(417,1,NULL,0),EPERM);denied();rule(SYS_getppid,SECCOMP_RET_ERRNO|77,0);DENY(syscall(SYS_getppid),77);
 rule(417,SECCOMP_RET_ERRNO|78,0);DENY(syscall(417,1,NULL,0),78);denied();
 puts("F1_ABI_NATIVE_ALLOW_RETAINS_DENIALS_PASS");
}
static void invalid(void){
 enable();DENY(put(NULL,1,0),EFAULT);struct sock_filter p[4];
 for(unsigned i=0;i<12;i++){
  p[0]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
  unsigned n=1;
  switch(i){
   case 0:p[0].code=0xffff;break;
   case 1:p[0]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_W|BPF_ABS,64);n=2;break;
   case 2:p[0]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_W|BPF_ABS,1);n=2;break;
   case 3:p[0]=(struct sock_filter)BPF_STMT(BPF_JMP|BPF_JA,UINT32_MAX);n=2;break;
   case 4:p[0]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_MEM,0);n=2;break;
   case 5:p[0]=(struct sock_filter)BPF_STMT(BPF_ALU|BPF_DIV|BPF_K,0);n=2;break;
   case 6:p[0]=(struct sock_filter)BPF_STMT(BPF_ALU|BPF_LSH|BPF_K,32);n=2;break;
   case 7:p[0]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_IMM,7);break;
   case 8:p[0]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRACE);break;
   case 9:p[0]=(struct sock_filter)BPF_STMT(BPF_ST,16);n=2;break;
   case 10:p[0]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0,2,0);n=2;break;
   case 11:p[0]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0,0,1);p[1]=(struct sock_filter)BPF_STMT(BPF_ST,0);p[2]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_MEM,0);n=4;break;
  }
  if(n==2)p[1]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
  if(n==4)p[3]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
  DENY(put(p,n,0),EINVAL);OK(prctl(PR_GET_SECCOMP)==0);
 }
 DENY(put(p,0,0),EINVAL);DENY(put(p,4097,0),EINVAL);
 puts("F1_VERIFIER_POINTER_OPCODE_FLOW_SCRATCH_REJECTION_PASS");
}
static void side_effect(void){
 int f=memfd_create("f1-no-effect",0);OK(f>=0&&dup2(f,60)==60);if(f!=60)close(f);enable();
 struct sock_filter p[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_write,0,3),BPF_STMT(BPF_LD|BPF_W|BPF_ABS,16),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,60,0,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EACCES),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 OK(!put(p,6,0));DENY(write(60,"bad",3),EACCES);struct stat s;OK(!fstat(60,&s)&&s.st_size==0);close(60);
 puts("F1_ARGUMENT_ERRNO_NO_SIDE_EFFECT_PASS");
}
static volatile sig_atomic_t traps;
static void trap(int sig,siginfo_t *i,void *v){
 ucontext_t *u=v;OK(sig==SIGSYS);
 if(i->si_code==SYS_SECCOMP){
  OK(i->si_arch==AUDIT_ARCH_X86_64);OK(i->si_call_addr==(void *)u->uc_mcontext.gregs[REG_RIP]);
  OK(i->si_syscall==SYS_getppid||i->si_syscall==SYS_getpgid);OK(i->si_errno==19||i->si_errno==20);traps++;
  if(i->si_syscall==SYS_getppid)OK(syscall(SYS_getpgid,0)==4343);
  u->uc_mcontext.gregs[REG_RAX]=i->si_syscall==SYS_getppid?4242:4343;
 }else{OK(i->si_code==0);i->si_code=SYS_SECCOMP;u->uc_mcontext.gregs[REG_RAX]=9999;}
 /* These forged edits must ALL be ignored; accepting RIP/RSP would crash. */
 u->uc_mcontext.gregs[REG_RIP]=0xffffffffffffffffULL;u->uc_mcontext.gregs[REG_RSP]=0;
 u->uc_mcontext.gregs[REG_EFL]=~0ULL;u->uc_mcontext.gregs[REG_CSGSFS]=0;
 u->uc_mcontext.fpregs=(void *)1;sigfillset(&u->uc_sigmask);
}
static void trap_test(void){
 struct sigaction a={.sa_sigaction=trap,.sa_flags=SA_SIGINFO|SA_NODEFER};sigemptyset(&a.sa_mask);OK(!sigaction(SIGSYS,&a,NULL));enable();
 rule(SYS_getppid,SECCOMP_RET_TRAP|19,0);rule(SYS_getpgid,SECCOMP_RET_TRAP|20,0);rule(SYS_getppid,SECCOMP_RET_ERRNO|99,0);
 OK(syscall(SYS_getppid)==4242&&traps==2);
 OK(!syscall(SYS_tgkill,getpid(),syscall(SYS_gettid),SIGSYS));OK(traps==2);
 OK(syscall(SYS_getppid)==4242&&traps==4);
 puts("F1_AUTHENTIC_NESTED_SIGSYS_RAX_ONLY_FORGERY_REJECTED_PASS");
}
static int instruction_ready,instruction_stop[4];
static void *instruction_worker(void *index){
 unsigned i=(uintptr_t)index;struct sock_filter *p=calloc(4096,sizeof(*p));OK(p);
 for(unsigned n=0;n<4095;n++)p[n]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_IMM,n);
 p[4095]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
 for(;;){long result=put(p,4096,0);if(!result)break;OK(errno==EAGAIN);usleep(1000);}
 free(p);__atomic_fetch_add(&instruction_ready,1,__ATOMIC_RELEASE);
 while(!__atomic_load_n(&instruction_stop[i],__ATOMIC_ACQUIRE))usleep(1000);
 return NULL;
}
static void quota(void){
 enable();pthread_t workers[4];
 for(unsigned i=0;i<4;i++)OK(!pthread_create(&workers[i],NULL,instruction_worker,(void *)(uintptr_t)i));
 while(__atomic_load_n(&instruction_ready,__ATOMIC_ACQUIRE)!=4)usleep(1000);
 struct sock_filter a[]={BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 DENY(put(a,1,0),ENOMEM);OK(prctl(PR_GET_SECCOMP)==0);
 __atomic_store_n(&instruction_stop[0],1,__ATOMIC_RELEASE);OK(!pthread_join(workers[0],NULL));usleep(10000);
 OK(!put(a,1,0));OK(prctl(PR_GET_SECCOMP)==2);
 for(unsigned i=1;i<4;i++){__atomic_store_n(&instruction_stop[i],1,__ATOMIC_RELEASE);OK(!pthread_join(workers[i],NULL));}
 puts("F1_TOTAL_INSTRUCTION_BOUND_PASS");
}
static void depth(void){enable();for(unsigned i=0;i<4;i++)rule(SYS_getppid,SECCOMP_RET_ERRNO|(70+i),0);DENY(syscall(SYS_getppid),73);struct sock_filter p[]={BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};DENY(put(p,1,0),ENOMEM);DENY(syscall(SYS_getppid),73);puts("F1_OBJECT_DEPTH_INTERSECTION_NEWEST_DATA_PASS");}
static int ready,phase,incompatible;
static pid_t worker_tid;
static void *worker(void *unused){
 (void)unused;worker_tid=syscall(SYS_gettid);OK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==0);
 if(incompatible){enable();rule(SYS_getppid,SECCOMP_RET_ERRNO|81,0);}
 __atomic_store_n(&ready,1,__ATOMIC_RELEASE);while(!__atomic_load_n(&phase,__ATOMIC_ACQUIRE))usleep(1000);
 if(incompatible){DENY(syscall(SYS_getppid),81);OK(prctl(PR_GET_SECCOMP)==2);}
 else{OK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);DENY(syscall(SYS_getppid),80);}
 return NULL;
}
static void *untouched(void *unused){(void)unused;while(!__atomic_load_n(&phase,__ATOMIC_ACQUIRE))usleep(1000);OK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==0);OK(prctl(PR_GET_SECCOMP)==0);return NULL;}
static void sync_case(int bad){
 incompatible=bad;pthread_t p,third;OK(!pthread_create(&p,NULL,worker,NULL));if(bad)OK(!pthread_create(&third,NULL,untouched,NULL));while(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE))usleep(1000);enable();
 struct sock_filter b[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getppid,0,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|80),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 long result=put(b,4,SECCOMP_FILTER_FLAG_TSYNC);
 if(bad){OK(result==worker_tid);OK(prctl(PR_GET_SECCOMP)==0);OK(syscall(SYS_getppid)>0);}
 else{OK(!result);DENY(syscall(SYS_getppid),80);}
 __atomic_store_n(&phase,1,__ATOMIC_RELEASE);OK(!pthread_join(p,NULL));if(bad)OK(!pthread_join(third,NULL));
 printf("F1_TSYNC_%s_PASS\n",bad?"INCOMPATIBLE_ATOMIC_ROLLBACK":"NNP_AND_FILTER_PROPAGATION");
}
static void *kill_worker(void *unused){(void)unused;rule(SYS_getppid,SECCOMP_RET_KILL_THREAD,0);rule(SYS_getppid,SECCOMP_RET_ERRNO|99,0);syscall(SYS_getppid);_exit(113);}
static void kills(void){enable();pthread_t p;OK(!pthread_create(&p,NULL,kill_worker,NULL));OK(!pthread_join(p,NULL));OK(syscall(SYS_getppid)>0);puts("F1_KILL_THREAD_PRESERVES_SIBLING_PASS");}
static void inheritance(void){
 enable();rule(SYS_getppid,SECCOMP_RET_ERRNO|82,0);struct sock_filter allow[]={BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};OK(!put(allow,1,0));DENY(syscall(SYS_getppid),82);
 pid_t p=fork();OK(p>=0);if(!p){DENY(syscall(SYS_getppid),82);char *a[]={"seccomp-check","--exec",NULL};execve("/bin/seccomp-check",a,environ);_exit(112);}joined(p,0);
 p=vfork();if(!p){char *a[]={"seccomp-check","--exec",NULL};execve("/bin/seccomp-check",a,environ);_exit(112);}joined(p,0);
 char *a[]={"seccomp-check","--exec",NULL};joined(syscall(400,"/bin/seccomp-check",a,1),0);joined(syscall(405,"/bin/seccomp-check",a,environ,1),0);
 struct {int32_t fd[3];uint32_t reserved;uint64_t cwd;} io={{0,1,2},0,(uintptr_t)"/home/port-test"};joined(syscall(404,"/bin/seccomp-check",a,&io),0);
 char *bad[]={"bad",NULL};DENY(execve("/not-there",bad,environ),ENOENT);DENY(syscall(SYS_getppid),82);
 p=fork();OK(p>=0);if(!p){OK(!syscall(SYS_unshare,CLONE_NEWUSER));DENY(syscall(SYS_getppid),82);_exit(0);}joined(p,0);
 puts("F1_FORK_VFORK_NATIVE_SPAWN_EXEC_NAMESPACE_INHERITANCE_PASS");
}
static void alarm_filter(int sig){(void)sig;rule(SYS_read,SECCOMP_RET_ERRNO|83,0);}
static void restart(void){enable();int p[2];OK(!pipe(p));struct sigaction a={.sa_handler=alarm_filter,.sa_flags=SA_RESTART};sigemptyset(&a.sa_mask);OK(!sigaction(SIGALRM,&a,NULL));alarm(1);char c;DENY(read(p[0],&c,1),83);close(p[0]);close(p[1]);puts("F1_RESTART_REENTERS_FILTER_PASS");}

static void arithmetic(void){
 enable();struct sock_filter p[40];unsigned n=0;
 #define S(c,k) p[n++]=(struct sock_filter)BPF_STMT(c,k)
 S(BPF_LD|BPF_W|BPF_ABS,0);p[n++]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getppid,0,0);
 S(BPF_LD|BPF_IMM,7);S(BPF_ST,0);S(BPF_LD|BPF_IMM,3);S(BPF_MISC|BPF_TAX,0);S(BPF_LD|BPF_MEM,0);
 S(BPF_ALU|BPF_ADD|BPF_X,0);S(BPF_ALU|BPF_MUL|BPF_K,10);S(BPF_ALU|BPF_DIV|BPF_K,4);S(BPF_ALU|BPF_MOD|BPF_K,7);
 S(BPF_ALU|BPF_OR|BPF_K,8);S(BPF_ALU|BPF_AND|BPF_K,15);S(BPF_ALU|BPF_LSH|BPF_K,1);S(BPF_ALU|BPF_RSH|BPF_X,0);
 S(BPF_ALU|BPF_SUB|BPF_K,1);S(BPF_ALU|BPF_XOR|BPF_K,1);S(BPF_ALU|BPF_NEG,0);S(BPF_ALU|BPF_ADD|BPF_K,94);
 S(BPF_ST,1);S(BPF_LDX|BPF_MEM,1);S(BPF_MISC|BPF_TXA,0);S(BPF_ALU|BPF_OR|BPF_K,SECCOMP_RET_ERRNO);S(BPF_RET|BPF_A,0);
 S(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);p[1].jf=n-3;OK(!put(p,n,0));DENY(syscall(SYS_getppid),91);
 struct sock_filter q[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,4),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
 BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getpgid,0,4),BPF_STMT(BPF_LD|BPF_W|BPF_ABS,20),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x1234,0,2),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|92),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 OK(!put(q,10,0));DENY(syscall(SYS_getpgid,0x123400000000ULL),92);rule(8888,SECCOMP_RET_ERRNO|93,0);DENY(syscall(8888),93);
 struct sock_filter shift[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,8889,0,6),BPF_STMT(BPF_LD|BPF_IMM,47),BPF_STMT(BPF_LDX|BPF_IMM,32),BPF_STMT(BPF_ALU|BPF_LSH|BPF_X,0),BPF_STMT(BPF_ALU|BPF_ADD|BPF_K,47),BPF_STMT(BPF_ALU|BPF_OR|BPF_K,SECCOMP_RET_ERRNO),BPF_STMT(BPF_RET|BPF_A,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 OK(!put(shift,9,0));DENY(syscall(8889),94);
 puts("F1_ARITHMETIC_SCRATCH_ARCH_64BIT_ARGUMENT_UNKNOWN_SYSCALL_PASS");
 #undef S
}
static int race_stop,race_ready;
static void *race_worker(void *unused){(void)unused;__atomic_fetch_add(&race_ready,1,__ATOMIC_RELEASE);while(!__atomic_load_n(&race_stop,__ATOMIC_ACQUIRE)){errno=0;long v=syscall(SYS_getppid);OK(v>0||(v==-1&&errno==89));usleep(1000);}OK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);DENY(syscall(SYS_getppid),89);return NULL;}
static void *race_creator(void *unused){(void)unused;for(unsigned i=0;i<20;i++){pthread_t p;OK(!pthread_create(&p,NULL,race_worker,NULL));usleep(1000);__atomic_store_n(&race_stop,1,__ATOMIC_RELEASE);OK(!pthread_join(p,NULL));}return NULL;}
static void racing(void){
 pthread_t a,b;OK(!pthread_create(&a,NULL,race_worker,NULL));while(!__atomic_load_n(&race_ready,__ATOMIC_ACQUIRE))usleep(1000);enable();
 rule(SYS_getppid,SECCOMP_RET_ERRNO|89,SECCOMP_FILTER_FLAG_TSYNC);OK(!pthread_create(&b,NULL,race_creator,NULL));
 for(unsigned i=0;i<3;i++){rule(SYS_getppid,SECCOMP_RET_ERRNO|89,SECCOMP_FILTER_FLAG_TSYNC);usleep(1000);}
 __atomic_store_n(&race_stop,1,__ATOMIC_RELEASE);OK(!pthread_join(a,NULL)&&!pthread_join(b,NULL));DENY(syscall(SYS_getppid),89);
 puts("F1_TSYNC_ENTRY_CREATE_EXIT_LIFETIME_RACE_PASS");
}
static void overflow_trap(int sig,siginfo_t *info,void *ctx){(void)sig;(void)info;(void)ctx;syscall(SYS_getppid);_exit(117);}
static void bad_frame(int sig,siginfo_t *info,void *ctx){(void)sig;(void)info;(void)ctx;__asm__ volatile("mov $15,%%rax; xor %%rsp,%%rsp; syscall; ud2":::"rax","memory");__builtin_unreachable();}
static void *group_killer(void *unused){(void)unused;rule(SYS_getppid,SECCOMP_RET_KILL_PROCESS,0);syscall(SYS_getppid);_exit(118);}


static int object_ready,object_stop[32];
static void *object_worker(void *index){unsigned i=(uintptr_t)index;struct sock_filter p[]={BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};for(;;){long v=put(p,1,0);if(!v)break;OK(errno==EAGAIN);usleep(1000);}__atomic_fetch_add(&object_ready,1,__ATOMIC_RELEASE);while(!__atomic_load_n(&object_stop[i],__ATOMIC_ACQUIRE))usleep(1000);return NULL;}
static void objects(void){enable();pthread_t p[32];for(unsigned i=0;i<32;i++)OK(!pthread_create(&p[i],NULL,object_worker,(void *)(uintptr_t)i));while(__atomic_load_n(&object_ready,__ATOMIC_ACQUIRE)!=32)usleep(1000);struct sock_filter allow[]={BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};DENY(put(allow,1,0),ENOMEM);OK(prctl(PR_GET_SECCOMP)==0);__atomic_store_n(&object_stop[0],1,__ATOMIC_RELEASE);OK(!pthread_join(p[0],NULL));usleep(10000);OK(!put(allow,1,0));for(unsigned i=1;i<32;i++){__atomic_store_n(&object_stop[i],1,__ATOMIC_RELEASE);OK(!pthread_join(p[i],NULL));}puts("F1_DOMAIN_GLOBAL_OBJECT_LIMIT_REFUND_AND_SLOT_REUSE_PASS");}


static void *zero_divisor_worker(void *kind){struct sock_filter p[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getppid,0,4),BPF_STMT(BPF_LD|BPF_IMM,123),BPF_STMT(BPF_LDX|BPF_IMM,0),BPF_STMT(BPF_ALU|BPF_DIV|BPF_X,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|95),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};if(kind)p[4].code=BPF_ALU|BPF_MOD|BPF_X;OK(!put(p,7,0));syscall(SYS_getppid);_exit(120);}
static void zero_divisors(void){enable();pthread_t p;for(unsigned i=0;i<2;i++){OK(!pthread_create(&p,NULL,zero_divisor_worker,(void *)(uintptr_t)i));OK(!pthread_join(p,NULL));OK(syscall(SYS_getppid)>0);}puts("F1_CLASSIC_DIV_MOD_ZERO_RETURNS_THREAD_KILL_ONLY_PASS");}


static void branches(void){enable();struct sock_filter p[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,8887,0,16),BPF_STMT(BPF_LD|BPF_W|BPF_LEN,0),BPF_STMT(BPF_LDX|BPF_IMM,64),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_X,0,1,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,64,1,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),BPF_JUMP(BPF_JMP|BPF_JGT|BPF_K,63,1,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K,64,1,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),BPF_STMT(BPF_STX,0),BPF_STMT(BPF_LD|BPF_MEM,0),BPF_STMT(BPF_JMP|BPF_JA,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),BPF_STMT(BPF_ALU|BPF_OR|BPF_K,SECCOMP_RET_ERRNO),BPF_STMT(BPF_RET|BPF_A,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};OK(!put(p,19,0));DENY(syscall(8887),64);puts("F1_CONDITIONAL_X_GE_GT_SET_JA_LEN_STX_PASS");}

static int query_ready,query_stop;
static pid_t query_tid;
static void *query_worker(void *unused) {
 (void)unused;int policy=-1;struct sched_param param={.sched_priority=-1};
 OK(!pthread_getschedparam(pthread_self(),&policy,&param));
 OK(policy==SCHED_OTHER && param.sched_priority==0);
 query_tid=syscall(SYS_gettid);__atomic_store_n(&query_ready,1,__ATOMIC_RELEASE);
 while(!__atomic_load_n(&query_stop,__ATOMIC_ACQUIRE))usleep(1000);
 return NULL;
}
static void scheduling_queries(void) {
 const char *frequency_path="/sys/devices/system/cpu/cpu0/tsc_freq_khz";
 int fd=open(frequency_path,O_RDONLY|O_CLOEXEC);OK(fd>=0);
 struct stat metadata;OK(!fstat(fd,&metadata) && metadata.st_uid==0 && (metadata.st_mode&0777)==0444);
 char frequency[32]={0};ssize_t bytes=read(fd,frequency,3);OK(bytes==3);
 ssize_t rest=read(fd,frequency+bytes,sizeof(frequency)-bytes-1);OK(rest>0);
 bytes+=rest;OK(bytes==metadata.st_size && frequency[bytes-1]=='\n');
 OK(read(fd,frequency+bytes,1)==0);
 unsigned long long khz=0;
 for(ssize_t i=0;i<bytes-1;i++) {
  OK(frequency[i]>='0' && frequency[i]<='9' && khz<=(UINT64_MAX-9)/10);
  khz=khz*10+(unsigned)(frequency[i]-'0');
 }
 OK(khz>0 && khz<=UINT64_MAX/105);
 OK(lseek(fd,1,SEEK_SET)==1 && read(fd,frequency,1)==1);
 OK(!close(fd));errno=0;OK(open(frequency_path,O_WRONLY)==-1 && (errno==EROFS || errno==EACCES));
 errno=0;OK(mkdir("/sys/devices/system/cpu/cpu0/new",0700)==-1 && (errno==EROFS || errno==EACCES));
 struct timespec raw_before,raw_after,resolution,monotonic;
 unsigned low,high;__asm__ volatile("lfence; rdtsc":"=a"(low),"=d"(high)::"memory");
 uint64_t ticks_before=((uint64_t)high<<32)|low;
 OK(!clock_gettime(CLOCK_MONOTONIC_RAW,&raw_before));
 OK(!clock_gettime(CLOCK_MONOTONIC,&monotonic));
 OK(raw_before.tv_sec<=monotonic.tv_sec);
 OK(!clock_getres(CLOCK_MONOTONIC_RAW,&resolution));
 OK(resolution.tv_sec==0 && resolution.tv_nsec==1000);
 OK(!clock_getres(CLOCK_MONOTONIC_RAW,NULL));
 DENY(syscall(SYS_clock_gettime,CLOCK_MONOTONIC_RAW,NULL),EFAULT);
 DENY(syscall(SYS_clock_gettime,-1,&raw_after),EINVAL);
 usleep(10000);OK(!clock_gettime(CLOCK_MONOTONIC_RAW,&raw_after));
 __asm__ volatile("lfence; rdtsc":"=a"(low),"=d"(high)::"memory");
 uint64_t ticks_after=((uint64_t)high<<32)|low;
 OK(raw_after.tv_sec>raw_before.tv_sec ||
    (raw_after.tv_sec==raw_before.tv_sec && raw_after.tv_nsec>raw_before.tv_nsec));
 uint64_t elapsed=(raw_after.tv_sec-raw_before.tv_sec)*1000000000ULL+raw_after.tv_nsec-raw_before.tv_nsec;
 uint64_t measured_khz=(ticks_after-ticks_before)*1000000ULL/elapsed;
 OK(measured_khz>khz*95/100 && measured_khz<khz*105/100);
 printf("TSC_METADATA_CALIBRATION_READONLY_PASS khz=%llu measured_khz=%llu\n",khz,(unsigned long long)measured_khz);
 struct sched_param param={.sched_priority=-1};
 OK(sched_getscheduler(0)==SCHED_OTHER);
 OK(!sched_getparam(0,&param) && param.sched_priority==0);
 OK(sched_getscheduler(getpid())==SCHED_OTHER);
 OK(!sched_getparam(syscall(SYS_gettid),&param) && !param.sched_priority);
 DENY(syscall(SYS_sched_getscheduler,-1),EINVAL);
 DENY(syscall(SYS_sched_getparam,-1,&param),EINVAL);
 DENY(syscall(SYS_sched_getscheduler,0x7fffffff),ESRCH);
 DENY(syscall(SYS_sched_getparam,0x7fffffff,&param),ESRCH);
 DENY(syscall(SYS_sched_getparam,0,NULL),EFAULT);
 pthread_t worker;OK(!pthread_create(&worker,NULL,query_worker,NULL));
 while(!__atomic_load_n(&query_ready,__ATOMIC_ACQUIRE))usleep(1000);
 int policy=-1;OK(!pthread_getschedparam(worker,&policy,&param));
 OK(policy==SCHED_OTHER && !param.sched_priority);
 OK(sched_getscheduler(query_tid)==SCHED_OTHER);
 __atomic_store_n(&query_stop,1,__ATOMIC_RELEASE);OK(!pthread_join(worker,NULL));
 DENY(syscall(SYS_sched_getparam,query_tid,&param),ESRCH);
 enable();rule(SYS_sched_getparam,SECCOMP_RET_ERRNO|77,0);
 DENY(syscall(SYS_sched_getparam,0,&param),77);
 OK(sched_getscheduler(0)==SCHED_OTHER);
 puts("SCHEDULER_QUERY_SELF_THREAD_ERRORS_FILTER_PASS");
}

static void subject(const char *name){
 OK(!syscall(SYS_setresgid,1000,1000,1000)&&!syscall(SYS_setresuid,1000,1000,1000));OK(getuid()==1000&&getgid()==1000);
 if(!strcmp(name,"scheduler")){scheduling_queries();printf("F1_CASE_PASS scheduler checks=%u\n",checks);return;}
 if(!strcmp(name,"off")){DENY(prctl(PR_GET_SECCOMP),ENOSYS);DENY(syscall(SYS_seccomp,1,0,NULL),ENOSYS);puts("F1_DEFAULT_OFF_INSIDE_C1_DOMAIN_PASS");}
 else if(!strcmp(name,"basic"))basic();else if(!strcmp(name,"invalid"))invalid();else if(!strcmp(name,"side-effect"))side_effect();else if(!strcmp(name,"trap"))trap_test();else if(!strcmp(name,"quota"))quota();else if(!strcmp(name,"depth"))depth();else if(!strcmp(name,"sync"))sync_case(0);else if(!strcmp(name,"incompatible"))sync_case(1);else if(!strcmp(name,"thread-kill"))kills();else if(!strcmp(name,"inherit"))inheritance();else if(!strcmp(name,"restart"))restart();else if(!strcmp(name,"branches"))branches();else if(!strcmp(name,"zero-divisor"))zero_divisors();else if(!strcmp(name,"objects"))objects();else if(!strcmp(name,"math"))arithmetic();else if(!strcmp(name,"race"))racing();
 else if(!strcmp(name,"trap-overflow")||!strcmp(name,"trap-badframe")){enable();struct sigaction a={.sa_sigaction=!strcmp(name,"trap-overflow")?overflow_trap:bad_frame,.sa_flags=SA_SIGINFO|SA_NODEFER};sigemptyset(&a.sa_mask);OK(!sigaction(SIGSYS,&a,NULL));rule(SYS_getppid,SECCOMP_RET_TRAP|19,0);syscall(SYS_getppid);_exit(119);}
 else if(!strcmp(name,"group-kill")){enable();pthread_t p;OK(!pthread_create(&p,NULL,group_killer,NULL));for(;;)usleep(1000);}
 else if(!strcmp(name,"kill")||!strcmp(name,"trap-default")||!strcmp(name,"trap-ignore")||!strcmp(name,"trap-blocked")||!strcmp(name,"dynamic-unknown")){
  enable();if(!strcmp(name,"trap-ignore"))signal(SIGSYS,SIG_IGN);
  if(!strcmp(name,"trap-blocked")){sigset_t s;sigemptyset(&s);sigaddset(&s,SIGSYS);OK(!sigprocmask(SIG_BLOCK,&s,NULL));}
  if(!strcmp(name,"dynamic-unknown")){struct sock_filter b[]={BPF_STMT(BPF_LD|BPF_IMM,0x12340000),BPF_STMT(BPF_RET|BPF_A,0)};OK(!put(b,2,0));}
  else rule(SYS_getppid,!strcmp(name,"kill")?SECCOMP_RET_KILL_PROCESS:SECCOMP_RET_TRAP|19,0);
  syscall(SYS_getppid);_exit(114);
 }else _exit(115);
 printf("F1_CASE_PASS %s checks=%u\n",name,checks);
}
int main(int argc,char **argv){
 struct utsname u;if(uname(&u)||strcmp(u.sysname,"Coast"))return 2;setvbuf(stdout,NULL,_IONBF,0);alarm(90);
 if(argc==2&&!strcmp(argv[1],"--exec")){OK(getuid()==1000&&prctl(PR_GET_SECCOMP)==2);DENY(syscall(SYS_getppid),82);OK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);puts("F1_EXEC_INHERITED_PASS");return 0;}
 OK(getuid()==0);DENY(syscall(SYS_seccomp,1,0,NULL),EFAULT);
 if(argc==2&&!strcmp(argv[1],"--root-off")){puts("F1_DEFAULT_OFF_ROOT_PASS");return 0;}
 OK(!mkdir("/home",0755)||errno==EEXIST);OK(!mkdir("/home/port-test",0700)||errno==EEXIST);OK(!chown("/home/port-test",1000,1000)&&!chmod("/home/port-test",0700)&&!chdir("/home/port-test"));
 const char *cases[]={"basic","invalid","side-effect","trap","quota","depth","sync","incompatible","thread-kill","inherit","restart","kill","trap-default","trap-ignore","trap-blocked","dynamic-unknown","math","race","trap-overflow","trap-badframe","group-kill","objects","zero-divisor","branches"};
 unsigned count=sizeof(cases)/sizeof(*cases);if(argc==2)count=1;
 for(unsigned k=0;k<count;k++){
  const char *name=argc==2?argv[1]:cases[k];int in[2],out[2];OK(!pipe(in)&&!pipe(out));pid_t p=fork();OK(p>=0);
  if(!p){OK(dup2(in[0],0)==0&&dup2(out[1],1)==1&&dup2(out[1],2)==2);for(int fd=3;fd<128;fd++)close(fd);subject(name);_exit(0);}
  close(in[0]);close(in[1]);close(out[1]);char buf[4096];ssize_t n;while((n=read(out[0],buf,sizeof(buf)))>0)OK(write(1,buf,n)==n);close(out[0]);
  int sig=(!strcmp(name,"kill")||!strncmp(name,"trap-",5)||!strcmp(name,"dynamic-unknown")||!strcmp(name,"group-kill"))?SIGSYS:0;joined(p,sig);printf("F1_ROOT_CASE_COMPLETE %s expected_signal=%d\n",name,sig);
 }
 puts("F1_SECCOMP_BOUNDED_SUITE_PASS");return 0;
}
