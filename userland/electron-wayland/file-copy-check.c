#define _GNU_SOURCE
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { checks++;if(!(x)){fprintf(stderr,"FILE_COPY_FAIL line=%u errno=%d\n",__LINE__,errno);exit(1);} } while(0)
static unsigned char data[65567],copy[65567];
int main(void) {
    struct utsname os;if(uname(&os) || strcmp(os.sysname,"Coast"))return 77;
    const char *source="/home/port-test/runtime/copy-source",*target="/home/port-test/runtime/copy-target";
    for(unsigned i=0;i<sizeof(data);i++)data[i]=(i*37+i/101)&255;
    int in=open(source,O_CREAT|O_TRUNC|O_RDWR,0600),out=open(target,O_CREAT|O_TRUNC|O_RDWR,0600);
    CHECK(in>=0&&out>=0);CHECK(write(in,data,sizeof(data))==sizeof(data));CHECK(lseek(in,7,SEEK_SET)==7);
    off_t offset=13;CHECK(sendfile(out,in,&offset,4099)==4099);CHECK(offset==4112);CHECK(lseek(in,0,SEEK_CUR)==7);
    CHECK(pread(out,copy,4099,0)==4099);CHECK(!memcmp(copy,data+13,4099));
    CHECK(ftruncate(out,0)==0);CHECK(lseek(out,0,SEEK_SET)==0);CHECK(lseek(in,0,SEEK_SET)==0);
    CHECK(sendfile(out,in,NULL,sizeof(data))==sizeof(data));CHECK(lseek(in,0,SEEK_CUR)==sizeof(data));
    CHECK(pread(out,copy,sizeof(copy),0)==sizeof(copy));CHECK(!memcmp(copy,data,sizeof(data)));
    CHECK(sendfile(out,in,NULL,1)==0);CHECK(sendfile(out,in,NULL,0)==0);
    CHECK(lseek(in,0,SEEK_SET)==0);CHECK(lseek(out,0,SEEK_SET)==0);
    errno=0;CHECK(sendfile(out,in,(off_t *)1,20)==-1&&errno==EFAULT);CHECK(lseek(out,0,SEEK_CUR)==0);CHECK(lseek(in,0,SEEK_CUR)==0);
    offset=-1;errno=0;CHECK(sendfile(out,in,&offset,20)==-1&&errno==EINVAL);CHECK(offset==-1);
    errno=0;CHECK(sendfile(-1,in,NULL,1)==-1&&errno==EBADF);
    errno=0;CHECK(sendfile(out,-1,NULL,1)==-1&&errno==EBADF);
    int ro=open(target,O_RDONLY),wo=open(source,O_WRONLY),path=open(source,O_PATH),append=open(target,O_WRONLY|O_APPEND);
    CHECK(ro>=0&&wo>=0&&path>=0&&append>=0);
    errno=0;CHECK(sendfile(ro,in,NULL,1)==-1&&errno==EBADF);
    errno=0;CHECK(sendfile(out,wo,NULL,1)==-1&&errno==EBADF);
    errno=0;CHECK(sendfile(out,path,NULL,1)==-1&&errno==EBADF);
    errno=0;CHECK(sendfile(append,in,NULL,1)==-1&&errno==EINVAL);
    errno=0;CHECK(sendfile(in,in,NULL,1)==-1&&errno==EINVAL);
    int same=open(source,O_RDWR);CHECK(same>=0);errno=0;CHECK(sendfile(same,in,NULL,1)==-1&&errno==EINVAL);
    int pair[2];CHECK(pipe(pair)==0);errno=0;CHECK(sendfile(pair[1],in,NULL,1)==-1&&errno==EINVAL);
    errno=0;CHECK(sendfile(out,pair[0],NULL,1)==-1&&errno==EINVAL);close(pair[0]);close(pair[1]);
    int alias=dup(in);CHECK(alias>=0);CHECK(lseek(in,21,SEEK_SET)==21);CHECK(sendfile(out,alias,NULL,17)==17);CHECK(lseek(in,0,SEEK_CUR)==38);
    CHECK(unlink(source)==0);offset=100;CHECK(sendfile(out,in,&offset,50)==50);CHECK(offset==150);
    CHECK(pread(out,copy,50,17)==50);CHECK(!memcmp(copy,data+100,50));
    close(alias);close(same);close(ro);close(wo);close(path);close(append);close(in);close(out);CHECK(unlink(target)==0);
    printf("FILE_COPY_PASS checks=%u offsets exact_bytes invalid_fd invalid_pointer permissions append endpoint same_inode dup unlinked_cleanup\n",checks);return 0;
}
