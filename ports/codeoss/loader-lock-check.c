#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/utsname.h>
#include <string.h>
static unsigned long long now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))exit(3);return (unsigned long long)t.tv_sec*1000000+t.tv_nsec/1000;}
static void run(const char *target){
 int gate[2],dummy[2];if(pipe(gate)||pipe(dummy))exit(3);
 if(fcntl(dummy[0],F_SETFL,O_NONBLOCK)<0)exit(3);
 pid_t child=fork();if(child<0)exit(3);
 if(!child){close(gate[1]);char c;if(read(gate[0],&c,1)!=1)_exit(3);close(gate[0]);close(dummy[0]);close(dummy[1]);int null=open("/dev/null",O_RDWR);if(null<0)_exit(3);for(int i=0;i<3;i++)if(dup2(null,i)<0)_exit(3);if(null>2)close(null);char *args[]={(char*)target,"--loader-probe",NULL};execv(target,args);_exit(127);}
 close(gate[0]);if(write(gate[1],"x",1)!=1)exit(3);close(gate[1]);
 unsigned long long peak=0,clock_peak=0;unsigned attempts=0;int status=0;unsigned long long deadline=now()+10000000;
 for(;;){unsigned long long a=now(),b=now();if(b-a>clock_peak)clock_peak=b-a;char byte;a=now();ssize_t n=read(dummy[0],&byte,1);b=now();if(b-a>peak)peak=b-a;if(n>=0||errno!=EAGAIN)exit(4);attempts++;pid_t result=waitpid(child,&status,WNOHANG);if(result==child)break;if(result<0||now()>deadline){kill(child,SIGKILL);waitpid(child,&status,0);exit(5);}struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
 close(dummy[0]);close(dummy[1]);printf("LOADER_LOCK target=%s attempts=%u empty_nonblock_read_peak_us=%llu clock_pair_peak_us=%llu exited=%d status=%d\n",target,attempts,peak,clock_peak,WIFEXITED(status),WIFEXITED(status)?WEXITSTATUS(status):-1);
 if(!WIFEXITED(status)||WEXITSTATUS(status)!=(strcmp(target,"/bin/hello")?1:0))exit(6);
}
int main(void){struct utsname os;if(uname(&os)||strcmp(os.sysname,"Coast")){fputs("Coast guest only\n",stderr);return 2;}run("/bin/hello");for(int i=0;i<3;i++)run("/bin/codeoss-model");puts("LOADER_LOCK_CHECK_DONE no JSC context or document initialized; dummy pipe never contains data");return 0;}
