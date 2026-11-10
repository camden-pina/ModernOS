#define _GNU_SOURCE
#include "config.h"
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int forward_all(const char *bytes,size_t size) {
    while(size) {
        ssize_t count=write(1,bytes,size);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return -1;
        bytes+=count;size-=count;
    }
    return 0;
}
static int private_dir(const char *p) {
    struct stat st;
    if(mkdir(p,0700) && errno!=EEXIST)return -1;
    int fd=open(p,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    int error=fstat(fd,&st) || !S_ISDIR(st.st_mode) || (st.st_uid!=0 && st.st_uid!=1000) ||
              fchown(fd,1000,1000) || fchmod(fd,0700);
    close(fd);return error;
}
static int personal_dir(const char *p) {
    struct stat st;
    if(mkdir(p,0755) && errno!=EEXIST)return -1;
    int fd=open(p,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    /* Migrate old root-owned personal folders, while keeping an existing
       user's group and directory mode. A launch must not chmod Projects. */
    int error=fstat(fd,&st) || !S_ISDIR(st.st_mode) || (st.st_uid!=0 && st.st_uid!=1000);
    if(!error && st.st_uid==0)error=fchown(fd,1000,1000);
    close(fd);return error;
}
/* The boot RAMFS imports archive files as root. Adopt only this private
   cache's bounded, no-follow tree; never follow links or change an inode
   shared with a path outside the cache. Existing user modes stay intact. */
static int adopt_cache(int fd,unsigned depth,unsigned *remaining) {
    struct stat st;
    if(fstat(fd,&st) || !S_ISDIR(st.st_mode) || (st.st_uid!=0 && st.st_uid!=1000)) {close(fd);return -1;}
    if(st.st_uid==0 && (fchown(fd,1000,1000) || fchmod(fd,0700))) {close(fd);return -1;}
    DIR *directory=fdopendir(fd);
    if(!directory){close(fd);return -1;}
    struct dirent *entry;int error=0;
    while((entry=readdir(directory))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        if(!*remaining){error=-1;break;}--*remaining;
        if(fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)) {error=-1;break;}
        if(!S_ISREG(st.st_mode) && !(S_ISDIR(st.st_mode) && depth))continue;
        int child=openat(fd,entry->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|
                         (S_ISDIR(st.st_mode)?O_DIRECTORY:0));
        if(child<0){error=-1;break;}
        if(S_ISDIR(st.st_mode)) {if(adopt_cache(child,depth-1,remaining)){error=-1;break;}}
        else {
            if(fstat(child,&st) || !S_ISREG(st.st_mode) ||
               (st.st_uid==0 && (st.st_nlink!=1 || fchown(child,1000,1000) || fchmod(child,0600))))error=-1;
            close(child);if(error)break;
        }
    }
    closedir(directory);return error;
}
int main(int argc,char **argv) {
    struct utsname os;if(uname(&os) || strcmp(os.sysname,"Coast")){fputs("C1 guest-only launcher\n",stderr);return 2;}
    setvbuf(stdout,NULL,_IONBF,0);
    if(getuid()!=0 || argc<2 || argc>32)return 2;
    unsigned nnp=0,filter=0;int start_arg=1;
    if(!strcmp(argv[start_arg],"--nnp=1")){nnp=1;start_arg++;}else if(!strcmp(argv[start_arg],"--nnp=0"))start_arg++;
    if(start_arg<argc&&!strcmp(argv[start_arg],"--filter")){filter=1;start_arg++;if(!nnp)return 2;}
    int browser=start_arg<argc&&!strcmp(argv[start_arg],"--browser");
    int workbench=browser || (start_arg<argc&&!strcmp(argv[start_arg],"--workbench"));
    if(workbench)start_arg++;
    else alarm(80);
    if(start_arg>=argc)return 2;
    if(!nnp || !filter)return 2;
    int type=0;struct ucred peer={0};socklen_t length=sizeof(type);
    if(getsockopt(3,SOL_SOCKET,SO_TYPE,&type,&length) || length!=sizeof(type) || type!=SOCK_STREAM)return 118;
    length=sizeof(peer);
    if(getsockopt(3,SOL_SOCKET,SO_PEERCRED,&peer,&length) || length!=sizeof(peer) ||
       peer.pid!=getppid() || peer.uid!=0 || peer.gid!=0)return 119;
    struct sockaddr_un address={0};length=sizeof(address);
    if(getpeername(3,(struct sockaddr*)&address,&length) || address.sun_family!=AF_UNIX || length!=sizeof(sa_family_t))return 120;
    printf("ELECTRON_DISPLAY_FD_VERIFIED fd=3 unnamed=1 peer_parent=%d creator_uid=%u\n",peer.pid,peer.uid);
    if(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)) {
        fprintf(stderr,"C1_SUBREAPER_ERROR errno=%d\n",errno);return 121;
    }

    unsigned uid;if(syscall(SYS_getresuid,&uid,&uid,&uid) || uid)return 3;
    if(mkdir("/home",0755) && errno!=EEXIST)return 4;
    const char *home="/home/port-test";
    if(private_dir(home) || private_dir("/home/port-test/tmp") ||
       private_dir("/home/port-test/.config") || private_dir("/home/port-test/.cache") ||
       private_dir("/home/port-test/runtime") ||
       (workbench && (private_dir("/home/port-test/.cache/codeoss-compile") ||
                      private_dir("/home/port-test/.cache/fontconfig"))) || chdir(home))return 5;
    if(workbench && (personal_dir("/home/user") || personal_dir("/home/user/Projects")))return 5;
    if(browser) {
        /* glibc, c-ares and Chromium may cache the initial resolver state.
           Give the real DHCP worker time to publish its configured servers
           before Chromium's network threads start. Offline launch is bounded. */
        uint64_t deadline=now()+10000;
        for(;;) {
            int resolver=open("/etc/resolv.conf",O_RDONLY|O_CLOEXEC);
            char text[160];ssize_t size=resolver<0?-1:read(resolver,text,sizeof(text)-1);
            if(resolver>=0)close(resolver);
            if(size>0){text[size]=0;if(strstr(text,"nameserver "))break;}
            if(now()>=deadline)break;
            struct timespec pause={.tv_nsec=100000000};nanosleep(&pause,NULL);
        }
    }
    if(workbench && !browser) {
        const char *caches[]={"/home/port-test/.cache/codeoss-compile","/home/port-test/.cache/fontconfig"};
        for(unsigned i=0;i<sizeof(caches)/sizeof(*caches);i++) {
            int cache=open(caches[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            unsigned remaining=1024;
            if(cache<0 || adopt_cache(cache,1,&remaining))fputs("CODEOSS_CACHE_ADOPTION_SKIPPED\n",stderr);
        }
    }
    int input[2],output[2];if(pipe(input) || pipe(output))return 6;
    pid_t child=fork();if(child<0)return 7;
    if(!child) {
        if(dup2(input[0],0)!=0 || dup2(output[1],1)!=1 || dup2(output[1],2)!=2)_exit(110);
        for(int i=4;i<COAST_FD_LIMIT;i++)close(i);
        if(syscall(SYS_setgroups,0,NULL) || syscall(SYS_setresgid,1000,1000,1000) ||
           syscall(SYS_setresuid,1000,1000,1000)) {
            fprintf(stderr,"C1_DROP_ERROR errno=%d\n",errno);_exit(111);
        }
        if(getuid()!=1000 || geteuid()!=1000 || getgid()!=1000 || getegid()!=1000)_exit(112);
        if(workbench && !browser) {
            char *project=realpath(argv[argc-1],NULL);
            /* Resolve and enter the folder after dropping privileges. Normal
               directory permissions govern browsing, as in a Linux editor. */
            if(!project || chdir(project)) {
                fprintf(stderr,"CODEOSS_PROJECT_DENIED path=%s errno=%d\n",argv[argc-1],errno);free(project);_exit(123);
            }
            argv[argc-1]=project;
            printf("CODEOSS_PROJECT path=%s uid=%u\n",project,getuid());
        }
        errno=0;
        if(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)!=-1 || errno!=EPERM)_exit(122);
        puts("C1_SUBREAPER_UNPRIVILEGED_DENIED");
        if(nnp && prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))_exit(115);
        if(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)!=(long)nnp)_exit(116);
        if(filter){struct sock_filter code[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,8888,0,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|77),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};struct sock_fprog p={4,code};if(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&p))_exit(117);}
        printf("G2_LAUNCH_CONFIG actual_nnp=%d actual_filter=%d requested_nnp=%u filter=%u no_account_before_canonical_exec=1\n",prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0),prctl(PR_GET_SECCOMP),nnp,filter);
        if(setenv("HOME",workbench?"/home/user":home,1) || setenv("TMPDIR","/home/port-test/runtime",1) ||
           setenv("XDG_CONFIG_HOME","/home/port-test/.config",1) ||
           setenv("XDG_CACHE_HOME","/home/port-test/.cache",1) ||
           setenv("XDG_RUNTIME_DIR","/home/port-test/runtime",1))_exit(113);
        unsetenv("ELECTRON_RUN_AS_NODE");unsetenv("DISPLAY");unsetenv("WAYLAND_DISPLAY");
        if(setenv("WAYLAND_SOCKET","3",1))_exit(113);
        if(setenv("XCURSOR_PATH","/usr/share/icons",1) || setenv("XCURSOR_THEME","Coast",1) ||
           setenv("XCURSOR_SIZE","24",1))_exit(113);
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
        unsetenv("NODE_OPTIONS");unsetenv("NODE_PATH");unsetenv("LD_PRELOAD");unsetenv("LD_LIBRARY_PATH");
        if(setenv("ELECTRON_TRASH","trash-cli",1))_exit(113);
        /* Two I/O workers match the supported two-vCPU guest. Keep an explicit
           user choice; each descendant remains under the same task ceiling. */
        if(!getenv("UV_THREADPOOL_SIZE") && setenv("UV_THREADPOOL_SIZE","2",1))_exit(113);
        /* The workbench also starts Electron in Node mode through its shell.
           Those invocations need the same jitless policy as the main app. */
        if(workbench && !browser && (setenv("NODE_OPTIONS","--jitless",1) ||
                         setenv("NODE_COMPILE_CACHE","/home/port-test/.cache/codeoss-compile",1)))_exit(113);
        printf("GENUINE_ELECTRON_EXEC pid=%u retained_display_fd=3\n",getpid());
        char *args[34]={"/opt/electron-43.7.3/electron"};
        for(int i=start_arg;i<argc;i++)args[1+i-start_arg]=argv[i];
        printf("C1_RUNTIME_CHILD uid=%u euid=%u gid=%u egid=%u canonical=/opt/electron-43.7.3/electron\n",getuid(),geteuid(),getgid(),getegid());
        execve(args[0],args,environ);fprintf(stderr,"C1_RUNTIME_EXECVE_ERROR errno=%d\n",errno);_exit(114);
    }
    close(3);close(input[0]);close(input[1]);close(output[1]);
    uint64_t start=now(),terminated=0,exited=0;int status=-1;size_t captured=0;bool reaped=false,eof=false,children_done=false;
    unsigned helpers=0;
    const size_t log_limit=workbench?16u*1024*1024:262144u;
    bool log_truncated=false,log_error=false;
    while(!reaped || !eof || !children_done) {
        struct pollfd p={eof?-1:output[0],POLLIN,0};int r=poll(&p,1,100);
        if(r>0 && (p.revents&(POLLIN|POLLHUP))) {
            char bytes[4096];ssize_t n=read(output[0],bytes,sizeof(bytes));
            if(n<=0)eof=true;
            else {
                size_t remaining=captured<log_limit?log_limit-captured:0;
                size_t visible=(size_t)n<remaining?(size_t)n:remaining;
                if(visible && !log_error && forward_all(bytes,visible))log_error=true;
                captured+=n;
                if(captured>log_limit && !log_truncated) {
                    printf("\nC1_RUNTIME_OUTPUT_TRUNCATED limit=%zu drained_bytes=%zu\n",log_limit,captured);
                    log_truncated=true;
                }
            }
        }
        if(!reaped && waitpid(child,&status,WNOHANG)==child) {
            reaped=true;exited=now();
            printf("C1_RUNTIME_MAIN_EXIT status=%d exit=%d signal=%d eof=%u\n",status,
                   WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0,eof);
        }
        if(reaped) {
            int helper_status;pid_t helper;
            while((helper=waitpid(-1,&helper_status,WNOHANG))>0) {
                helpers++;
                printf("C1_RUNTIME_HELPER_REAP pid=%d status=%d\n",helper,helper_status);
            }
            children_done=helper<0 && errno==ECHILD;
        }
        if(!workbench && !reaped && !terminated && now()-start>60000){kill(child,SIGTERM);terminated=now();}
        if(!reaped && terminated && now()-terminated>1500){kill(child,SIGKILL);waitpid(child,&status,0);reaped=true;}
        if(reaped && (!eof || !children_done) && (workbench?now()-exited>10000:now()-start>63000)) {terminated=now();break;}
    }
    close(output[0]);
    printf("C1_RUNTIME_DESCENDANTS_REAPED helpers=%u complete=%u\n",helpers,children_done);
    printf("C1_RUNTIME_WAIT status=%d exit=%d signal=%d timeout=%u bytes=%zu elapsed_ms=%lu parent_uid=%u\n",status,WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0,terminated!=0,captured,now()-start,getuid());
    return terminated?124:log_error?126:WIFEXITED(status)?WEXITSTATUS(status):WIFSIGNALED(status)?128+WTERMSIG(status):125;
}
