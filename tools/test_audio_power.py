#!/usr/bin/env python3
"""Exercise real speaker _CRS templates and GPIO code without host MMIO writes.

Pin fields are filled using the DSDT's pure GNUM results (140 reset, 171 IRQ).
Memory addresses in the model are fixtures, not measured host addresses.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "drivers/aw88399_power.h"
struct model {uint32_t regs[65536/4];unsigned writes,holds,levels[3];bool ignore;};
static struct model *active;
static uint32_t read32(void *data,unsigned offset){assert(!(offset&3) && offset<65536);return ((struct model*)data)->regs[offset/4];}
static void write32(void *data,unsigned offset,uint32_t value){
 struct model *m=data;assert(offset==0x6c0 || offset==0x6c4);m->writes++;
 if(!m->ignore)m->regs[offset/4]=value;
}
static void hold(unsigned us){assert(us==1000 && active->holds<3);active->levels[active->holds++]=active->regs[0x6c0/4]&1;}
static void reset_model(struct model *m){memset(m,0,sizeof(*m));m->regs[0]=0x940000;m->regs[0xc/4]=0x600;active=m;}
static void file_bytes(const char *path,uint8_t *bytes,size_t size){FILE *f=fopen(path,"rb");assert(f && fread(bytes,1,size,f)==size && fgetc(f)==EOF);fclose(f);}
int main(int argc,char **argv){
 assert(argc==3);uint8_t crs[138],memory[241],bad[241];
 file_bytes(argv[1],crs,sizeof(crs));file_bytes(argv[2],memory,sizeof(memory));
 struct aw_power_resources resources;uint64_t base;
 assert(aw_power_parse(crs,sizeof(crs),&resources));
 assert(resources.reset_pin==140 && resources.pull==1 && resources.bus_hz==400000);
 assert(aw_power_window(memory,sizeof(memory),&base) && base==0x10010000);
 for(size_t n=0;n<sizeof(crs);n++)assert(!aw_power_parse(crs,n,&resources));
 for(size_t n=0;n<sizeof(memory);n++)assert(!aw_power_window(memory,n,&base));
 memcpy(bad,crs,sizeof(crs));bad[1]=255;assert(!aw_power_parse(bad,sizeof(crs),&resources));
 memcpy(bad,crs,sizeof(crs));bad[0x59]=171;assert(aw_power_parse(bad,sizeof(crs),&resources) && resources.reset_pin==171);
 memcpy(bad,crs,sizeof(crs));bad[0x10]=0x36;assert(!aw_power_parse(bad,sizeof(crs),&resources));
 memcpy(bad,crs,sizeof(crs));bad[0x42+14]=22;assert(!aw_power_parse(bad,sizeof(crs),&resources));
 memcpy(bad,crs,sizeof(crs));bad[0x42+17]=24;assert(!aw_power_parse(bad,sizeof(crs),&resources));
 memcpy(bad,crs,sizeof(crs));bad[0x42+4]=0;assert(!aw_power_parse(bad,sizeof(crs),&resources));
 memcpy(bad,crs,sizeof(crs));bad[0x42+7]=2;assert(!aw_power_parse(bad,sizeof(crs),&resources));
 memcpy(bad,memory,sizeof(memory));bad[9+30]=1;assert(!aw_power_window(bad,sizeof(memory),&base));
 struct model *m=malloc(sizeof(*m));assert(m);struct aw_gpio_io io={m,read32,write32,hold};
 reset_model(m);m->regs[0x6c0/4]=0x81000400;m->regs[0x6c4/4]=0x80000001;
 assert(aw_gpio_reset(&io,140,1) && m->holds==3 && m->levels[0]==1 && m->levels[1]==0 && m->levels[2]==1);
 assert((m->regs[0x6c0/4]&0x3f00)==0x200 && (m->regs[0x6c0/4]&0x81000000)==0x81000000);
 assert((m->regs[0x6c4/4]&0x3c00)==0x3000 && (m->regs[0x6c4/4]&0x80000001)==0x80000001);
 reset_model(m);m->regs[0xb4/4]=1U<<16;assert(!aw_gpio_reset(&io,140,1) && !m->writes);
 reset_model(m);m->regs[0x114/4]=1U<<12;assert(!aw_gpio_reset(&io,140,1) && !m->writes);
 reset_model(m);m->regs[0x110/4]=1U<<12;assert(!aw_gpio_reset(&io,140,1) && !m->writes);
 reset_model(m);m->regs[0x110/4]=1U<<12;m->regs[0x6c0/4]=0x200;m->regs[0x6c4/4]=0x3000;
 assert(aw_gpio_reset(&io,140,1) && m->writes==3 && m->holds==3);
 reset_model(m);m->regs[0]=UINT32_MAX;assert(!aw_gpio_reset(&io,140,1) && !m->writes);
 reset_model(m);m->regs[0]=0x1100000;assert(!aw_gpio_reset(&io,140,1) && !m->writes);
 reset_model(m);m->regs[0xc/4]=65520;assert(!aw_gpio_reset(&io,140,1) && !m->writes);
 reset_model(m);m->ignore=true;assert(!aw_gpio_reset(&io,140,1) && !m->holds);
 reset_model(m);assert(!aw_gpio_reset(&io,171,1) && !m->writes);
 free(m);puts("PASS actual AW88399 ACPI resource parser and GPIO reset code: supplied DSDT templates, bounds, ownership, locks, preserved pad bits, high/low/high and failures; no physical audible claim");
}
'''


def buffer_at(aml, name, start):
    at = aml.index(b'\x08' + name + b'\x11', start) + 6
    count = aml[at] >> 6
    at += count + 1
    op = aml[at];
    at += 1

    widths = {0x0a: 1, 0x0b: 2, 0x0c: 4}
    width = widths[op];
    size = int.from_bytes(aml[at:at + width], 'little');
    at += width

    return bytearray(aml[at:at + size])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dsdt', type=Path, required=True)
    args = parser.parse_args();

    aml = args.dsdt.read_bytes()
    assert aml[:4] == b'DSDT' and sum(aml) % 256 == 0

    speaker = buffer_at(aml, b'RBUF', aml.index(b'AWDZ8399'))
    assert len(speaker) == 138
    struct.pack_into('<H', speaker, 0x59, 140);
    struct.pack_into('<H', speaker, 0x7c, 171)

    controller = buffer_at(aml, b'RBFS', aml.index(b'INTC1084'))
    assert len(controller) == 241

    for i in range(5):
        at = 9 + i * 46;
        assert controller[at] == 0x8a
        base = 0x10000000 + i * 65536
        struct.pack_into('<Q', controller, at + 14, base);
        struct.pack_into('<Q', controller, at + 22, base + 65535)

    with tempfile.TemporaryDirectory(prefix='modernos-audio-power-') as temp:
        p = Path(temp);
        (p / 'check.c').write_text(SOURCE);
        (p / 'speaker.crs').write_bytes(speaker);
        (p / 'gpio.crs').write_bytes(controller)

        subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-idirafter', str(
                            ROOT / 'include'),
                        '-idirafter', str(ROOT / 'arch/x86/include/asm'),
                        str(p / 'check.c'), '-o', str(p / 'check')], check=True)

        subprocess.run([str(p / 'check'), str(p / 'speaker.crs'), str(p / 'gpio.crs')], check=True)
