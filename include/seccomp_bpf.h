#ifndef COAST_SECCOMP_BPF_H
#define COAST_SECCOMP_BPF_H
#include <kernel.h>
struct seccomp_insn { uint16_t code; uint8_t jt, jf; uint32_t k; };
struct seccomp_data { int32_t number; uint32_t arch; uint64_t ip, args[6]; };
bool seccomp_bpf_action(uint32_t);
unsigned seccomp_bpf_rank(uint32_t);
bool seccomp_bpf_verify(const struct seccomp_insn *, unsigned, uint32_t *, bool);
uint32_t seccomp_bpf_run(const struct seccomp_insn *, unsigned, const struct seccomp_data *);
#endif
