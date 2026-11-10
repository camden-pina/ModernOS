#ifndef COAST_LANDLOCK_H
#define COAST_LANDLOCK_H
#include <kernel.h>
struct task;
struct vnode;
struct landlock_ruleset;
struct landlock_domain;
#define LL_EXECUTE (1ULL<<0)
#define LL_WRITE_FILE (1ULL<<1)
#define LL_READ_FILE (1ULL<<2)
#define LL_READ_DIR (1ULL<<3)
#define LL_REMOVE_DIR (1ULL<<4)
#define LL_REMOVE_FILE (1ULL<<5)
#define LL_MAKE_CHAR (1ULL<<6)
#define LL_MAKE_DIR (1ULL<<7)
#define LL_MAKE_REG (1ULL<<8)
#define LL_MAKE_SOCK (1ULL<<9)
#define LL_MAKE_FIFO (1ULL<<10)
#define LL_MAKE_BLOCK (1ULL<<11)
#define LL_MAKE_SYM (1ULL<<12)
#define LL_REFER (1ULL<<13)
#define LL_TRUNCATE (1ULL<<14)
#define LL_FS_ALL ((1ULL<<15)-1)
#define LL_MAX_LAYERS 16
struct landlock_walk {
    unsigned count;
    const struct landlock_domain *layers[LL_MAX_LAYERS];
    uint64_t allowed[LL_MAX_LAYERS];
};
long landlock_call(struct task *,register_t *);
bool landlock_active(void);
void landlock_task_inherit(struct task *,const struct task *);
void landlock_task_release(struct task *);
void landlock_ruleset_release(struct landlock_ruleset *);
void landlock_walk_begin(struct landlock_walk *);
void landlock_walk_visit(struct landlock_walk *,const struct vnode *);
int landlock_walk_check(const struct landlock_walk *,uint64_t);
int landlock_path_access(const char *,uint64_t,bool);
int landlock_parent_access(const char *,uint64_t);
int landlock_reparent(const char *,const char *,const struct vnode *);
uint64_t landlock_make_access(const struct vnode *);
#endif
