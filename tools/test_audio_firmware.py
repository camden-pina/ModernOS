#!/usr/bin/env python3
"""Test the real AW88399 parser and driver sequence without host device writes."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/aw88399_core.h"
struct model {uint16_t registers[128],memory[65536],address;const struct aw_profile *profile;bool crc,watchdog;unsigned writes;};
static bool read_register(void *opaque,uint8_t reg,uint16_t *value) {
    struct model *m=opaque;
    if(reg==0x41)*value=m->address==0x1f40?0x4638:m->memory[m->address++];
    else *value=m->registers[reg];return true;
}
static bool write_register(void *opaque,uint8_t reg,const uint8_t *bytes,size_t size) {
    struct model *m=opaque;assert(reg<128 && size>=2 && size<=128 && !(size&1));m->writes++;
    if(reg==0x41){for(size_t i=0;i<size;i+=2)m->memory[m->address++]=(uint16_t)bytes[i]<<8|bytes[i+1];return true;}
    assert(size==2);uint16_t value=(uint16_t)bytes[0]<<8|bytes[1];m->registers[reg]=value;
    if(reg==0x40)m->address=value;
    if(reg==4 && !(value&4))m->registers[0x42]=m->watchdog?1:0;
    if(reg==0x4b && (value&0x3000)) {
        assert(m->registers[0x17]&1);struct aw_section section=value&0x2000?m->profile->program:m->profile->configuration;
        unsigned start=value&0x2000?0x8980:0x9b00;
        for(size_t i=0;i<section.size/2;i++)assert(m->memory[start+i]==aw_le16(section.bytes+i*2));
        m->registers[0x20]=m->crc?0x4000:0;
    }
    return true;
}
static void delay(unsigned microseconds){assert(microseconds<=4000);}
static void check_chip(const struct aw_profile *profile,bool crc,bool watchdog,bool clock) {
    struct model *m=calloc(1,sizeof(*m));assert(m);m->profile=profile;m->crc=crc;m->watchdog=watchdog;
    m->registers[0]=0x2183;m->registers[1]=clock?0x111:0; /* No BSTS on this Lenovo model. */
    struct aw_chip chip={.context=m,.read=read_register,.write=write_register,.delay=delay,.skip_boost_status=true};
    assert(aw_chip_load(&chip,profile));assert(chip.loaded && !chip.playing);
    assert((m->registers[4]&0x107)==0x107);
    bool played=aw_chip_start(&chip);assert(played==(crc && watchdog && clock));
    if(played){assert(chip.playing && !(m->registers[4]&0x107));assert((m->registers[5]&0x3ff)==chip.volume);assert(m->registers[0x4a]>0);}
    else {assert(!chip.playing && (m->registers[4]&0x107)==0x107);assert((m->registers[5]&0x3ff)==1023);}
    aw_chip_stop(&chip);assert(!chip.playing && (m->registers[4]&0x107)==0x107);
    assert(m->writes<500);free(m);
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *file=fopen(argv[1],"rb");assert(file);assert(!fseek(file,0,SEEK_END));long size=ftell(file);rewind(file);
    assert(size>=80 && size<=512*1024);uint8_t *bytes=malloc(size),*bad=malloc(size);assert(bytes && bad);assert(fread(bytes,1,size,file)==(size_t)size);fclose(file);
    struct aw_profile right={0},left={0},invalid={0};
    assert(aw_acf_profile(bytes,size,1,0x34,&right));assert(aw_acf_profile(bytes,size,0,0x35,&left));
    assert(right.registers.size && left.registers.size && memcmp(left.registers.bytes,right.registers.bytes,left.registers.size));
    for(long cut=0;cut<size;cut+=113)assert(!aw_acf_profile(bytes,cut,0,0x35,&invalid));
    for(long offset=336;offset<size;offset+=29){memcpy(bad,bytes,size);bad[offset]^=1U<<(offset%8);assert(!aw_acf_profile(bad,size,0,0x35,&invalid));}
    for(unsigned offset=52;offset<68;offset++){memcpy(bad,bytes,size);bad[offset]^=0x80;assert(!aw_acf_profile(bad,size,0,0x35,&invalid));}
    assert(!aw_acf_profile(bytes,size,2,0x34,&invalid));assert(!aw_acf_profile(bytes,size,0,0x36,&invalid));
    check_chip(&left,true,true,true);check_chip(&right,true,true,true);
    check_chip(&left,false,true,true);check_chip(&left,true,false,true);check_chip(&left,true,true,false);
    free(bad);free(bytes);puts("PASS AW88399: actual firmware profiles, truncation/corruption/bounds, byte order, DSP upload/CRC, factory trims, left/right mapping and muted failure on CRC/watchdog/clock errors");
}
'''

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, default=Path('/lib/firmware/aw88399_acf.bin'))
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix='modernos-audio-') as temporary:
        path = Path(temporary);
        source = path / 'check.c';
        binary = path / 'check';
        source.write_text(SOURCE)

        subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-misleading-indentation', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-idirafter', str(ROOT / 'include'), '-idirafter', str(ROOT / 'arch/x86/include/asm'),
                        str(source), '-o', str(binary)], check=True)

        subprocess.run([str(binary), str(args.firmware)], check=True)
