#ifndef COAST_WINDOW_H
#define COAST_WINDOW_H
#include <stdint.h>
#include <stdbool.h>
struct task;
long window_call(struct task *, unsigned, uint64_t, uint64_t);
void window_drop(unsigned);
bool window_client_focused(unsigned);
#endif
