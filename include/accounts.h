#ifndef COAST_ACCOUNTS_H
#define COAST_ACCOUNTS_H
#include <sched.h>
long accounts_call(struct task *, register_t *);
long accounts_prctl(struct task *, const register_t *);
bool accounts_console(const struct task *, const struct file *);
void accounts_broker_commit(struct task *, const char *, const struct file *);
bool accounts_terminal(const struct task *, const struct file *);
bool accounts_window_client(const struct task *);
#endif
