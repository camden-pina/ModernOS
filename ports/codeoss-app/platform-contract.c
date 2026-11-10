#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
static void result(const char *name,long value,int error){printf("{\"probe\":\"%s\",\"result\":%ld,\"errno\":%d}\n",name,value,error);}
static void fd_result(const char *name,int fd){int e=fd<0?errno:0;result(name,fd>=0?0:-1,e);if(fd>=0)close(fd);}
static void path_probe(const char *name,const char *path){struct stat st;errno=0;int e=stat(path,&st);result(name,e,e<0?errno:0);}
static void reserve(const char *name,size_t bytes,int prot){errno=0;void *p=mmap(NULL,bytes,prot,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);int e=p==MAP_FAILED?errno:0;result(name,p==MAP_FAILED?-1:0,e);if(p!=MAP_FAILED)munmap(p,bytes);}
static void *worker(void *p){*(int *)p=73;return NULL;}
int main(int argc,char **argv){struct utsname os;if(uname(&os)||strcmp(os.sysname,"Coast")){fputs("Coast disposable guest only\n",stderr);return 2;}setvbuf(stdout,NULL,_IONBF,0);puts("CODEOSS_APP_CONTRACT_BEGIN");printf("{\"probe\":\"identity\",\"uid\":%u,\"euid\":%u,\"os\":\"%s\"}\n",getuid(),geteuid(),os.sysname);
 reserve("reserve_4GiB_PROT_NONE",4ULL*1024*1024*1024,PROT_NONE);reserve("anonymous_RWX_denied",4096,PROT_READ|PROT_WRITE|PROT_EXEC);
 errno=0;void *code=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);int e=code==MAP_FAILED?-1:mprotect(code,4096,PROT_READ|PROT_EXEC);result("private_RW_to_RX",e,e<0?errno:0);if(code!=MAP_FAILED)munmap(code,4096);
 errno=0;fd_result("inotify_init1",inotify_init1(IN_CLOEXEC|IN_NONBLOCK));errno=0;fd_result("timerfd_create",timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC|TFD_NONBLOCK));sigset_t mask;sigemptyset(&mask);errno=0;fd_result("signalfd4",signalfd(-1,&mask,SFD_CLOEXEC|SFD_NONBLOCK));errno=0;fd_result("eventfd",eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK));
 errno=0;long q=syscall(SYS_prctl,39,0,0,0,0);result("PR_GET_NO_NEW_PRIVS",q,q<0?errno:0);errno=0;q=syscall(SYS_prctl,21,0,0,0,0);result("PR_GET_SECCOMP",q,q<0?errno:0);unsigned action=0x7fff0000;errno=0;q=syscall(SYS_seccomp,2,0,&action);result("SECCOMP_GET_ACTION_AVAIL",q,q<0?errno:0);
 int pair[2];errno=0;e=socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair);result("AF_UNIX_socketpair",e,e<0?errno:0);if(!e){char c='x',d=0;e=write(pair[0],&c,1)==1&&read(pair[1],&d,1)==1&&d==c;result("AF_UNIX_transfer",e?0:-1,e?0:errno);close(pair[0]);close(pair[1]);}
 errno=0;int mem=syscall(SYS_memfd_create,"codeoss-app-contract",3);result("memfd_create",mem<0?-1:0,mem<0?errno:0);
 if(mem>=0){errno=0;e=ftruncate(mem,4096);char *a=MAP_FAILED,*b=MAP_FAILED;if(!e){a=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,mem,0);b=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,mem,0);}e=a!=MAP_FAILED&&b!=MAP_FAILED;if(e){a[0]='A';e=b[0]=='A';b[1]='B';e=e&&a[1]=='B';}result("memfd_shared_alias",e?0:-1,e?0:errno);if(a!=MAP_FAILED)munmap(a,4096);if(b!=MAP_FAILED)munmap(b,4096);close(mem);}
 pthread_t thread;int value=0;errno=0;e=pthread_create(&thread,NULL,worker,&value);if(!e)e=pthread_join(thread,NULL);result("pthread_create_join",e? -1:value==73?0:-1,e);
 path_probe("glibc_interpreter_present","/lib64/ld-linux-x86-64.so.2");path_probe("musl_interpreter_present","/lib/ld-musl-x86_64.so.1");path_probe("proc_self_status","/proc/self/status");path_probe("dev_shm","/dev/shm");path_probe("X11_display_socket","/tmp/.X11-unix/X0");path_probe("Wayland_display_socket","/run/user/0/wayland-0");path_probe("DRI_device_directory","/dev/dri");if(argc>1){pid_t child=fork();if(!child){char *args[]={argv[1],"--version",NULL};execv(argv[1],args);result("official_codium_ELF_execve",-1,errno);_exit(0);}if(child<0)result("execve_probe_fork",-1,errno);else{int status=0;unsigned i;for(i=0;i<200;i++){pid_t got=waitpid(child,&status,WNOHANG);if(got==child)break;if(got<0&&errno!=EINTR)break;struct timespec delay={0,50000000};nanosleep(&delay,NULL);}if(i==200){kill(child,SIGKILL);waitpid(child,&status,0);result("upstream_exec_timeout",-1,ETIMEDOUT);}else result("upstream_child_wait_status",status,0);}}
 puts("CODEOSS_APP_CONTRACT_END");return 0;}
