#ifndef SCHED_H
#define SCHED_H
#include <kernel.h>
#include <boot/smp.h>
#include <cpu.h>
#include <sync.h>
#include <mm/user.h>
#include <fs/vfs.h>

#define TASK_MAX 128
#define TASK_STACK_PAGES 16

enum task_state { TASK_UNUSED, TASK_NEW, TASK_RUNNABLE, TASK_RUNNING, TASK_SLEEPING, TASK_ZOMBIE };
struct task_context {
    unsigned references, live, tgid, creation_mask;
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
    unsigned pid, tgid, ppid, cpu;
    enum task_state state;
    bool user, idle;
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
register_t *sched_interrupt(register_t *frame);
void sched_finish_switch(void);
void sched_yield(void);
void sched_sleep(uint64_t us);
void task_exit(int status);
void task_exit_group(int status);
void task_reap_threads(void);
void task_context_bind(struct task *, struct task_context *);
struct file *task_file_pin(struct task *, unsigned fd);
void task_files_unpin(struct task *);
long task_futex(struct task *, register_t *);
struct task *task_allocate(const char *name, bool user, unsigned ppid, int cpu);
void task_discard(struct task *task);
void task_publish(struct task *task);
int task_kernel(const char *name, void (*entry)(void *), void *arg, unsigned cpu);
int task_wait(int pid, int *status, unsigned options);
int task_wait_usage(int pid, int *status, unsigned options, struct os_rusage *usage);
void task_cpu_usage(struct task *, bool children, uint64_t *user_us, uint64_t *system_us);
void task_thread_cpu_usage(struct task *, uint64_t *user_us, uint64_t *system_us);
int task_kill(unsigned pid, unsigned signal);
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
bool signal_pending(struct task *);
bool signal_deliver(struct task *, register_t *, bool restart, uint64_t return_mask);
bool signal_fault(struct task *, register_t *, unsigned signal, int code, uint64_t address);
long signal_suspend(struct task *, uint64_t mask);
long signal_return(struct task *, register_t *);
#endif
