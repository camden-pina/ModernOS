#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"READ_PREFIX_FAIL line=%u errno=%d\n",__LINE__,errno);exit(1);}}while(0)
int main(void) {
    struct utsname os;if(uname(&os)||strcmp(os.sysname,"Coast"))return 77;
    const size_t bound=16*1024*1024,total=19*1024*1024;
    const char *path="/home/port-test/runtime/read-prefix-check.bin";
    unsigned char *input=malloc(total),*output=mmap(NULL,total,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(input&&output!=MAP_FAILED);
    for(size_t i=0;i<total;i++)input[i]=(i*37+i/101)&255;
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
    for(size_t at=0;at<total;at+=1024*1024)CHECK(write(fd,input+at,1024*1024)==1024*1024);
    CHECK(lseek(fd,0,SEEK_SET)==0);CHECK(read(fd,output,total)==(ssize_t)bound);CHECK(lseek(fd,0,SEEK_CUR)==(off_t)bound);
    CHECK(!memcmp(input,output,bound));CHECK(read(fd,output+bound,total-bound)==(ssize_t)(total-bound));CHECK(!memcmp(input,output,total));
    CHECK(lseek(fd,17,SEEK_SET)==17);CHECK(pread(fd,output,total,0)==(ssize_t)bound);CHECK(!memcmp(input,output,bound));CHECK(lseek(fd,0,SEEK_CUR)==17);
    CHECK(lseek(fd,0,SEEK_SET)==0);CHECK(mprotect(output+bound-4096,4096,PROT_NONE)==0);
    errno=0;CHECK(read(fd,output,total)==-1&&errno==EFAULT);CHECK(lseek(fd,0,SEEK_CUR)==0);
    CHECK(mprotect(output+bound-4096,4096,PROT_READ|PROT_WRITE)==0);
    errno=0;CHECK(write(fd,input,total)==-1&&errno==E2BIG);CHECK(lseek(fd,0,SEEK_CUR)==0);
    int ends[2];CHECK(pipe(ends)==0);errno=0;CHECK(read(ends[0],output,total)==-1&&errno==E2BIG);close(ends[0]);close(ends[1]);
    close(fd);FILE *stream=fopen(path,"rb");CHECK(stream!=NULL);CHECK(fread(output,1,total,stream)==total);CHECK(!ferror(stream)&&!memcmp(input,output,total));fclose(stream);
    CHECK(unlink(path)==0);CHECK(munmap(output,total)==0);free(input);
    printf("READ_PREFIX_PASS checks=%u regular_read16MiB_bound pread_position libc_fread19MiB exact_bytes fault_atomicity unchanged_write_pipe_bounds cleanup\n",checks);
    return 0;
}
