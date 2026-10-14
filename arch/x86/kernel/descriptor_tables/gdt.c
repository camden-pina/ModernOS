#include <descriptor_tables/gdt.h>
#include <string.h>
#include <cpu.h>

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3], reserved1, ist[7], reserved2;
    uint16_t reserved3, iomap;
} __attribute__((packed));
struct descriptor_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));
static struct {
    uint64_t entries[8];
    struct tss64 tss;
    uint8_t fault_stack[8192] __attribute__((aligned(16)));
    uint8_t nmi_stack[8192] __attribute__((aligned(16)));
} tables[MAX_CPUS] __attribute__((aligned(64)));
extern void gdt_load(const struct descriptor_ptr *ptr);

void gdt_init_cpu(unsigned cpu, uintptr_t stack) {
    uint64_t *g = tables[cpu].entries;
    struct tss64 *t = &tables[cpu].tss;
    memset(t, 0, sizeof(*t));
    /* STAR user base 0x18: SYSRET SS=0x23, CS=0x2b. */
    g[0] = 0;
    g[1] = 0x00af9a000000ffffULL;
    g[2] = 0x00cf92000000ffffULL;
    g[3] = 0;
    g[4] = 0x00cff2000000ffffULL;
    g[5] = 0x00affa000000ffffULL;
    uintptr_t base = (uintptr_t)t;
    g[6] = (sizeof(*t) - 1) | ((base & 0xffffff) << 16) | (0x89ULL << 40) |
           ((base & 0xff000000) << 32);
    g[7] = base >> 32;
    t->rsp[0] = stack;
    t->ist[0] = (uintptr_t)tables[cpu].fault_stack + sizeof(tables[cpu].fault_stack);
    t->ist[1] = (uintptr_t)tables[cpu].nmi_stack + sizeof(tables[cpu].nmi_stack);
    t->iomap = sizeof(*t);
    struct descriptor_ptr ptr = {sizeof(tables[cpu].entries) - 1, (uintptr_t)g};
    gdt_load(&ptr);
}

void gdt_init(void) {
    uintptr_t stack;
    __asm__ volatile("mov %%rsp,%0" : "=r"(stack));
    gdt_init_cpu(0, stack);
}

void gdt_set_rsp0(unsigned cpu, uintptr_t stack) {
    tables[cpu].tss.rsp[0] = stack;
}
