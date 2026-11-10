#ifndef COAST_TEST_CREDENTIALS_H
#define COAST_TEST_CREDENTIALS_H
#include <sched.h>
#ifndef CONFIG_CREDENTIALS_TEST
#define CONFIG_CREDENTIALS_TEST 0
#endif
struct exec_credentials {unsigned ruid,uid,rgid,gid;bool secure;};
unsigned credentials_ruid(const struct task *);
unsigned credentials_rgid(const struct task *);
void credentials_all_ids(struct task *,unsigned,unsigned);
void credentials_inherit(struct task_context *,const struct task *);
bool credentials_in_group(const struct task *,unsigned);
long credentials_posix_ids(struct task *,register_t *);
void credentials_exec_prepare(const struct task *,const struct vnode *,struct exec_credentials *);
void credentials_exec_commit(struct task *,const struct exec_credentials *);
unsigned credentials_uid(const struct task *);
unsigned credentials_gid(const struct task *);
unsigned credentials_fsuid(const struct task *);
unsigned credentials_fsgid(const struct task *);
bool credentials_fs_restricted(const struct task *);
bool credentials_restricted(const struct task *);
/* Ordinary native accounts use the host-independent IPv4 backend. Private
   browser/editor domains keep their existing network capability checks. */
bool credentials_native_network(const struct task *);
long credentials_ids(struct task *, register_t *);
int credentials_syscall(struct task *, const register_t *);
unsigned credentials_mode(const struct vnode *,unsigned);
int credentials_modified(struct vnode *);
int credentials_node(struct vnode *, unsigned);
int credentials_chown(struct vnode *,unsigned,unsigned);
int credentials_owner(struct vnode *);
int credentials_file(const struct file *);
void credentials_trace(struct task *, const register_t *, long);
#ifndef CONFIG_ZYGOTE_TRACE
#define CONFIG_ZYGOTE_TRACE 0
#endif
#if CONFIG_ZYGOTE_TRACE
unsigned credentials_zygote_record(const struct task *);
void credentials_zygote_syscall(const struct task *,const register_t *,uint64_t,long,bool);
#else
static inline unsigned credentials_zygote_record(const struct task *t){(void)t;return 0;}
static inline void credentials_zygote_syscall(const struct task *t,const register_t *r,uint64_t n,long v,bool b){(void)t;(void)r;(void)n;(void)v;(void)b;}
#endif
bool credentials_private_transition(struct task *);
#endif
