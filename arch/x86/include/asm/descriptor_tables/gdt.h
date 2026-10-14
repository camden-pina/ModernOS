#ifndef _GDT_H
#define _GDT_H 1

#include <kernel.h>
#include <string.h>

// From:
// http://www.cocoawithlove.com/2008/04/using-pointers-to-recast-in-c-is-bad.html
#define UNION_CAST(x, destType) \
    (((union {                  \
         __typeof__(x) a;       \
         destType b;            \
     })x)                       \
         .b)

#define KERNEL_CS 0x08
#define GDT_KERNEL_DS 0x10
#define GDT_TSS 0x30
#define USER_CS 0x2b
#define USER_DS 0x23

void gdt_init(void);
void gdt_init_cpu(unsigned cpu, uintptr_t stack);
void gdt_set_rsp0(unsigned cpu, uintptr_t stack);

#endif
