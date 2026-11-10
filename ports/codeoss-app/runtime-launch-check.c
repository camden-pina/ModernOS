#define _POSIX_C_SOURCE 200809L
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
int main(int argc,char **argv){struct utsname os;if(uname(&os)||strcmp(os.sysname,"Coast")||argc<2){fputs("Coast owned-guest launcher only; specify an absolute executable path\n",stderr);return 2;}if(argv[1][0]!='/')return 2;setvbuf(stdout,NULL,_IONBF,0);pid_t child=fork();if(child<0)return 3;if(!child){if(getenv("RUNTIME_LD_DEBUG"))setenv("LD_DEBUG","files,libs",1);execv(argv[1],argv+1);printf("RUNTIME_EXECVE_ERROR errno=%d\n",errno);_exit(125);}int status=0;for(unsigned i=0;i<400;i++){pid_t got=waitpid(child,&status,WNOHANG);if(got==child){printf("RUNTIME_WAIT status=%d exit=%d signal=%d\n",status,WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);return 0;}if(got<0&&errno!=EINTR)return 4;struct timespec t={0,50000000};nanosleep(&t,NULL);}kill(child,SIGKILL);waitpid(child,&status,0);puts("RUNTIME_TIMEOUT owned child killed and reaped");return 1;}
