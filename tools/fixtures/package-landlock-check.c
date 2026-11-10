#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL<<13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL<<14)
#endif
#define READS (LANDLOCK_ACCESS_FS_READ_FILE|LANDLOCK_ACCESS_FS_READ_DIR)
#define ALL ((1ULL<<15)-1)
#define REQUIRE(c) do {if(!(c)){fprintf(stderr,"CHECK_FAILED line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
static char top[512],allowed[512],outside[512],a[512],b[512];
static const char *self;
static int create(unsigned long long rights) {
    struct landlock_ruleset_attr attr={.handled_access_fs=rights};
    return syscall(SYS_landlock_create_ruleset,&attr,8,0);
}
static int add(int rules,const char *path,unsigned long long rights) {
    int fd=open(path,O_PATH|O_CLOEXEC);if(fd<0)return -1;
    struct landlock_path_beneath_attr attr={.allowed_access=rights,.parent_fd=fd};
    int result=syscall(SYS_landlock_add_rule,rules,LANDLOCK_RULE_PATH_BENEATH,&attr,0);
    int saved=errno;close(fd);errno=saved;return result;
}
static int restrict_set(int fd) {
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))return -1;
    return syscall(SYS_landlock_restrict_self,fd,0);
}
static int readable(const char *path) {
    int fd=open(path,O_RDONLY);if(fd<0)return -1;char c;
    int result=read(fd,&c,1)==1?0:-1;close(fd);return result;
}
static int write_file(const char *path) {
    int fd=open(path,O_CREAT|O_WRONLY|O_TRUNC,0600);if(fd<0)return -1;
    int result=write(fd,"test",4)==4?0:-1;close(fd);return result;
}
static void name(char dest[512],const char *base,const char *tail) {
    if(snprintf(dest,512,"%s/%s",base,tail)>=512)abort();
}
static int validation(void) {
    REQUIRE(getuid()!=0);
    REQUIRE(syscall(SYS_landlock_create_ruleset,NULL,0,LANDLOCK_CREATE_RULESET_VERSION)>=3);
    errno=0;REQUIRE(create(0)<0 && errno==ENOMSG);
    errno=0;REQUIRE(create(1ULL<<63)<0 && errno==EINVAL);
    int rules=create(ALL);REQUIRE(rules>=0);
    errno=0;REQUIRE(syscall(SYS_landlock_restrict_self,rules,0)<0 && errno==EPERM);
    struct landlock_path_beneath_attr attr={.allowed_access=READS,.parent_fd=-1};
    errno=0;REQUIRE(syscall(SYS_landlock_add_rule,rules,LANDLOCK_RULE_PATH_BENEATH,&attr,0)<0 && errno==EBADF);
    errno=0;REQUIRE(add(rules,a,READS)<0 && errno==EINVAL);
    REQUIRE(add(rules,allowed,ALL)==0);
    int duplicate=dup(rules);REQUIRE(duplicate>=0 && (fcntl(rules,F_GETFD)&FD_CLOEXEC));
    char c;errno=0;REQUIRE(read(rules,&c,1)<0 && errno==EINVAL);
    errno=0;REQUIRE(ftruncate(rules,0)<0 && errno==EINVAL);
    close(rules);close(duplicate);return 0;
}
static int read_paths(void) {
    int dir=open(outside,O_PATH|O_DIRECTORY);REQUIRE(dir>=0);
    int rules=create(READS);REQUIRE(rules>=0 && add(rules,allowed,READS)==0 && restrict_set(rules)==0);
    close(rules);REQUIRE(readable(a)==0);
    errno=0;REQUIRE(readable(b)<0 && errno==EACCES);
    char path[512];name(path,allowed,"escape/file");
    errno=0;REQUIRE(readable(path)<0 && errno==EACCES);
    name(path,top,"alias/a");REQUIRE(readable(path)==0);
    int fd=open(outside,O_RDONLY|O_DIRECTORY);REQUIRE(fd<0 && errno==EACCES);
    fd=openat(dir,"file",O_RDONLY);REQUIRE(fd<0 && errno==EACCES);
    close(dir);return 0;
}
static int download_policy(void) {
    int rules=create(ALL);
    REQUIRE(rules>=0 && add(rules,"/",READS)==0 && add(rules,allowed,ALL&~(LANDLOCK_ACCESS_FS_REFER|LANDLOCK_ACCESS_FS_EXECUTE))==0);
    REQUIRE(restrict_set(rules)==0);close(rules);
    REQUIRE(readable(b)==0 && write_file(a)==0);
    errno=0;REQUIRE(write_file(b)<0 && errno==EACCES);
    char c[512],d[512];name(c,allowed,"created");name(d,allowed,"renamed");
    REQUIRE(write_file(c)==0 && rename(c,d)==0 && unlink(d)==0);
    name(c,outside,"created");errno=0;REQUIRE(mkdir(c,0700)<0 && errno==EACCES);
    errno=0;REQUIRE(symlink(a,c)<0 && errno==EACCES);
    errno=0;REQUIRE(unlink(b)<0 && errno==EACCES);
    const char *args[]={self,"--probe",top,NULL};
    errno=0;REQUIRE(execv(self,(char *const *)args)<0 && errno==EACCES);
    return 0;
}
static int truncate_rights(void) {
    int before=open(b,O_RDWR);REQUIRE(before>=0);
    int rules=create(LANDLOCK_ACCESS_FS_TRUNCATE);REQUIRE(rules>=0 && add(rules,a,LANDLOCK_ACCESS_FS_TRUNCATE)==0);
    REQUIRE(restrict_set(rules)==0);close(rules);
    REQUIRE(ftruncate(before,3)==0);close(before);
    int after=open(b,O_WRONLY);REQUIRE(after>=0);
    errno=0;REQUIRE(ftruncate(after,0)<0 && errno==EACCES);close(after);
    errno=0;REQUIRE(truncate(b,0)<0 && errno==EACCES);
    errno=0;REQUIRE(open(b,O_WRONLY|O_TRUNC)<0 && errno==EACCES);
    errno=0;REQUIRE(open(b,O_RDONLY|O_TRUNC)<0 && errno==EACCES);
    after=open(a,O_RDONLY|O_TRUNC);REQUIRE(after>=0);close(after);
    REQUIRE(truncate(a,0)==0);return 0;
}
static int existing_descriptors(void) {
    int before=open(b,O_RDWR);REQUIRE(before>=0);
    int rules=create(READS|LANDLOCK_ACCESS_FS_WRITE_FILE);REQUIRE(rules>=0 && add(rules,allowed,READS|LANDLOCK_ACCESS_FS_WRITE_FILE)==0);
    REQUIRE(restrict_set(rules)==0);close(rules);
    char c;REQUIRE(read(before,&c,1)==1 && write(before,"x",1)==1);close(before);
    errno=0;REQUIRE(open(b,O_RDONLY)<0 && errno==EACCES);
    return 0;
}
static int immutable_layers(void) {
    int rules=create(READS);REQUIRE(rules>=0 && add(rules,allowed,READS)==0 && restrict_set(rules)==0);
    REQUIRE(add(rules,outside,READS)==0);
    errno=0;REQUIRE(readable(b)<0 && errno==EACCES);
    REQUIRE(restrict_set(rules)==0);close(rules);
    REQUIRE(readable(a)==0);errno=0;REQUIRE(readable(b)<0 && errno==EACCES);
    rules=create(READS);REQUIRE(rules>=0 && restrict_set(rules)==0);close(rules);
    errno=0;REQUIRE(readable(a)<0 && errno==EACCES);
    return 0;
}
static int inherited(void) {
    int rules=create(READS);REQUIRE(rules>=0 && add(rules,allowed,READS)==0 && add(rules,self,LANDLOCK_ACCESS_FS_READ_FILE)==0 && restrict_set(rules)==0);
    close(rules);pid_t child=fork();REQUIRE(child>=0);
    if(!child) {execl(self,self,"--probe",top,NULL);perror("landlock exec fixture");_exit(92);}
    int status;REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    return 0;
}
static int refer_denial(void) {
    char dst[512];name(dst,outside,"linked");
    int rules=create(ALL);REQUIRE(rules>=0 && add(rules,top,ALL&~LANDLOCK_ACCESS_FS_REFER)==0 && restrict_set(rules)==0);close(rules);
    errno=0;REQUIRE(rename(a,dst)<0 && errno==EXDEV);
    errno=0;REQUIRE(link(a,dst)<0 && errno==EXDEV);
    name(dst,allowed,"same-directory");REQUIRE(link(a,dst)==0 && rename(dst,a)==0);
    return 0;
}
static int refer_allow(void) {
    char dst[512];name(dst,outside,"linked");
    int rules=create(ALL);REQUIRE(rules>=0 && add(rules,top,ALL)==0 && restrict_set(rules)==0);close(rules);
    REQUIRE(link(a,dst)==0 && unlink(dst)==0 && rename(a,dst)==0 && readable(dst)==0);
    return 0;
}
static int refer_gain_denied(void) {
    char dst[512];name(dst,outside,"linked");
    int rules=create(ALL);REQUIRE(rules>=0 && add(rules,allowed,ALL&~LANDLOCK_ACCESS_FS_READ_FILE)==0 && add(rules,outside,ALL)==0 && restrict_set(rules)==0);close(rules);
    errno=0;REQUIRE(link(a,dst)<0 && errno==EXDEV);
    errno=0;REQUIRE(rename(a,dst)<0 && errno==EXDEV);
    return 0;
}
static atomic_int phase,result;
static void *thread_body(void *unused) {
    (void)unused;
    while(!atomic_load_explicit(&phase,memory_order_acquire))sched_yield();
    atomic_store(&result,readable(b)==0);return NULL;
}
static int thread_scope(void) {
    pthread_t thread;REQUIRE(pthread_create(&thread,NULL,thread_body,NULL)==0);
    int rules=create(READS);REQUIRE(rules>=0 && add(rules,allowed,READS)==0 && restrict_set(rules)==0);close(rules);
    errno=0;REQUIRE(readable(b)<0 && errno==EACCES);
    atomic_store_explicit(&phase,1,memory_order_release);
    REQUIRE(pthread_join(thread,NULL)==0 && atomic_load(&result)==1);
    return 0;
}
static int inode_anchor(void) {
    int ready[2],resume[2];REQUIRE(pipe(ready)==0 && pipe(resume)==0);
    pid_t child=fork();REQUIRE(child>=0);
    if(!child) {
        int rules=create(READS);
        if(rules<0 || add(rules,allowed,READS) || restrict_set(rules))_exit(91);
        close(rules);char c='x';
        if(write(ready[1],&c,1)!=1 || read(resume[0],&c,1)!=1)_exit(92);
        char moved[512];name(moved,top,"moved/a");
        if(readable(moved) || readable(a)!=-1 || errno!=EACCES)_exit(93);
        _exit(0);
    }
    char c;REQUIRE(read(ready[0],&c,1)==1);
    char moved[512];name(moved,top,"moved");
    REQUIRE(rename(allowed,moved)==0 && mkdir(allowed,0700)==0 && write_file(a)==0);
    REQUIRE(write(resume[1],"x",1)==1);int status;
    REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(ready[0]);close(ready[1]);close(resume[0]);close(resume[1]);return 0;
}
int main(int argc,char **argv) {
    self=argv[0];
    if(argc==3 && !strcmp(argv[1],"--probe")) {
        name(a,argv[2],"allow/a");name(b,argv[2],"out/file");
        REQUIRE(readable(a)==0);errno=0;REQUIRE(readable(b)<0 && errno==EACCES);return 0;
    }
    struct {const char *name;int (*body)(void);} tests[]={
        {"ABI, flags, fd types and unprivileged setup",validation},
        {"directory rules, symlink escape and directory fd traversal",read_paths},
        {"upstream pacman read-root write-cache policy",download_policy},
        {"truncate right is captured on open file descriptions",truncate_rights},
        {"descriptors opened before restriction keep their access",existing_descriptors},
        {"rules are immutable snapshots and layers intersect",immutable_layers},
        {"restriction survives close, fork and exec",inherited},
        {"cross-directory rename and hardlink require REFER",refer_denial},
        {"equal-rights REFER permits rename and hardlink",refer_allow},
        {"REFER cannot gain destination privileges",refer_gain_denied},
        {"ABI v3 restricts the caller and future children only",thread_scope},
        {"grants follow inode identity after an outside rename",inode_anchor},
    };
    unsigned failures=0;
    for(unsigned i=0;i<sizeof(tests)/sizeof(tests[0]);i++) {
        strcpy(top,"/tmp/coast-landlock-XXXXXX");REQUIRE(mkdtemp(top));
        name(allowed,top,"allow");name(outside,top,"out");
        REQUIRE(mkdir(allowed,0700)==0 && mkdir(outside,0700)==0);
        name(a,allowed,"a");name(b,outside,"file");REQUIRE(write_file(a)==0 && write_file(b)==0);
        char path[512];name(path,allowed,"escape");REQUIRE(symlink("../out",path)==0);
        name(path,top,"alias");REQUIRE(symlink("allow",path)==0);
        fflush(NULL);pid_t child=fork();REQUIRE(child>=0);
        if(!child)_exit(tests[i].body());
        int status;REQUIRE(waitpid(child,&status,0)==child);
        int passed=WIFEXITED(status) && !WEXITSTATUS(status);
        printf("%s %s status=%d\n",passed?"PASS":"FAIL",tests[i].name,status);failures+=!passed;
        /* The runner is unrestricted, so it can remove only its own fixture. */
        char moved[512],dest[512];name(moved,top,"moved");name(dest,outside,"linked");unlink(dest);
        unlink(a);unlink(b);name(path,allowed,"escape");unlink(path);name(path,top,"alias");unlink(path);
        name(path,allowed,"same-directory");unlink(path);name(path,moved,"escape");unlink(path);name(path,moved,"a");unlink(path);
        rmdir(allowed);rmdir(outside);rmdir(moved);rmdir(top);
    }
    printf("PACKAGE_LANDLOCK_%s cases=%zu\n",failures?"FAIL":"PASS",sizeof(tests)/sizeof(tests[0]));
    return failures?1:0;
}
