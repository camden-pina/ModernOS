#define _GNU_SOURCE
#include <alpm.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define REQUIRE(c) do {if(!(c)){fprintf(stderr,"ALPM_CHECK_FAILED line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
static const char *cache="/tmp/coast-alpm-cache";
static const char *outside="/tmp/coast-alpm-outside";
static int child(alpm_handle_t *handle) {
    REQUIRE(alpm_sandbox_setup_child(handle,"alpm",cache,1)==0);
    REQUIRE(getuid()==990 && geteuid()==990 && getgid()==990 && getegid()==990 && getgroups(0,NULL)==0);
    REQUIRE(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1 && prctl(PR_GET_SECCOMP)==2);
    errno=0;REQUIRE(syscall(SYS_personality,~0UL)<0 && errno==EPERM);
    int fd=open("/tmp/coast-alpm-cache/download",O_CREAT|O_WRONLY|O_TRUNC,0600);
    REQUIRE(fd>=0 && write(fd,"test",4)==4);close(fd);
    errno=0;REQUIRE(open(outside,O_WRONLY|O_TRUNC)<0 && errno==EACCES);
    errno=0;REQUIRE(unlink(outside)<0 && errno==EACCES);
    const char *args[]={"/bin/sh","-c","exit 0",NULL};
    errno=0;REQUIRE(execv(args[0],(char *const *)args)<0 && errno==EACCES);
    return 0;
}
int main(void) {
    REQUIRE(getuid()==0 && geteuid()==0);
    REQUIRE(mkdir(cache,0700)==0 && chown(cache,990,990)==0);
    int fd=open(outside,O_CREAT|O_WRONLY|O_TRUNC,0666);REQUIRE(fd>=0);close(fd);
    REQUIRE(chmod(outside,0666)==0 && chown(outside,990,990)==0);
    alpm_errno_t error;alpm_handle_t *handle=alpm_initialize("/","/var/lib/pacman/",&error);REQUIRE(handle);
    pid_t pid=fork();REQUIRE(pid>=0);if(!pid)_exit(child(handle));
    int status;REQUIRE(waitpid(pid,&status,0)==pid);
    alpm_release(handle);
    unlink("/tmp/coast-alpm-cache/download");rmdir(cache);unlink(outside);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status)==0);
    puts("PACKAGE_ALPM_SANDBOX_PASS real_uid=990 groups=0 nnp=1 seccomp=2 landlock=3");return 0;
}
