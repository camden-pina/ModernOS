#ifndef _ASM_X86_TSC_H
#define _ASM_X86_TSC_H

#include <stdint.h>

void tsc_init(void);
uint64_t tsc_uptime_us(void);

#endif /* _ASM_X86_TSC_H */
