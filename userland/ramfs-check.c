#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do{if(!(x)){perror(#x);return 1;}}while(0)

int main(void) {
    const char *path = "/usr/share/fonts/TTF/DejaVuSans.ttf";
    int fd = open(path, O_RDWR);
    CHECK(fd>=0);
    struct stat st;
    CHECK(!fstat(fd,&st) && st.st_size>16384 && st.st_size<2*1024*1024);
    size_t length = st.st_size;
    unsigned char *original = malloc(length), *copy = malloc(length);
    CHECK(original && copy);
    CHECK(pread(fd,original,length,0)==(ssize_t)length);
    unsigned char change[3] = {0x42, 0x83, 0x17};
    CHECK(pwrite(fd,change,3,4095)==3);
    CHECK(pread(fd,copy,length,0)==(ssize_t)length);
    CHECK(!memcmp(copy,original,4095) && !memcmp(copy+4095,change,3) && !memcmp(copy+4098,original+4098,length-4098));
    CHECK(!ftruncate(fd,4097) && !ftruncate(fd,12001));
    memset(copy, 0xff, 12001);
    CHECK(pread(fd,copy,12001,0)==12001);
    CHECK(!memcmp(copy,original,4095) && copy[4095]==change[0] && copy[4096]==change[1]);
    for (unsigned i = 4097; i < 12001; i++)
        CHECK(copy[i]==0);
    CHECK(pwrite(fd,change,3,16383)==3 && pread(fd,copy,12289,4097)==12289);
    for (unsigned i = 0; i < 12286; i++)
        CHECK(copy[i]==0);
    CHECK(!memcmp(copy+12286,change,3));
    CHECK(!ftruncate(fd,0) && write(fd,change,3)==3 && !fstat(fd,&st) && st.st_size==3);
    CHECK(!ftruncate(fd,0) && pwrite(fd,original,length,0)==(ssize_t)length);
    CHECK(pread(fd,copy,length,0)==(ssize_t)length && !memcmp(copy,original,length));
    CHECK(!close(fd));
    free(copy);
    free(original);
    puts(
        "RAMFS BOOT COPY CHECK PASS: borrowed reads, private page-boundary writes, shrink/grow zeroes, sparse gaps, truncate and exact restoration");
    return 0;
}
