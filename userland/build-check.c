#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static _Thread_local int tls_value = 19;
static volatile sig_atomic_t child_notifications;
static volatile int shared_flag;
static volatile sig_atomic_t timer_notifications;
static volatile sig_atomic_t suspend_notifications, suspend_mask;
static volatile sig_atomic_t fault_notifications;
static void *fault_page;

static void child_signal(int signal) {
    if (signal == SIGCHLD)
        child_notifications++;
}

static void timer_signal(int signal) {
    if (signal == SIGALRM)
        timer_notifications++;
}

static void suspend_signal(int signal) {
    if (signal == SIGUSR1) {
        sigset_t mask;
        if (!sigprocmask(SIG_SETMASK, NULL, &mask)) {
            suspend_mask =
                (sigismember(&mask, SIGUSR1) ? 1 : 0) | (sigismember(&mask, SIGUSR2) ? 2 : 0) |
                (sigismember(&mask, SIGKILL) ? 4 : 0) | (sigismember(&mask, SIGSTOP) ? 8 : 0);
        }
        suspend_notifications++;
    }
}

static void exit_fault(int signal, siginfo_t *info, void *context) {
    ucontext_t *frame = context;
    int expected = signal == SIGFPE ? 0 : 6;
    if ((signal != SIGFPE && signal != SIGILL) || !info || !frame || info->si_signo != signal ||
        info->si_code != (signal == SIGFPE ? FPE_INTDIV : ILL_ILLOPN) ||
        info->si_addr != (void *)frame->uc_mcontext.gregs[REG_RIP] ||
        frame->uc_mcontext.gregs[REG_TRAPNO] != expected)
        _exit(36);
    _exit(0);
}

static void repair_fault(int signal, siginfo_t *info, void *context) {
    ucontext_t *frame = context;
    if (signal != SIGSEGV || !info || !frame || info->si_code != SEGV_ACCERR ||
        info->si_addr != fault_page || frame->uc_mcontext.gregs[REG_CR2] != (long)fault_page ||
        mprotect(fault_page, 4096, PROT_READ | PROT_WRITE))
        _exit(37);
    fault_notifications++;
}

#define CHECK(expression) do { if (!(expression)) { \
    printf("BUILD CHECK FAIL line=%d errno=%d: %s\n",__LINE__,errno,#expression); \
    return 1; } } while (0)

static int wait_exit(pid_t pid, int expected) {
    int status;
    while (waitpid(pid, &status, 0) == -1)
        if (errno != EINTR)
            return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == expected;
}

static int fpu_pause(uint64_t seed) {
    const uint64_t expected[2] = {seed, ~seed};
    uint64_t observed3[2], observed15[2];
    struct timespec delay = {0, 8000000};
    register long nr __asm__("rax") = SYS_nanosleep;
    register long time_arg __asm__("rdi") = (long)&delay;
    register long left_arg __asm__("rsi") = 0;
    __asm__ volatile("movdqu %[in],%%xmm3; movdqu %[in],%%xmm15; syscall; "
                     "movdqu %%xmm3,%[out3]; movdqu %%xmm15,%[out15]"
                     : "+a"(nr), "+D"(time_arg),
                       "+S"(left_arg), [out3] "=m"(observed3), [out15] "=m"(observed15)
                     : [in] "m"(expected)
                     : "rcx", "r11", "xmm3", "xmm15", "memory", "cc");
    return (nr == 0 || nr == -EINTR) && !memcmp(expected, observed3, sizeof(expected)) &&
           !memcmp(expected, observed15, sizeof(expected));
}

static int check_fpu_processes(void) {
    pid_t child = fork();
    CHECK(child>=0);
    if (!child) {
        for (unsigned i = 0; i < 24; i++)
            if (!fpu_pause(0x1122334400000000ULL + i))
                _exit(17);
        _exit(0);
    }

    for (unsigned i = 0; i < 24; i++)
        CHECK(fpu_pause(0x9988776600000000ULL+i));
    CHECK(wait_exit(child,0));
    puts(
        "BUILD FPU CHECK PASS: distinct user SSE state survives input/idle workers, competing processes and signal delivery");
    return 0;
}

static int check_signal_registers(void) {
    struct sigaction action = {.sa_handler = timer_signal};
    CHECK(!sigaction(SIGALRM,&action,NULL));
    timer_notifications = 0;
    struct itimerval timer = {.it_interval = {0, 5000}, .it_value = {0, 1000}};
    CHECK(!setitimer(ITIMER_REAL,&timer,NULL));
    unsigned corrupted;
    /* No syscall occurs in this hot loop. A timer-delivered handler must
       return all live registers, including the RCX/R11 consumed by SYSRET. */
    __asm__ volatile("movabs $0x123456789abcdef0,%%rcx\n\t"
                     "movabs $0xfedcba9876543210,%%r11\n\t"
                     "mov $100000000,%%edi\n\t"
                     "1: movabs $0x123456789abcdef0,%%rax\n\t"
                     "cmp %%rax,%%rcx\n\tjne 2f\n\t"
                     "movabs $0xfedcba9876543210,%%rax\n\t"
                     "cmp %%rax,%%r11\n\tjne 2f\n\t"
                     "dec %%edi\n\tjnz 1b\n\t"
                     "xor %%eax,%%eax\n\tjmp 3f\n\t"
                     "2: mov $1,%%eax\n\t3:"
                     : "=a"(corrupted)::"rcx", "r11", "rdi", "memory", "cc");
    timer = (struct itimerval){0};
    CHECK(!setitimer(ITIMER_REAL,&timer,NULL));
    CHECK(timer_notifications>0 && !corrupted);
    timer_notifications = 0;
    puts("BUILD REGISTER CHECK PASS: asynchronous signal return preserves live RCX/R11");
    return 0;
}

static int check_signal_suspend(void) {
    struct sigaction action = {.sa_handler = suspend_signal, .sa_flags = SA_RESTART}, old_action,
                     old_ignore;
    CHECK(!sigaction(SIGUSR1,&action,&old_action));
    action = (struct sigaction){.sa_handler = SIG_IGN};
    CHECK(!sigaction(SIGUSR2,&action,&old_ignore));
    sigset_t previous, blocked, temporary, current;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigaddset(&blocked, SIGUSR2);
    CHECK(!sigprocmask(SIG_BLOCK,&blocked,&previous));
    CHECK(!sigprocmask(SIG_SETMASK,NULL,&blocked));
    temporary = blocked;
    sigdelset(&temporary, SIGUSR1);
    sigaddset(&temporary, SIGKILL);
    sigaddset(&temporary, SIGSTOP);
    suspend_notifications = 0;
    suspend_mask = 0;
    CHECK(!raise(SIGUSR1) && !suspend_notifications);
    errno = 0;
    CHECK(sigsuspend(&temporary)==-1 && errno==EINTR);
    CHECK(suspend_notifications==1 && suspend_mask==3);
    CHECK(!sigprocmask(SIG_SETMASK,NULL,&current) && sigismember(&current,SIGUSR1)==1 &&
        sigismember(&current,SIGUSR2)==1);
    errno = 0;
    CHECK(syscall(SYS_rt_sigsuspend,&temporary,16)==-1 && errno==EINVAL);
    errno = 0;
    CHECK(syscall(SYS_rt_sigsuspend,(void *)1,8)==-1 && errno==EFAULT);
    sigdelset(&temporary, SIGUSR2);
    pid_t parent = getpid(), child = fork();
    CHECK(child>=0);
    if (!child) {
        usleep(10000);
        if (kill(parent, SIGUSR2))
            _exit(19); /* Ignored signal must not return. */
        usleep(10000);
        _exit(kill(parent, SIGUSR1) ? 20 : 0);
    }

    errno = 0;
    CHECK(sigsuspend(&temporary)==-1 && errno==EINTR);
    CHECK(suspend_notifications==2 && suspend_mask==1 && wait_exit(child,0));
    CHECK(!sigprocmask(SIG_SETMASK,NULL,&current) && sigismember(&current,SIGUSR1)==1 &&
        sigismember(&current,SIGUSR2)==1);
    CHECK(!raise(SIGUSR1));
    errno = 0;
    CHECK(ppoll(NULL,0,NULL,&temporary)==-1 && errno==EINTR);
    CHECK(suspend_notifications==3 && suspend_mask==1);
    CHECK(!sigprocmask(SIG_SETMASK,NULL,&current) && sigismember(&current,SIGUSR1)==1 &&
        sigismember(&current,SIGUSR2)==1);
    CHECK(!raise(SIGUSR1));
    errno = 0;
    CHECK(pselect(0,NULL,NULL,NULL,NULL,&temporary)==-1 && errno==EINTR);
    CHECK(suspend_notifications==4 && suspend_mask==1);
    CHECK(!sigprocmask(SIG_SETMASK,NULL,&current) && sigismember(&current,SIGUSR1)==1 &&
        sigismember(&current,SIGUSR2)==1);
    struct sigaction old_timer;
    action = (struct sigaction){.sa_handler = SIG_IGN};
    CHECK(!sigaction(SIGALRM,&action,&old_timer));
    struct itimerval ignored_timer = {.it_value = {0, 5000}};
    CHECK(!setitimer(ITIMER_REAL,&ignored_timer,NULL));
    struct timespec wait = {0, 20000000};
    CHECK(ppoll(NULL,0,&wait,&temporary)==0);
    ignored_timer = (struct itimerval){0};
    CHECK(!setitimer(ITIMER_REAL,&ignored_timer,NULL) && !sigaction(SIGALRM,&old_timer,NULL));
    sigdelset(&blocked, SIGUSR1);
    CHECK(!sigprocmask(SIG_SETMASK,&blocked,NULL));
    child = fork();
    CHECK(child>=0);
    if (!child) {
        usleep(50000);
        _exit(kill(parent, SIGUSR1) ? 21 : 0);
    }
    errno = 0;
    CHECK(pause()==-1 && errno==EINTR);
    CHECK(suspend_notifications==5 && wait_exit(child,0));
    CHECK(!sigprocmask(SIG_SETMASK,&previous,NULL));
    CHECK(!sigaction(SIGUSR1,&old_action,NULL) && !sigaction(SIGUSR2,&old_ignore,NULL));
    puts(
        "BUILD SUSPEND CHECK PASS: pending/delayed signals, handler and restored masks, ignored signals, ppoll/pselect/pause, no restart and checked arguments");
    return 0;
}

static int check_fault_signals(void) {
    for (int kind = 0; kind < 3; kind++) {
        pid_t child = fork();
        CHECK(child>=0);
        if (!child) {
            if (kind) {
                struct sigaction action = {.sa_sigaction = exit_fault, .sa_flags = SA_SIGINFO};
                if (sigaction(kind == 1 ? SIGFPE : SIGILL, &action, NULL))
                    _exit(38);
            }
            if (kind == 2)
                __asm__ volatile("ud2");
            else
                __asm__ volatile("mov $1,%%eax; xor %%edx,%%edx; xor %%ecx,%%ecx; div %%ecx" ::
                                     : "rax", "rdx", "rcx", "cc");
            _exit(39);
        }

        int status;
        CHECK(waitpid(child,&status,0)==child);
        if (!kind)
            CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGFPE);
        else
            CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    }

    pid_t child = fork();
    CHECK(child>=0);
    if (!child) {
        fault_page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        struct sigaction action = {.sa_sigaction = repair_fault, .sa_flags = SA_SIGINFO};
        if (fault_page == MAP_FAILED || sigaction(SIGSEGV, &action, NULL))
            _exit(40);
        fault_notifications = 0;
        *(volatile char *)fault_page =
            73; /* Handler fixes protection, IRET retries the actual instruction. */
        if (fault_notifications != 1 || *(volatile char *)fault_page != 73 ||
            munmap(fault_page, 4096))
            _exit(41);
        _exit(0);
    }

    CHECK(wait_exit(child,0));
    child = fork();
    CHECK(child>=0);
    if (!child) {
        struct sigaction action = {.sa_sigaction = repair_fault, .sa_flags = SA_SIGINFO};
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGSEGV);
        if (sigaction(SIGSEGV, &action, NULL) || sigprocmask(SIG_BLOCK, &blocked, NULL))
            _exit(42);
        *(volatile char *)0 = 1;
        _exit(43);
    }

    int status;
    CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
    puts(
        "BUILD FAULT CHECK PASS: actual divide/illegal-instruction handlers, signal metadata, repaired page retry and blocked-fault termination");
    return 0;
}

static int check_timers(void) {
    struct sigaction action = {.sa_handler = timer_signal};
    CHECK(!sigaction(SIGALRM,&action,NULL));
    struct itimerval timer = {{0, 25000}, {0, 20000}}, old, current;
    CHECK(!setitimer(ITIMER_REAL,&timer,&old) && !old.it_value.tv_sec && !old.it_value.tv_usec);
    CHECK(!getitimer(ITIMER_REAL,&current) && current.it_interval.tv_usec==25000 &&
        !current.it_value.tv_sec && current.it_value.tv_usec>0 && current.it_value.tv_usec<=20000);
    for (int i = 0; i < 100 && timer_notifications < 2; i++) {
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    CHECK(timer_notifications>=2);
    timer = (struct itimerval){0};
    CHECK(!setitimer(ITIMER_REAL,&timer,&old) && old.it_interval.tv_usec==25000);
    CHECK(!getitimer(ITIMER_REAL,&current) && !current.it_value.tv_sec && !current.it_value.tv_usec);
    errno = 0;
    CHECK(syscall(SYS_setitimer,ITIMER_REAL,(void *)1,NULL)==-1 && errno==EFAULT);
    timer.it_value.tv_usec = 1000000;
    errno = 0;
    CHECK(setitimer(ITIMER_REAL,&timer,NULL)==-1 && errno==EINVAL);
    errno = 0;
    CHECK(getitimer(ITIMER_VIRTUAL,&current)==-1 && errno==ENOTSUP);
    CHECK(!alarm(3));
    pid_t child = fork();
    CHECK(child>=0);
    if (!child)
        _exit(!getitimer(ITIMER_REAL, &current) && !current.it_value.tv_sec &&
                      !current.it_value.tv_usec
                  ? 0
                  : 15);
    CHECK(wait_exit(child,0));
    CHECK(alarm(0)==3);
    sigset_t mask, previous;
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    CHECK(!sigprocmask(SIG_BLOCK,&mask,&previous));
    timer = (struct itimerval){.it_value = {0, 10000}};
    CHECK(!setitimer(ITIMER_REAL,&timer,NULL));
    struct timespec delay = {0, 50000000};
    CHECK(!nanosleep(&delay,NULL));
    CHECK(!sigpending(&mask) && sigismember(&mask,SIGALRM));
    int before = timer_notifications;
    CHECK(!sigprocmask(SIG_SETMASK,&previous,NULL));
    CHECK(timer_notifications==before+1);
    puts(
        "BUILD TIMER CHECK PASS: real/periodic SIGALRM, mask/pending delivery, alarm cancellation, no fork inheritance and checked arguments");
    return 0;
}

static int check_devices(void) {
    int fd = open("/dev/null", O_RDWR);
    char data[16];
    CHECK(fd>=0 && write(fd,"discard",7)==7 && read(fd,data,sizeof(data))==0);
    errno = 0;
    CHECK(lseek(fd,0,SEEK_SET)==-1 && errno==ESPIPE);
    CHECK(!isatty(fd));
    struct pollfd probe = {fd, POLLIN | POLLOUT, 0};
    CHECK(poll(&probe,1,0)==1 && probe.revents==(POLLIN|POLLOUT));
    CHECK(!close(fd));
    fd = open("/dev/zero", O_RDWR);
    memset(data, 0xff, sizeof(data));
    CHECK(fd>=0 && read(fd,data,sizeof(data))==sizeof(data));
    for (unsigned i = 0; i < sizeof(data); i++)
        CHECK(!data[i]);
    CHECK(write(fd,"discard",7)==7 && !close(fd));
    errno = 0;
    CHECK(mkdir("/",0755)==-1 && errno==EEXIST);
    puts(
        "BUILD DEVICE CHECK PASS: null discard/EOF, zero fill, readiness, terminal/seek rejection and root mkdir semantics");
    return 0;
}

static int check_files(const char *base) {
    char path[512], link[512];
    snprintf(path, sizeof(path), "%s/build-check-file", base);
    snprintf(link, sizeof(link), "%s/build-check-link", base);
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd>=0 && write(fd,"abcd",4)==4);
    CHECK(!ftruncate(fd,8197));
    struct stat metadata;
    CHECK(!fstat(fd,&metadata) && metadata.st_size==8197);
    unsigned char data[32];
    CHECK(pread(fd,data,7,8190)==7);
    for (int i = 0; i < 7; i++)
        CHECK(!data[i]);
    CHECK(!ftruncate(fd,2) && !ftruncate(fd,8197));
    CHECK(pread(fd,data,4,0)==4 && !memcmp(data,"ab\0\0",4));
    CHECK(lseek(fd,0,SEEK_CUR)==4);
    struct timespec times[2] = {{1699920000, 0}, {1700000100, 0}};
    CHECK(!futimens(fd,times));
    CHECK(!fstat(fd,&metadata) && metadata.st_atim.tv_sec==1699920000 &&
        metadata.st_mtim.tv_sec==1700000100);
    times[0].tv_nsec = times[1].tv_nsec = UTIME_OMIT;
    CHECK(!futimens(fd,times));
    CHECK(!fstat(fd,&metadata) && metadata.st_mtim.tv_sec==1700000100);
    times[1].tv_nsec = 1000000000;
    errno = 0;
    CHECK(futimens(fd,times)==-1 && errno==EINVAL);
    errno = 0;
    CHECK(syscall(SYS_utimensat,fd,NULL,(void *)1,0)==-1 && errno==EFAULT);
    CHECK(!close(fd));
    if (!strncmp(base, "/tmp", 4)) {
        fd = open(link, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd>=0 && write(fd,"OLD",3)==3);
        errno = 0;
        CHECK(rename(path,link)==-1 && errno==EBUSY);
        CHECK(!close(fd));
        errno = 0;
        CHECK(syscall(SYS_renameat2,AT_FDCWD,path,AT_FDCWD,link,1)==-1 && errno==EEXIST);
        CHECK(!stat(path,&metadata) && metadata.st_size==8197);
        CHECK(!stat(link,&metadata) && metadata.st_size==3);
        errno = 0;
        CHECK(syscall(SYS_renameat2,AT_FDCWD,path,AT_FDCWD,link,2)==-1 && errno==ENOTSUP);
        CHECK(!rename(path,link));
        errno = 0;
        CHECK(stat(path,&metadata)==-1 && errno==ENOENT);
        CHECK(!stat(link,&metadata) && metadata.st_size==8197);
        CHECK(!rename(link,path));
        CHECK(!symlink("build-check-file",link));
        CHECK(!lstat(link,&metadata) && S_ISLNK(metadata.st_mode));
        CHECK(readlink(link,(char *)data,sizeof(data))==16 && !memcmp(data,"build-check-file",16));
        CHECK(!stat(link,&metadata) && S_ISREG(metadata.st_mode) && metadata.st_size==8197);
        CHECK(!unlink(link));
        CHECK(!symlink("build-check-link",link));
        errno = 0;
        CHECK(stat(link,&metadata)==-1 && errno==ELOOP);
        CHECK(!unlink(link));
    } else {
        errno = 0;
        CHECK(symlink("build-check-file",link)==-1 && errno==ENOTSUP);
    }
    CHECK(!truncate(path,0));
    CHECK(!stat(path,&metadata) && !metadata.st_size);
    CHECK(!unlink(path));
    puts(
        "BUILD FILE CHECK PASS: resize/zero fill, retained offsets, timestamps, pointer validation and symlink capabilities");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--registers"))
        return check_signal_registers();
    if (argc == 2 && !strcmp(argv[1], "--suspend"))
        return check_signal_suspend();
    if (argc == 2 && !strcmp(argv[1], "--faults"))
        return check_fault_signals();
    if (argc == 3 && !strcmp(argv[1], "--files"))
        return check_files(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--spawn-child")) {
        if (!getenv("BUILD_TEST") || strcmp(getenv("BUILD_TEST"), "native"))
            return 7;
        errno = 0;
        return fcntl(atoi(argv[2]), F_GETFD) == -1 && errno == EBADF ? 0 : 8;
    }

    CHECK(!check_timers());
    CHECK(!check_signal_registers());
    CHECK(!check_signal_suspend());
    CHECK(!check_fault_signals());
    CHECK(!check_devices());
    struct sigaction action = {.sa_handler = child_signal, .sa_flags = SA_RESTART};
    CHECK(!sigaction(SIGCHLD,&action,NULL));
    CHECK(!check_fpu_processes());
    int fds[2];
    CHECK(!pipe(fds));
    tls_value = 41;
    int private_value = 83;
    pid_t pid = fork();
    CHECK(pid>=0);
    if (!pid) {
        close(fds[0]);
        if (private_value != 83 || tls_value != 41)
            _exit(9);
        private_value = 99;
        tls_value = 77;
        if (write(fds[1], "child", 5) != 5)
            _exit(10);
        close(fds[1]);
        _exit(23);
    }

    CHECK(!close(fds[1]));
    char bytes[16];
    CHECK(read(fds[0],bytes,sizeof(bytes))==5 && !memcmp(bytes,"child",5));
    CHECK(read(fds[0],bytes,sizeof(bytes))==0);
    CHECK(!close(fds[0]));
    CHECK(wait_exit(pid,23));
    CHECK(private_value==83 && tls_value==41 && child_notifications);

    int fd = open("/tmp/build-check-offset", O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd>=0 && write(fd,"abcd",4)==4 && lseek(fd,0,SEEK_SET)==0);
    pid = fork();
    CHECK(pid>=0);
    if (!pid)
        _exit(read(fd, bytes, 2) == 2 && !memcmp(bytes, "ab", 2) ? 0 : 11);
    CHECK(wait_exit(pid,0));
    CHECK(read(fd,bytes,2)==2 && !memcmp(bytes,"cd",2));
    CHECK(!fcntl(fd,F_SETFD,FD_CLOEXEC));
    char number[16];
    snprintf(number, sizeof(number), "%d", fd);
    char *args[] = {"build-check", "--spawn-child", number, NULL};
    char *environment[] = {"BUILD_TEST=native", "PATH=/bin", NULL};
    CHECK(!posix_spawn(&pid,"/bin/build-check",NULL,NULL,args,environment));
    CHECK(wait_exit(pid,0));
    CHECK(!close(fd) && !unlink("/tmp/build-check-offset"));
    CHECK(posix_spawn(&pid,"/missing/build-check",NULL,NULL,args,environment)==ENOENT);

    shared_flag = 0;
    pid = vfork();
    CHECK(pid>=0);
    if (!pid) {
        shared_flag = 1;
        /* A borrowed VM cannot be changed before exec. */
        if (syscall(SYS_mmap, 0, 4096, 3, 0x22, -1, 0) != -1 || errno != ENOTSUP)
            _exit(12);
        _exit(29);
    }

    CHECK(shared_flag==1 && wait_exit(pid,29));
    void *guard = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(guard!=MAP_FAILED);
    pid = fork();
    CHECK(pid>=0);
    if (!pid) {
        *(volatile char *)guard = 1;
        _exit(13);
    }
    int status;
    CHECK(waitpid(pid,&status,0)==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
    CHECK(!munmap(guard,4096));
    errno = 0;
    CHECK(syscall(SYS_clone,0,NULL,NULL,NULL,NULL)==-1 && errno==ENOTSUP);
    errno = 0;
    CHECK(syscall(SYS_clone,0x4111,(void *)1,NULL,NULL,NULL)==-1 && errno==EFAULT);
    for (int i = 0; i < 24; i++) {
        pid = fork();
        CHECK(pid>=0);
        if (!pid)
            _exit(i);
        CHECK(wait_exit(pid,i));
    }

    puts("BUILD CHECK PASS: fork isolation/TLS/guards, shared offsets, SIGCHLD, vfork ordering, "
         "posix_spawn/exec errors and close-on-exec");
    return 0;
}
