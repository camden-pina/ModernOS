#!/usr/bin/env python3
"""Sanitize actual native pixel/damage/region code, including guarded tails."""
import argparse
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "userland/desktop/pixel_damage.h"
#include "userland/electron-wayland/pixel-convert.h"
#include "userland/electron-wayland/region-bitmap.h"
#include "userland/electron-wayland/surface-damage.h"
static unsigned seed=17;
static unsigned random32(void){seed=seed*1664525+1013904223;return seed;}
static void *guarded(size_t size,void **mapping,size_t *mapped){
    size_t page=(size_t)sysconf(_SC_PAGESIZE),pages=(size+page-1)/page;
    *mapped=(pages+1)*page;*mapping=mmap(NULL,*mapped,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(*mapping!=MAP_FAILED);assert(!mprotect((char*)*mapping+pages*page,page,PROT_NONE));
    return (char*)*mapping+pages*page-size;
}
static uint32_t reference_pixel(uint32_t x,int argb){
    if(!argb)return x&0xffffff;
    unsigned out=0,alpha=x>>24,bg[3]={42,32,24};
    for(unsigned i=0;i<3;i++){
        unsigned color=((x>>(8*i))&255)+(bg[i]*(255-alpha)+127)/255;
        out|=(color>255?255:color)<<(8*i);
    }
    return out;
}
int main(void){
    unsigned damage_cases=0,conversion_cases=0,region_cases=0,surface_cases=0;
    assert(!pixel_damage(NULL,NULL,0,11).height);
    assert(!pixel_damage(NULL,NULL,11,0).height);
    for(unsigned width=1;width<=131;width++)for(unsigned height=1;height<=35;height+=3){
        size_t size=(size_t)width*height*4,ma,mb;void *pa,*pb;
        uint32_t *a=guarded(size,&pa,&ma),*b=guarded(size,&pb,&mb);
        for(unsigned i=0;i<width*height;i++)a[i]=b[i]=random32();
        assert(!pixel_damage(a,b,width,height).height);
        unsigned left=width,top=height,right=0,bottom=0;
        for(unsigned n=0;n<25;n++){
            unsigned x=random32()%width,y=random32()%height;
            b[y*width+x]=a[y*width+x]^1;
            if(x<left)left=x;if(y<top)top=y;if(x+1>right)right=x+1;if(y+1>bottom)bottom=y+1;
            PixelDamage d=pixel_damage(a,b,width,height);
            assert(d.x==left&&d.y==top&&d.width==right-left&&d.height==bottom-top);damage_cases++;
        }
        b[0]=a[0]^1;b[width*height-1]=a[width*height-1]^1;
        PixelDamage d=pixel_damage(a,b,width,height);
        assert(!d.x&&!d.y&&d.width==width&&d.height==height);damage_cases++;
        munmap(pa,ma);munmap(pb,mb);
    }
    for(unsigned count=1;count<=257;count++){
        size_t ma,mb;void *pa,*pb;
        uint32_t *a=guarded(count*4,&pa,&ma),*b=guarded(count*4,&pb,&mb);
        for(unsigned variant=0;variant<5;variant++)for(int argb=0;argb<2;argb++){
            for(unsigned i=0;i<count;i++)a[i]=variant==0?random32()|0xff000000:
                variant==1?random32()&0xffffff:random32();
            pixel_convert(b,a,count,argb);
            for(unsigned i=0;i<count;i++)assert(b[i]==reference_pixel(a[i],argb));
            conversion_cases++;
            unsigned first=count,last=0;
            for(unsigned i=0;i<count;i++) {
                b[i]=random32()&0xffffff;
                if(i%5==0)b[i]=reference_pixel(a[i],argb);
                if(b[i]!=reference_pixel(a[i],argb)){if(i<first)first=i;last=i+1;}
            }
            struct pixel_span span=pixel_convert_damage(b,a,count,argb);
            assert(span.width==(last?last-first:0));if(last)assert(span.x==first);
            for(unsigned i=0;i<count;i++)assert(b[i]==reference_pixel(a[i],argb));
            assert(!pixel_convert_damage(b,a,count,argb).width);
            conversion_cases+=2;
        }
        munmap(pa,ma);munmap(pb,mb);
    }
    for(unsigned n=0;n<20000;n++) {
        struct surface_damage d={0};
        int64_t x0=INT64_MAX,y0=INT64_MAX,x1=INT64_MIN,y1=INT64_MIN;
        for(unsigned j=0;j<4;j++) {
            int32_t x=(int32_t)random32(),y=(int32_t)random32();
            int32_t w=random32()>>1,h=random32()>>1;
            if(n%2==0){x=x%300-50;y=y%300-50;w=w%100;h=h%100;}
            surface_damage_add(&d,x,y,w,h);
            if(w>0&&h>0){if(x<x0)x0=x;if(y<y0)y0=y;if((int64_t)x+w>x1)x1=(int64_t)x+w;if((int64_t)y+h>y1)y1=(int64_t)y+h;}
        }
        unsigned scale=1+(random32()%2),width=1+random32()%256,height=1+random32()%256;
        int left=(int)(random32()%80)-40,top=(int)(random32()%80)-40;
        struct pixel_rect r=surface_damage_pixels(d,scale,left,top,width,height);
        int64_t l=x0==INT64_MAX?0:x0*scale+left,t=y0==INT64_MAX?0:y0*scale+top;
        int64_t right=x1==INT64_MIN?0:x1*scale+left,bottom=y1==INT64_MIN?0:y1*scale+top;
        if(l<0)l=0;if(t<0)t=0;if(right>width)right=width;if(bottom>height)bottom=height;
        if(!d.set||right<=l||bottom<=t)assert(!r.width&&!r.height);
        else assert(r.x==l&&r.y==t&&r.width==right-l&&r.height==bottom-t);
        surface_cases++;
    }
    unsigned char a[101],b[101];
    for(unsigned n=0;n<15000;n++){
        for(unsigned i=0;i<sizeof(a);i++)a[i]=b[i]=random32();
        size_t first=random32()%(sizeof(a)*8+1),last=random32()%(sizeof(a)*8+1);
        if(first>last){size_t tmp=first;first=last;last=tmp;}
        int add=random32()&1;
        region_span(a,first,last,add);
        for(size_t bit=first;bit<last;bit++){
            if(add)b[bit/8]|=1u<<(bit%8);else b[bit/8]&=(unsigned char)~(1u<<(bit%8));
        }
        assert(!memcmp(a,b,sizeof(a)));region_cases++;
    }
    printf("PASS actual graphics helpers: %u damage bounds, %u pixel conversions, %u exact bit spans, %u surface mappings; guarded row tails and ASan/UBSan\n",damage_cases,conversion_cases,region_cases,surface_cases);
}
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output-root',type=Path,required=True)
    a=p.parse_args();out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False)
    source=out/'graphics.c';source.write_text(HARNESS)
    binary=out/'graphics'
    subprocess.run(['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror',
                    '-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    '-I'+str(ROOT),str(source),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],check=True,text=True,capture_output=True)
    (out/'result.txt').write_text(result.stdout);print(result.stdout,end='')


if __name__=='__main__':main()
