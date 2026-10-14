#!/usr/bin/env python3
"""Exercise the actual I226 driver against bounded MMIO/PHY/DMA failure models.

This does not touch a host device or establish physical PHY compatibility.
QEMU's igb integration separately verifies real packet delivery.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADERS = {
    'mm/pmm.h': '''#include <stddef.h>
#include <stdint.h>
#define PAGE_SIZE 4096
uint64_t pmm_alloc_pages(size_t);
void pmm_free_pages(uint64_t,size_t);
''',
    'mm/user.h': '''#include <stdint.h>
static inline void *phys_ptr(uint64_t address){return (void *)(uintptr_t)address;}
''',
    'mm/vmem_io.h': '''#include <stddef.h>
#include <stdint.h>
void *vmem_map_mmio(uint64_t,size_t);
''',
    'tsc.h': '#include <stdint.h>\nuint64_t tsc_uptime_us(void);\n',
    'printk.h': '''#include <stdio.h>
#define pr_info(...) ((void)fprintf(stderr,__VA_ARGS__))
#define pr_warn(...) ((void)fprintf(stderr,__VA_ARGS__))
''',
}
SOURCE = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "drivers/net/igc.c"
static uint32_t mmio[128*1024/4];
static uint16_t phy[32], mmd_control, mmd_address, multigig;
static uint64_t ticks;
static unsigned allocations, frees, mdi_operations;
static void *blocks[4];
static bool queue_failure, stuck_dma, phy_reset;
static _Alignas(8) unsigned char configuration[256];
uint64_t pmm_alloc_pages(size_t pages) {
    assert(allocations<4);void *p=aligned_alloc(PAGE_SIZE,pages*PAGE_SIZE);assert(p);
    blocks[allocations++]=p;memset(p,0xa5,pages*PAGE_SIZE);return (uintptr_t)p;
}
void pmm_free_pages(uint64_t address,size_t pages) {
    assert(pages==1 || pages==BUFFER_PAGES);
    assert(!(mmio[STATUS/4]&0x80000));
    bool found=false;for(unsigned i=0;i<allocations;i++)if(blocks[i]==(void*)(uintptr_t)address){free(blocks[i]);blocks[i]=NULL;found=true;}
    assert(found);frees++;
}
void *vmem_map_mmio(uint64_t base,size_t bytes) {assert(base==0x100000 && bytes==sizeof(mmio));return mmio;}
uint64_t tsc_uptime_us(void) {
    ticks+=50;
    if(mmio[CTRL/4]&0x04000000) {
        mmio[CTRL/4]&=~0x04000000U;mmio[EECD/4]|=0x200;
        mmio[RAL/4]=0x12345652;mmio[RAH/4]=0x8000789a;
    }
    if(mmio[CTRL/4]&0x80000000U){phy_reset=true;mmio[PHPM/4]=0;}
    else if(phy_reset)mmio[PHPM/4]=0x100;
    if((mmio[CTRL/4]&4) && !(stuck_dma && allocations==4))mmio[STATUS/4]&=~0x80000U;
    else mmio[STATUS/4]|=0x80000;
    uint32_t command=mmio[MDIC/4];
    if((command&0x0c000000) && !(command&0x10000000)) {
        mdi_operations++;unsigned reg=(command>>16)&31;uint16_t data=command;
        assert(((command>>21)&31)==0); /* Integrated GPY PHY, not the 82576. */
        if(command&0x04000000) {
            if(reg==13)mmd_control=data;
            else if(reg==14){if(mmd_control&0x4000){assert((mmd_control&31)==7 && mmd_address==0x20);multigig=data;}else mmd_address=data;}
            else phy[reg]=data;
        } else {
            data=reg==14 && (mmd_control&0x4000)?multigig:phy[reg];
            command=(command&~0xffffU)|data;
        }
        mmio[MDIC/4]=command|0x10000000;
    }
    if(queue_failure && allocations==4)mmio[(RX_BASE+0x28)/4]&=~(1U<<25);
    return ticks;
}
static pci_device_header_t *fixture(void) {
    /* Retained pages in the failed-quiescence model are safe to free only in
     * this host model teardown, after disabling the simulated device. */
    for(unsigned i=0;i<4;i++)if(blocks[i])free(blocks[i]);
    memset(blocks,0,sizeof(blocks));memset(mmio,0,sizeof(mmio));memset(phy,0,sizeof(phy));
    memset(configuration,0,sizeof(configuration));
    registers=NULL;initialized=false;rx=NULL;tx=NULL;
    rx_physical=tx_physical=rx_buffers=tx_buffers=0;
    allocations=frees=mdi_operations=0;ticks=0;queue_failure=stuck_dma=phy_reset=false;
    mmd_control=mmd_address=multigig=0;
    phy[2]=0x67c9;phy[3]=0xdc01;phy[0]=0x0800;phy[4]=0x0c01;phy[9]=0x0100;
    mmio[STATUS/4]=3;mmio[EECD/4]=0x200;mmio[RAL/4]=0x12345652;mmio[RAH/4]=0x8000789a;
    pci_device_header_t *pci=(void*)configuration;
    pci->vendor_id=0x8086;pci->device_id=0x125c;pci->bar[0]=0x100000;pci->capabilities_ptr=0x40;
    configuration[0x40]=1;configuration[0x44]=3;
    return pci;
}
int main(void) {
    pci_device_header_t *pci=fixture();assert(igc_probe(pci));
    assert(igc_present() && igc_link() && allocations==4 && !frees && phy_reset);
    assert((configuration[0x44]&3)==0 && (pci->command&6)==6);
    assert(!(mmio[SWFW/4]&2) && !(mmio[SWSM/4]&3));
    assert((phy[0]&0x1200)==0x1200 && !(phy[0]&0x4c00));
    assert((phy[4]&0x01e0)==0x01e0 && !(phy[4]&0x0c00));
    assert((phy[9]&0x0300)==0x0200 && (multigig&0x80) && mmd_control==0);
    assert(mmio[0x2404/4]==(34U|(2U<<6)) && mmio[0x3404/4]==(20U|(4U<<24)));
    unsigned char frame[1515],copy[1515];memset(frame,0x63,sizeof(frame));
    assert(igc_send(frame,1515)==-E_MSGSIZE);
    for(unsigned i=0;i<RING;i++)assert(igc_send(frame,64)==64);
    assert(igc_send(frame,64)==-E_AGAIN && !tx_head);
    for(unsigned i=0;i<RING;i++){assert(tx[i].command==(0x2b300000U|64));assert(tx[i].status==(64U<<14));assert(!memcmp(phys_ptr(tx[i].packet),frame,64));}
    memcpy(phys_ptr(rx_buffers),frame,64);rx[0].done.status=3;rx[0].done.length=64;
    assert(igc_receive(copy,sizeof(copy))==64 && !memcmp(frame,copy,64));
    assert(rx[0].read.packet==rx_buffers && rx[0].read.header==0 && rx_head==1);
    rx[1].done.status=3;rx[1].done.length=3000;assert(igc_receive(copy,sizeof(copy))==0);
    rx[2].done.status=0x80000003;rx[2].done.length=64;assert(igc_receive(copy,sizeof(copy))==0);
    rx[3].done.status=1;rx[3].done.length=64;assert(igc_receive(copy,sizeof(copy))==0);
    rx[4].done.status=3;rx[4].done.length=64;assert(igc_receive(copy,8)==0);
    pci=fixture();phy[2]=0xffff;assert(!igc_probe(pci) && !initialized && !allocations && !(pci->command&4));
    pci=fixture();mmio[SWFW/4]=0x20000;
    assert(!igc_probe(pci) && !mdi_operations && mmio[SWFW/4]==0x20000 && ticks<500000);
    pci=fixture();queue_failure=true;
    assert(!igc_probe(pci) && !initialized && allocations==4 && frees==4 && registers==NULL);
    pci=fixture();queue_failure=stuck_dma=true;
    assert(!igc_probe(pci) && !initialized && allocations==4 && !frees && registers && !(pci->command&4));
    fixture();
    puts("PASS I226 driver model: PHY address/D0/reset/autoneg/firmware ownership, DMA rings/bounds, malformed RX, bounded failures, quiescent cleanup and retained pages when DMA cannot stop");
}
'''

if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='modernos-igc-') as temporary:
        path = Path(temporary)
        for name, text in HEADERS.items():
            target = path / name;
            target.parent.mkdir(parents=True, exist_ok=True);
            target.write_text(text)

        source = path / 'check.c';
        source.write_text(SOURCE);
        binary = path / 'check'
        subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(path), '-I', str(ROOT),
                        '-idirafter', str(ROOT / 'include'), '-idirafter', str(ROOT / 'arch/x86/include/asm'),
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
