#ifndef COAST_SECCOMP_H
#define COAST_SECCOMP_H
#include <kernel.h>
#ifndef CONFIG_SECCOMP_TEST
#define CONFIG_SECCOMP_TEST 0
#endif
struct task;
#define SC_KILL_PROCESS 0x80000000u
#define SC_KILL_THREAD 0x00000000u
#define SC_TRAP 0x00030000u
#define SC_ERRNO 0x00050000u
#define SC_ALLOW 0x7fff0000u
uint32_t seccomp_decide(struct task *,const register_t *);
long seccomp_call(struct task *,register_t *);
void seccomp_frame_tag(struct task *,unsigned,bool);
bool seccomp_frame_tagged(struct task *,unsigned);
bool signal_seccomp(struct task *,register_t *,unsigned);
long generic_seccomp_call(struct task *,register_t *);
uint32_t generic_seccomp_decide(struct task *,const register_t *);
void generic_seccomp_inherit(struct task *,const struct task *);
void generic_seccomp_release(struct task *);
#endif
