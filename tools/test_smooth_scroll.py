#!/usr/bin/env python3
"""Exercise the desktop's shared scroll dynamics at variable frame rates."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include "smooth_scroll.h"
int main(void) {
    for (int interval=8; interval<=33; interval++) {
        SmoothScroll s={0}; int value=0, frames=0;
        smooth_by(&s,0,300,600,0);
        assert(smooth_tick(&s,0,600,0)==0);
        for (int t=interval; t<1000; t+=interval) {
            int next=smooth_tick(&s,value,600,t);
            assert(next>=value && next<=300);
            frames+=next!=value; value=next;
        }
        assert(value==300 && !s.active && frames>=6);
        smooth_by(&s,value,-300,600,1000);
        value=smooth_tick(&s,value,600,1008);
        assert(value>0 && value<300);
        smooth_by(&s,value,100000000000LL,600,1008);
        assert(s.target==600);
        value=smooth_tick(&s,value,600,1700);
        assert(value==600 && !s.active);
        value=smooth_tick(&s,value,80,1701);
        assert(value==80 && s.target==80 && !s.active);
        smooth_by(&s,value,-90,80,1702);
        assert(s.target==0);
        smooth_jump(&s,0,1702);
        assert(smooth_tick(&s,0,600,1900)==0 && !s.active);
    }
    SmoothScroll burst={0}; int position=0;
    for(int t=0;t<500;t+=8) {
        if(t<32) smooth_by(&burst,position,54,500,t);
        position=smooth_tick(&burst,position,500,t);
    }
    assert(position==216);
    puts("PASS scroll intermediate frames, accumulated input, reversal, bounds, suspension and cancellation at 30–125 Hz");
}
'''

if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='modernos-scroll-') as temporary:
        directory = Path(temporary)
        source = directory / 'check.c'
        source.write_text(SOURCE)
        binary = directory / 'check'
        subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(ROOT / 'userland/desktop'), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
