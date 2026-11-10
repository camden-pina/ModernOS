/* QA-image-only /bin/code replacement. Enter the same explicit root frontend
 * transition as the real Code OSS launcher, then run the fixed C2 fixture.
 * This is never installed in the browser product image. */
#include <libc.h>
#include <uapi/account.h>
int main(void) {
    if(call0(SYS_geteuid)!=0)return 1;
    struct os_account_request request={.uid=call0(SYS_getuid)};
    if(call2(SYS_account,OS_ACCOUNT_CODE_LAUNCH,&request))return 2;
    for(unsigned fd=3;fd<OS_FD_MAX;fd++)close(fd);
    char *args[]={"seccomp-check",NULL};
    char *environment[]={"PATH=/usr/bin:/bin","HOME=/root","LANG=C.UTF-8",NULL};
    return call3(SYS_execve,"/bin/seccomp-check",args,environment)<0?3:0;
}
