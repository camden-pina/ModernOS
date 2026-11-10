#ifndef COAST_FILESYSTEM_H
#define COAST_FILESYSTEM_H
#include <kernel.h>
#ifndef CONFIG_FILESYSTEM_SANDBOX_TEST
#define CONFIG_FILESYSTEM_SANDBOX_TEST 0
#endif
#ifndef CONFIG_FILESYSTEM_SANDBOX_VERIFY
#define CONFIG_FILESYSTEM_SANDBOX_VERIFY 0
#endif
struct task;struct vnode;
int s1_clone_check(struct task *,uint64_t);
int s1_publish(struct task *,struct task *);
void s1_clone_abort(struct task *);
void s1_helper_exit(struct task *);
bool s1_borrowed(const struct task *);
bool s1_restricted(const struct task *);
int s1_cwd_check(const struct task *);
unsigned s1_mask(const struct task *,bool,unsigned);
long s1_chroot(struct task *,uint64_t);
void s1_chdir(struct task *,const char *,const struct vnode *);
int s1_fchdir(struct task *,const struct vnode *,const char *);
/* VFS mutex held; return1 handled or0 ordinary global root. */
int s1_root(struct vnode *);
bool s1_root_matches(const struct vnode *);
/* At-lookup: return1 not intercepted,0 success, negative error. */
int s1_at_lookup(struct task *,int,const char *,bool,bool,struct vnode *);
int s1_at_base(struct task *,int,const char *,const char **);
bool s1_fd_magic(const struct vnode *);
bool s1_fault(struct task *,unsigned);
int s1_pointers(const struct task *,uint64_t,uint64_t);
void s1_verify_live(struct task *,bool);
#endif
