#!/usr/bin/env python3
"""Test the actual renderer's scalar/SSE2 paths against independent arithmetic,
including inaccessible guard pages, then compile/run them inside ModernOS.
"""
import argparse
import shlex
import subprocess

from test_runtime import ROOT, VM, WORK, assert_success

SOURCE = r'''
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include "pixel_blend.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"PIXEL_FAIL line=%d\n",__LINE__); return 1; } } while(0)
static uint32_t state=0x12345678;
static uint32_t random_word(void) {
    state^=state<<13; state^=state>>17; state^=state<<5; return state;
}
static uint32_t reference(uint32_t background,uint32_t color,unsigned alpha) {
    if (!alpha) return background;
    uint32_t value=0;
    for (unsigned channel=0;channel<24;channel+=8) {
        unsigned b=(background>>channel)&255, c=(color>>channel)&255;
        value|=((b*(255-alpha)+c*alpha+128)/256)<<channel;
    }
    return value;
}
int main(void) {
    uint32_t scalar[400], vector[400], expected[400], selected[400];
    const size_t sizes[]={0,1,2,3,4,5,7,8,9,15,16,17,31,32,33,63,64,65,127,128,129,255,256,257};
    for (unsigned alpha=0;alpha<256;alpha++)
        for (unsigned alignment=0;alignment<16;alignment++)
            for (unsigned size=0;size<sizeof(sizes)/sizeof(sizes[0]);size++) {
                uint32_t color=random_word(); size_t n=sizes[size], at=32+alignment;
                for (unsigned i=0;i<400;i++) scalar[i]=vector[i]=expected[i]=selected[i]=random_word();
                for (size_t i=0;i<n;i++) expected[at+i]=reference(expected[at+i],color,alpha);
                pixels_blend_scalar(scalar+at,n,color,alpha);
                pixels_blend_sse2(vector+at,n,color,alpha);
                pixels_blend(selected+at,n,color,alpha);
                CHECK(!memcmp(scalar,expected,sizeof(scalar)));
                CHECK(!memcmp(vector,expected,sizeof(vector)));
                CHECK(!memcmp(selected,expected,sizeof(selected)));
            }
    size_t page=sysconf(_SC_PAGESIZE), span=2*page;
    unsigned char *mapping=mmap(0,span+2*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(mapping!=MAP_FAILED && !mprotect(mapping+page,span,PROT_READ|PROT_WRITE));
    pixels_blend_sse2((uint32_t *)mapping,4,0x121e2f,0);
    pixels_blend_sse2((uint32_t *)mapping,0,0x121e2f,37);
    for (size_t n=1;n<=257;n++) {
        uint32_t *pixels=(uint32_t *)(mapping+page+span)-n;
        for (size_t i=0;i<n;i++) pixels[i]=0x9b30445c;
        if ((unsigned char *)pixels>mapping+page) pixels[-1]=0xcafed00d;
        pixels_blend_sse2(pixels,n,0x121e2f,37);
        for (size_t i=0;i<n;i++) CHECK(pixels[i]==reference(0x9b30445c,0x121e2f,37));
        if ((unsigned char *)pixels>mapping+page) CHECK(pixels[-1]==0xcafed00d);
    }
    CHECK(!munmap(mapping,span+2*page));
    printf("PIXEL_BLEND_CHECK_PASS backend=%s: independent RGB arithmetic, all alpha values, alignments, tails and guards\n",
           pixels_blend_vectorized()?"SSE2":"integer");
    return 0;
}
'''


def host():
    directory = WORK / 'pixel-blend-host'
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / 'check.c'
    source.write_text(SOURCE)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-mno-avx', '-mno-avx2',
                    '-I' + str(ROOT / 'userland/desktop'), str(source),
                    str(ROOT / 'userland/desktop/pixel_blend.c'), '-o', str(directory / 'check')], check=True)
    subprocess.run([str(directory / 'check')], check=True, timeout=30)


def guest(cpus, iso=None):
    vm = VM(cpus, storage=False, network=False, iso=iso, label='pixel-blend')
    try:
        files = {'pixel-check.c': SOURCE}
        for name in ('pixel_blend.h', 'pixel_blend.c'):
            files[name] = (ROOT / 'userland/desktop' / name).read_text()

        for name, text in files.items():
            for index, line in enumerate(text.strip().splitlines()):
                command = ('write' if not index else 'append') + ' /tmp/' + name + ' ' + shlex.quote(line)
                assert len(command.encode()) < 511
                assert_success(vm.command(command))

        assert_success(vm.command(
            'gcc -O2 -I/tmp /tmp/pixel-check.c /tmp/pixel_blend.c -o /tmp/pixel-check'))
        output = vm.command('/tmp/pixel-check',
                            'PIXEL_BLEND_CHECK_PASS backend=integer', timeout=180)
        assert_success(output)
        vm.command('gui --info', 'Shadow blending=integer (QEMU TCG)')

        print(
            f'PASS {cpus} CPU renderer: actual native scalar/SSE2 results and emulator dispatch', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--host-only', action='store_true')
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--iso')
    args = parser.parse_args()

    host()
    if not args.host_only:
        guest(args.cpus, args.iso)
