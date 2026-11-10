#ifndef COAST_PIDNS_H
#define COAST_PIDNS_H
#include <kernel.h>
#ifndef CONFIG_PID_NAMESPACE_TEST
#define CONFIG_PID_NAMESPACE_TEST 0
#endif
#ifndef CONFIG_PID_NAMESPACE_VERIFY
#define CONFIG_PID_NAMESPACE_VERIFY 0
#endif
struct task;
struct pidns_signal_info {unsigned pid,uid;int code,status;bool valid;};
bool pidns_active(const struct task *);
bool pidns_proc_available(const struct task *);
unsigned pidns_view(const struct task *,unsigned);
unsigned pidns_resolve(const struct task *,unsigned);
bool pidns_verify_fault(struct task *,unsigned);
int pidns_clone_check(const struct task *,uint64_t);
int pidns_clone_prepare(struct task *,const struct task *,uint64_t);
void pidns_clone_abort(struct task *);
int pidns_publish(struct task *);
int pidns_group_set(struct task *,unsigned,unsigned);
int pidns_signal_permission(const struct task *,const struct task *,unsigned);
int pidns_signal_record(const struct task *,struct task *,unsigned,int,unsigned);
bool pidns_signal_take(struct task *,unsigned,struct pidns_signal_info *);
bool pidns_signal_drop(struct task *,unsigned);
void pidns_signal_cleanup(struct task *);
void pidns_reap_deferred(void);
bool pidns_init_exit(struct task *);
bool pidns_dying(const struct task *);
unsigned pidns_orphan_reaper(const struct task *,unsigned);
void *pidns_peer_get(const struct task *);
void pidns_peer_put(void *);
unsigned pidns_peer_view(const struct task *,void *,unsigned);
#endif
