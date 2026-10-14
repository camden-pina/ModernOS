#!/usr/bin/env python3
"""Check actual kernel integer memory routines against inaccessible boundaries."""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/kernel-memory-test'


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    source = BUILD / 'check.c'
    source.write_text(r'''
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
extern void *modernos_memset(void *, int, size_t);
extern void *modernos_memcpy(void *, const void *, size_t);
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"Kernel memory failure at %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    unsigned char destination[640], original[640];
    const int values[] = {-1025, -1, 0, 1, 127, 255, 256, 511};
    for (unsigned alignment=0;alignment<32;alignment++)
        for (unsigned size=0;size<=257;size++)
            for (unsigned value=0;value<sizeof(values)/sizeof(values[0]);value++) {
                memset(destination,0x5a,sizeof(destination));
                memcpy(original,destination,sizeof(destination));
                memset(original+32+alignment,values[value],size);
                CHECK(modernos_memset(destination+32+alignment,values[value],size)==destination+32+alignment);
                CHECK(!memcmp(destination,original,sizeof(destination)));
            }
    size_t page=sysconf(_SC_PAGESIZE), span=32*page;
    unsigned char *mapping=mmap(NULL,span+2*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    unsigned char *input=mmap(NULL,span+2*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(mapping!=MAP_FAILED && input!=MAP_FAILED);
    CHECK(!mprotect(mapping+page,span,PROT_READ|PROT_WRITE));
    CHECK(!mprotect(input+page,span,PROT_READ|PROT_WRITE));
    CHECK(modernos_memset(mapping,0,0)==mapping && modernos_memset(mapping+page+span,0,0)==mapping+page+span);
    CHECK(modernos_memcpy(mapping,input,0)==mapping);
    for (size_t size=1;size<=span;size=size<513?size+1:size*2) {
        unsigned char *end=mapping+page+span-size, *from=input+page+span-size;
        memset(mapping+page,0x5a,span); memset(from,0x93,size);
        CHECK(modernos_memset(end,-1,size)==end);
        for (size_t i=0;i<size;i++) CHECK(end[i]==0xff);
        if (end>mapping+page) CHECK(end[-1]==0x5a);
        CHECK(modernos_memcpy(end,from,size)==end && !memcmp(end,from,size));
        if (end>mapping+page) CHECK(end[-1]==0x5a);
        CHECK(modernos_memset(mapping+page,0x31,size)==mapping+page);
        for (size_t i=0;i<size;i++) CHECK(mapping[page+i]==0x31);
    }
    CHECK(!munmap(mapping,span+2*page) && !munmap(input,span+2*page));
    puts("PASS kernel memory spans: 32 alignments, byte patterns, exact returns, zero lengths and inaccessible page boundaries");
}
''')
    names = ('memset', 'memcpy', 'memmove', 'memcmp',
             'memchr', 'strlen', 'strcmp', 'strncmp', 'strcpy')
    subprocess.run(['cc', '-O2', '-ffreestanding', '-fno-builtin', '-mno-sse', '-mno-sse2',
                    '-I' + str(ROOT / 'include'),
                    '-I' + str(ROOT / 'arch/x86/include/asm'),
                    *('-D' + name + '=modernos_' + name for name in names),
                    '-c', str(ROOT / 'lib/string.c'), '-o', str(BUILD / 'string.o')], check=True)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(source),
                    str(BUILD / 'string.o'), '-o', str(BUILD / 'check')], check=True)
    subprocess.run([str(BUILD / 'check')], check=True, timeout=30)


if __name__ == '__main__':
    main()
