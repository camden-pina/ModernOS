#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "disk_inventory.h"
static DiskInventory load(const char *text) {
    DiskInventory d;disk_parse(&d,text,strlen(text));return d;
}
int main(void) {
    DiskInventory d=load("NAME TYPE BYTES SECTOR RO FSTYPE MOUNTPOINT\n"
        "nvme0n1 disk 67108864 512 0 - -\n"
        "  nvme0n1p1 part 33554432 512 1 fat32 /mnt/test\n"
        "sda disk 1048576 512 1 - -\n");
    assert(d.count==3&&!d.incomplete&&d.entries[1].parent==0&&d.entries[2].parent==-1);
    assert(d.entries[0].bytes==67108864&&d.entries[1].readonly&&d.entries[1].partition);
    assert(!strcmp(d.entries[1].mount,"/mnt/test"));
    const char *bad[]={"a disk 18446744073709551616 512 0 - -\n","a disk -1 512 0 - -\n",
       "a disk 512 0 0 - -\n","a disk 512 4294967296 0 - -\n","a disk 513 512 0 - -\n",
       "a disk 512 512 2 - -\n","a alien 512 512 0 - -\n","a disk 512 512 0 - - extra\n",
       "a disk 512 512 0 - -","a disk 512 512 0\n","a disk 512 512 0 \001 -\n"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){d=load(bad[i]);assert(d.incomplete&&d.count==0);}
    d=load("a disk 512 512 0 - -\na disk 512 512 0 - -\n");assert(d.count==1&&d.incomplete);
    d=load("  a1 part 512 512 0 - -\n");assert(d.count==1&&d.entries[0].parent==-1);
    char cap[12000];size_t n=0;for(int i=0;i<70;i++)n+=snprintf(cap+n,sizeof(cap)-n,"disk%d disk 512 512 0 - -\n",i);
    d=load(cap);assert(d.count==DISK_LIMIT&&d.incomplete);
    for(size_t i=0;i<strlen(cap);i++){disk_parse(&d,cap,i);assert(d.count<=DISK_LIMIT);}
    puts("PASS real inventory parser: device/partition hierarchy, capacity, mount and read-only state; overflow, truncation, malformed fields, duplicates, capacity limit and every truncated prefix.");
}
