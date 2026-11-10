#ifndef COAST_CLIPBOARD_H
#define COAST_CLIPBOARD_H
#include <kernel.h>
struct task;
long clipboard_call(struct task *, unsigned op, uint64_t pointer, uint64_t size, uint64_t expected);
#endif
