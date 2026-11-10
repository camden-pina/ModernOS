#ifndef SCHED_H
#define SCHED_H
#include <kernel.h>
#include <boot/smp.h>
#include <cpu.h>
#include <sync.h>
#include <mm/user.h>
#include <fs/vfs.h>
#include <userns.h>

/* App domain206 plus the existing32 slots for kernel/native helpers. */
#define TASK_MAX 238
#define TASK_STACK_PAGES 16

enum task_state { TASK_UNUSED, TASK_NEW, TASK_RUNNABLE, TASK_RUNNING, TASK_SLEEPING, TASK_STOPPED, TASK_ZOMBIE };
struct task_context {
    unsigned fork_owner;
    /* Linux real/effective/saved/filesystem identities. */
    unsigned uid, gid;
    unsigned ruid, suid, fsuid, rgid, sgid, fsgid;
    unsigned group_count, groups[32];
    unsigned desktop_login_uid, pending_login_uid, pending_login_generation;
    unsigned account_session, account_owner;
    bool account_console;
    unsigned desktop_fsuid; /* Root broker applies DAC for personal file actions. */
    unsigned account_uid; /* Guest login identity, retained across sudo/exec. */
    bool account_nnp;
    /* Granted only by the root-owned Chromium display bridge. Namespace
       renderers never receive native network access, even when inherited. */
    bool browser_network;
    bool gid_dropped;
    unsigned references, live, tgid, creation_mask;
    unsigned pgid, sid;
    bool job_managed, executed;
    /* Explicit root supervisor adoption; not inherited by forked contexts. */
    bool child_subreaper;
    /* A controller may reclaim only the exact terminal/group it handed off.
       This owns a VFS reference; fork never inherits the capability. */
    struct file *job_terminal;
    /* One retained loader file per process context; threads share this owner. */
    struct file *executable;
    unsigned job_foreground;
    struct os_termios job_termios;
    /* Group stops are requested asynchronously, completed only at safe user
       boundaries. wait events do not imply process death or resource reaping. */
    unsigned stop_requested, stop_signal;
    bool stopped, stop_event, continue_event;
    bool exiting;
    int exit_status;
    uint64_t user_ticks, system_ticks;
    uint64_t child_user_ticks, child_system_ticks, real_timer_at, real_timer_interval;
    uint64_t child_peak_pages;
    mutex_t syscall_lock;
    unsigned web_role, web_domain;
    char web_origin[256];
    struct memory_account *memory_account;
    char cwd[OS_PATH_MAX];
    struct file *fds[OS_FD_MAX];
    unsigned fd_flags[OS_FD_MAX];
    struct os_sigaction signal_actions[64];
};

struct task {
    struct generic_seccomp_filter *seccomp_filter;
    unsigned seccomp_mode, seccomp_signal_frames;
    struct userns_credential *userns;
    struct userns_budget_claim userns_budget_claim;
    mutex_t *waiting_mutex;
    unsigned pid, tgid, ppid, cpu;
    enum task_state state;
    bool user, idle;
    unsigned held_mutexes, no_park_depth, power_role;
    struct task *power_creator;
    int exit_status;
    unsigned kill_signal;
    uint64_t signal_pending, signal_mask, clear_tid;
    struct task_context *context;
    struct os_sigaction *signal_actions;
    bool syscall_locked, futex_waiting;
    uint64_t futex_root, futex_address;
    uint32_t futex_mask;
    int futex_result;
    struct {
        register_t frame;
        uint64_t mask, stack;
        uint8_t fpu[512] __attribute__((aligned(16)));
    } signal_frames[4];
    unsigned signal_depth, environment_count, vfork_parent;
    char *environment[OS_ENV_MAX + 1], *environment_data;
    char name[64], *cwd;
    bool browser_sandbox, browser_local_file;
    char browser_home[OS_PATH_MAX], browser_local[OS_PATH_MAX];
    uint64_t wake_at, switches, runtime_ticks, stack_phys, stack_top, fsbase;
    uint64_t user_ticks, system_ticks;
    register_t *frame;
    struct task *next;
    void (*entry)(void *);
    void *argument;
    struct address_space space;
    struct address_space borrowed_space;
    struct file **fds;
    unsigned *fd_flags;
    /* A syscall retains descriptors across waits and concurrent close/reuse. */
    struct file *file_pins[OS_FD_MAX];
    unsigned pin_count;
    /* Fits the existing alignment gap before the saved FPU state. */
    struct landlock_domain *landlock_domain;
    uint8_t fpu[512] __attribute__((aligned(16)));
};

static inline unsigned task_pending_signal(const struct task *t) {
    return __atomic_load_n(&t->kill_signal, __ATOMIC_ACQUIRE);
}

struct cpu_local {
    struct cpu_local *self; /* GS:0 */
    uintptr_t kernel_stack; /* GS:8: SYSCALL stack */
    uintptr_t user_stack;   /* GS:16: SYSCALL scratch */
    uintptr_t handoff;      /* GS:24: scheduler lock held across stack switch */
    unsigned id, lapic_id;
    bool online;
    unsigned held_mutexes, no_park_depth, irq_depth;
    uint64_t park_seen_epoch, park_ack_epoch, resume_ack_epoch, commit_ack_epoch;
    struct task *current, *head, *tail;
    uint64_t ticks, switches;
    unsigned fpu_pid;
    struct task *fpu_task;
    struct task idle;
} __attribute__((aligned(64)));

static inline struct cpu_local *this_cpu(void) {
    struct cpu_local *c;
    __asm__ volatile("mov %%gs:0,%0" : "=r"(c));
    return c;
}

void sched_init(struct boot_smp *smp);
void sched_start(void);
bool sched_ready(void);
unsigned sched_cpu_count(void);
struct cpu_local *sched_cpu(unsigned);
bool sched_power_drained(void);
register_t *sched_interrupt(register_t *frame);
void sched_finish_switch(void);
void sched_yield(void);
void sched_sleep(uint64_t us);
void task_exit(int status);
void task_exit_group(int status);
long task_subreaper(struct task *,register_t *);
void task_reap_threads(void);
void task_context_bind(struct task *, struct task_context *);
struct file *task_file_pin(struct task *, unsigned fd);
void task_files_unpin(struct task *);
long task_futex(struct task *, register_t *);
struct task *task_allocate(const char *name, bool user, unsigned ppid, int cpu);
void task_discard(struct task *task);
int task_publish(struct task *task);
int task_publish_s1(struct task *,struct task *);
int task_publish_thread_p1(struct task *,struct task *,uint64_t,uint64_t);
int task_publish_private_fork(struct task *,struct task *,unsigned);
bool private_fork_fault(unsigned,unsigned);
void private_fork_scope_selftest(void);
void private_fork_barrier_selftest(void);
int task_kernel(const char *name, void (*entry)(void *), void *arg, unsigned cpu);
int task_wait(int pid, int *status, unsigned options);
int task_wait_usage(int pid, int *status, unsigned options, struct os_rusage *usage, unsigned *child_sid,unsigned *view_pid);
int task_wait_exit(int *status,struct os_rusage *,unsigned *view_pid,unsigned *view_uid,unsigned *child_sid,bool nonblock);
void task_cpu_usage(struct task *, bool children, uint64_t *user_us, uint64_t *system_us);
void task_thread_cpu_usage(struct task *, uint64_t *user_us, uint64_t *system_us);
long task_sched_query(struct task *, int pid);
int task_kill(unsigned pid, unsigned signal);
void task_account_revoke(unsigned uid);
int task_kill_group(unsigned group, unsigned signal);
int task_signal_foreground(unsigned identity, unsigned signal);
unsigned task_input_identity(const struct task *);
long task_getpgid(struct task *, int pid, bool session);
int task_setpgid(struct task *, int pid, int group);
long task_private_setsid(struct task *);
long task_terminal_set(struct task *, struct file *, unsigned group);
bool task_terminal_access(struct task *, struct file *);
int task_terminal_cwd(struct file *, unsigned, char [OS_PATH_MAX]);
int task_tgkill(unsigned tgid, unsigned tid, unsigned signal);
size_t sched_info(char *out, size_t cap, bool processes);
bool sched_path_busy(const char *path);
void syscall_cpu_init(void);
void syscall_dispatch(register_t *frame);
void process_cleanup(struct task *task);
int process_spawn(const char *path, char *const argv[], unsigned ppid, const char *cwd, int cpu,
                  bool foreground);
int process_spawn_streams(const char *path, char *const argv[], unsigned ppid, const char *cwd,
                          int cpu, bool foreground, struct file *const streams[3]);
int process_exec(struct task *task, const char *path, char *const argv[], register_t *frame);
int process_exec_env(struct task *, const char *, char *const[], char *const[], register_t *);
int process_environment_set(struct task *, char *const[]);
void task_release_input(struct task *, struct file *);
long process_clone(struct task *, register_t *, uint64_t flags, uint64_t stack);
void task_vfork_wait(unsigned pid);
void task_real_timer(struct task *, bool set, uint64_t value, uint64_t interval,
                     struct os_itimerval *old);
int process_spawn_env(const char *, char *const[], char *const[], unsigned, const char *, int, bool,
                      struct file *const[3]);
int process_spawn_browser(const char *, char *const[], unsigned, const char *,
                          struct file *const[3], const char *);
int process_spawn_web(const char *, char *const[], unsigned, const char *, struct file *const[3],
                      unsigned, unsigned, const char *);
int task_browser_grant(unsigned parent, unsigned pid, const char *path);
void task_stop_request_locked(struct task *, unsigned signal);
bool task_stop_checkpoint(struct task *t);
void task_signal_checkpoint(struct task *, register_t *, bool restart, uint64_t return_mask);
int task_background_terminal(struct task *, unsigned signal);
bool signal_pending(struct task *);
bool signal_deliver(struct task *, register_t *, bool restart, uint64_t return_mask);
bool signal_fault(struct task *, register_t *, unsigned signal, int code, uint64_t address);
long signal_suspend(struct task *, uint64_t mask);
long signal_return(struct task *, register_t *);
/* Caller holds vfs_mutex. No arbitrary-PID executable query is exposed. */
long process_executable_readlink(const struct task *, const char *, void *, size_t);
#endif
