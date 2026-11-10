#ifndef COAST_USERNS_TEST_H
#define COAST_USERNS_TEST_H
#include <kernel.h>
#ifndef CONFIG_USERNS_TEST
#define CONFIG_USERNS_TEST 0
#endif
#ifndef CONFIG_NO_NEW_PRIVS_TEST
#define CONFIG_NO_NEW_PRIVS_TEST 0
#endif
#ifndef CONFIG_PRIVATE_FORK_TEST
#define CONFIG_PRIVATE_FORK_TEST 0
#endif
#ifndef CONFIG_PRIVATE_FORK_VERIFY
#define CONFIG_PRIVATE_FORK_VERIFY 0
#endif
struct task;struct userns_credential;struct user_budget;struct mount;struct vnode;struct file;
uint64_t userns_file_origin(const struct task *);
#define C2_CLONE_NEWUSER 0x10000000ULL
struct userns_budget_claim { void *domain; bool active; };
bool userns_enabled(const struct task *);
bool userns_admitted_account(const struct task *);
bool userns_browser_network(const struct task *);
bool userns_newuser_clone(const struct task *, uint64_t);
int userns_enroll(struct task *);
int userns_inherit(struct task *,const struct task *,bool thread,bool new_namespace);
int userns_inherit_fs(struct task *,const struct task *,bool,bool,bool);
void userns_task_live(struct task *,bool);
void userns_task_free(struct task *);
void userns_exec_commit(struct task *);
unsigned userns_uid_view(const struct task *,unsigned);
unsigned userns_gid_view(const struct task *,unsigned);
long userns_call(struct task *,register_t *);
int userns_budget_begin(struct task *,struct user_budget *,struct userns_budget_claim *);
struct user_budget *userns_budget_existing(struct task *);
void userns_budget_finish(struct userns_budget_claim *,bool);
bool userns_proc_mount(struct mount *);
bool userns_proc_node(const struct vnode *);
bool userns_proc_write_path(const char *);
int userns_proc_file(const struct file *);
bool userns_fork_freeze(struct task *);
bool task_native_spawn_freeze(struct task *);
bool userns_single_live(struct task *);
void userns_fork_thaw(struct task *);
#endif
