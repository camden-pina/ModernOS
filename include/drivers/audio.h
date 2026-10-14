#ifndef MODERNOS_KERNEL_AUDIO_H
#define MODERNOS_KERNEL_AUDIO_H
#include <stdbool.h>
#include <stdint.h>
#include <sched.h>

bool audio_probe(void *pci_header);
long audio_call(struct task *, unsigned operation, uint64_t pointer, uint64_t frames);
void audio_tick(void);
void audio_init(void);
#endif
