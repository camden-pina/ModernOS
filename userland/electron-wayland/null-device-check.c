#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"NULL_DEVICE_FAIL line=%u errno=%d\n",__LINE__,errno);exit(1);}} while(0)
int main(void) {
    struct utsname os;if(uname(&os)||strcmp(os.sysname,"Coast"))return 77;
    CHECK(getuid()==1000&&getgid()==1000);
    int fd=open("/dev/null",O_RDWR|O_CLOEXEC);CHECK(fd>=0);
    CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC);
    struct stat st;CHECK(fstat(fd,&st)==0&&S_ISCHR(st.st_mode));
    char input[8192],output=42;memset(input,37,sizeof(input));
    CHECK(read(fd,&output,1)==0&&output==42);
    CHECK(write(fd,input,sizeof(input))==sizeof(input));
    CHECK(read(fd,&output,1)==0&&output==42);
    int alias=dup(fd);CHECK(alias>=0);CHECK(close(fd)==0);
    CHECK(write(alias,"discard",7)==7);CHECK(close(alias)==0);
    fd=open("/dev/null",O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(fd>=0);
    CHECK(write(fd,"redirect",8)==8);CHECK(close(fd)==0);
    errno=0;CHECK(open("/dev/null",O_WRONLY|O_CREAT|O_EXCL,0600)==-1&&errno==EEXIST);
    fd=open("/dev/null",O_RDONLY);CHECK(fd>=0);
    errno=0;CHECK(write(fd,"x",1)==-1&&errno==EBADF);CHECK(close(fd)==0);
    for(unsigned i=0;i<4;i++) {
        const char *path=(const char*[]){"/dev/zero","/dev/tty","/dev/console","/dev/urandom"}[i];
        errno=0;CHECK(open(path,O_RDWR)==-1&&(errno==EACCES||errno==ENOENT||errno==EPERM));
    }
    CHECK(unlink("/dev/null")==-1);CHECK(chmod("/dev/null",0600)==-1);
    CHECK(stat("/dev/null",&st)==0&&S_ISCHR(st.st_mode)&&(st.st_mode&0777)==0666);
    printf("NULL_DEVICE_PASS checks=%u real_inode eof discard dup redirection exclusive other_devices_denied immutable cleanup\n",checks);
    return 0;
}
