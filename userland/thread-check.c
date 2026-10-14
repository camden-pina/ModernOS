#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL thread-check:%d: %s (errno=%d)\n",__LINE__,#x,errno); exit(1); } } while (0)

enum {
    F_WAIT = 0,
    F_WAKE = 1,
    F_REQUEUE = 3,
    F_CMP_REQUEUE = 4,
    F_PRIVATE = 128,
    F_REALTIME = 256,
    F_WAIT_BITSET = 9,
    F_WAKE_BITSET = 10
};

static long futex(atomic_int *word, int op, int value, const void *timeout, atomic_int *other,
                  int mask) {
    return syscall(SYS_futex, word, op, value, timeout, other, mask);
}

static int tid(void) {
    return syscall(SYS_gettid);
}

static struct timespec after(int clock, long ms) {
    struct timespec ts;
    CHECK(clock_gettime(clock,&ts)==0);
    ts.tv_nsec += ms * 1000000;
    ts.tv_sec += ts.tv_nsec / 1000000000;
    ts.tv_nsec %= 1000000000;
    return ts;
}

static void futex_errors(void) {
    atomic_int word = 0;
    errno = 0;
    CHECK(futex(&word,F_WAIT|F_PRIVATE,1,NULL,NULL,0)==-1 && errno==EAGAIN);
    errno = 0;
    CHECK(futex((void *)((char *)&word+1),F_WAIT,0,NULL,NULL,0)==-1 && errno==EINVAL);
    errno = 0;
    CHECK(futex((void *)0x12000,F_WAIT,0,NULL,NULL,0)==-1 && errno==EFAULT);
    struct timespec invalid = {0, 1000000000};
    errno = 0;
    CHECK(futex(&word,F_WAIT,0,&invalid,NULL,0)==-1 && errno==EINVAL);
    errno = 0;
    CHECK(futex(&word,F_WAIT_BITSET,0,NULL,NULL,0)==-1 && errno==EINVAL);
    struct timespec relative = {0, 3000000};
    errno = 0;
    CHECK(futex(&word,F_WAIT,0,&relative,NULL,0)==-1 && errno==ETIMEDOUT);
    struct timespec absolute = after(CLOCK_MONOTONIC, 3);
    errno = 0;
    CHECK(futex(&word,F_WAIT_BITSET,0,&absolute,NULL,1)==-1 && errno==ETIMEDOUT);
    absolute = after(CLOCK_REALTIME, 3);
    errno = 0;
    CHECK(futex(&word,F_WAIT_BITSET|F_REALTIME,0,&absolute,NULL,1)==-1 && errno==ETIMEDOUT);
    errno = 0;
    CHECK(futex(&word,6,0,NULL,NULL,0)==-1 && (errno==ENOTSUP || errno==ENOSYS));
    puts("PASS futex validation, relative/absolute deadlines and unsupported PI");
}

static void mapping_resize(void) {
    const size_t page = 4096;
    unsigned char *map =
        mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(map!=MAP_FAILED);
    memset(map, 0x5a, 4 * page);
    CHECK(mremap(map,4*page,page,0)==map);
    CHECK(mremap(map,page,3*page,0)==map);
    CHECK(map[0]==0x5a && map[page-1]==0x5a);
    for (size_t i = page; i < 3 * page; i++)
        CHECK(map[i]==0);
    unsigned char *guard = mmap(map + 3 * page, page, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(guard==map+3*page);
    guard[0] = 0x73;
    errno = 0;
    CHECK(mremap(map,3*page,4*page,0)==MAP_FAILED && errno==ENOMEM);
    CHECK(map[0]==0x5a && guard[0]==0x73);
    errno = 0;
    CHECK(mremap(map,3*page,4*page,MREMAP_MAYMOVE)==MAP_FAILED && errno==ENOTSUP);
    CHECK(munmap(map,4*page)==0);
    puts("PASS in-place mremap growth, shrink, zero-fill and occupied-page rollback");
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int arrived, release_workers, counter;
static pid_t owner;
static int ids[4];
static uintptr_t tls_addresses[4], map_addresses[4];
static _Thread_local int local = 19;

static void *worker(void *argument) {
    int id = (intptr_t)argument;
    CHECK(getpid()==owner && tid()!=owner && local==19);
    ids[id] = tid();
    tls_addresses[id] = (uintptr_t)&local;
    local = id + 100;
    unsigned char *map =
        mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(map!=MAP_FAILED);
    map_addresses[id] = (uintptr_t)map;
    memset(map, id + 1, 65536);
    CHECK(pthread_mutex_lock(&lock)==0);
    arrived++;
    CHECK(pthread_cond_broadcast(&condition)==0);
    while (!release_workers)
        CHECK(pthread_cond_wait(&condition,&lock)==0);
    CHECK(pthread_mutex_unlock(&lock)==0);
    for (int i = 0; i < 500; i++) {
        CHECK(pthread_mutex_lock(&lock)==0);
        counter++;
        CHECK(pthread_mutex_unlock(&lock)==0);
        if (!(i % 31))
            sched_yield();
    }

    for (int i = 0; i < 65536; i++)
        CHECK(map[i]==id+1);
    CHECK(local==id+100);
    CHECK(munmap(map,65536)==0);
    return (void *)(intptr_t)(id + 1);
}

static void pthreads(void) {
    pthread_t threads[4];
    owner = getpid();
    pthread_attr_t main_attr;
    void *stack;
    size_t size;
    CHECK(pthread_getattr_np(pthread_self(),&main_attr)==0);
    CHECK(pthread_attr_getstack(&main_attr,&stack,&size)==0 && size>=7*1024*1024);
    CHECK((uintptr_t)&main_attr>=(uintptr_t)stack && (uintptr_t)&main_attr<(uintptr_t)stack+size);
    CHECK(pthread_attr_destroy(&main_attr)==0);
    for (int i = 0; i < 4; i++)
        CHECK(pthread_create(&threads[i],NULL,worker,(void *)(intptr_t)i)==0);
    CHECK(pthread_mutex_lock(&lock)==0);
    while (arrived != 4)
        CHECK(pthread_cond_wait(&condition,&lock)==0);
    release_workers = 1;
    CHECK(pthread_cond_broadcast(&condition)==0);
    CHECK(pthread_mutex_unlock(&lock)==0);
    for (int i = 0; i < 4; i++) {
        void *result;
        CHECK(pthread_join(threads[i],&result)==0);
        CHECK((intptr_t)result==i+1);
        for (int j = 0; j < i; j++) {
            CHECK(ids[i]!=ids[j]);
            CHECK(tls_addresses[i]!=tls_addresses[j]);
            CHECK(map_addresses[i]!=map_addresses[j]);
        }
    }

    CHECK(counter==2000 && local==19);
    CHECK(pthread_mutex_lock(&lock)==0);
    struct timespec timeout = after(CLOCK_REALTIME, 5);
    CHECK(pthread_cond_timedwait(&condition,&lock,&timeout)==ETIMEDOUT);
    CHECK(pthread_mutex_unlock(&lock)==0);
    puts("PASS musl TLS, IDs, mutexes, conditions, mapping coherence and joins");
}

static int pipefd[2], replacement;

static void *replace_reader(void *unused) {
    (void)unused;
    usleep(15000);
    CHECK(close(pipefd[0])==0);
    replacement = open("/dev/zero", O_RDONLY);
    CHECK(replacement==pipefd[0]);
    CHECK(write(pipefd[1],"P",1)==1);
    return NULL;
}

static void descriptor_lifetime(void) {
    CHECK(pipe(pipefd)==0);
    pthread_t thread;
    CHECK(pthread_create(&thread,NULL,replace_reader,NULL)==0);
    char value = 0;
    CHECK(read(pipefd[0],&value,1)==1 && value=='P');
    CHECK(pthread_join(thread,NULL)==0);
    CHECK(read(replacement,&value,1)==1 && value==0);
    CHECK(close(replacement)==0 && close(pipefd[1])==0);
    puts("PASS shared descriptor table and blocked read across close/reuse");
}

static sem_t ready;
static atomic_int observed_signal, signal_done, signal_tid;

static void handler(int signal) {
    (void)signal;
    atomic_store(&observed_signal, tid());
}

static void *signal_worker(void *unused) {
    (void)unused;
    atomic_store(&signal_tid, tid());
    CHECK(sem_post(&ready)==0);
    while (!atomic_load(&signal_done))
        usleep(1000);
    return NULL;
}

static void directed_signal(void) {
    CHECK(sem_init(&ready,0,0)==0);
    struct sigaction action = {.sa_handler = handler};
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR2,&action,NULL)==0);
    pthread_t thread;
    CHECK(pthread_create(&thread,NULL,signal_worker,NULL)==0);
    CHECK(sem_wait(&ready)==0);
    errno = 0;
    CHECK(syscall(SYS_tgkill,owner+10000,atomic_load(&signal_tid),SIGUSR2)==-1 && errno==ESRCH);
    CHECK(pthread_kill(thread,SIGUSR2)==0);
    for (int i = 0; i < 100 && !atomic_load(&observed_signal); i++)
        usleep(1000);
    CHECK(atomic_load(&observed_signal)==atomic_load(&signal_tid));
    atomic_store(&signal_done, 1);
    CHECK(pthread_join(thread,NULL)==0);
    CHECK(sem_destroy(&ready)==0);
    puts("PASS thread-directed signals, TGID checks and semaphores");
}

static atomic_int detached_count;

static void *small_worker(void *argument) {
    if (argument)
        atomic_fetch_add(&detached_count, 1);
    return argument;
}

static void recycling(void) {
    for (int i = 0; i < 180; i++) {
        pthread_t thread;
        CHECK(pthread_create(&thread,NULL,small_worker,NULL)==0);
        CHECK(pthread_join(thread,NULL)==0);
    }
    pthread_attr_t attr;
    CHECK(pthread_attr_init(&attr)==0);
    CHECK(pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED)==0);
    for (int i = 0; i < 50; i++) {
        pthread_t thread;
        CHECK(pthread_create(&thread,&attr,small_worker,(void *)1)==0);
    }
    CHECK(pthread_attr_destroy(&attr)==0);
    for (int i = 0; i < 200 && atomic_load(&detached_count) != 50; i++)
        usleep(1000);
    CHECK(atomic_load(&detached_count)==50);
    usleep(20000);
    puts("PASS task reuse beyond TASK_MAX, detached stack teardown and TLS cleanup");
}

static atomic_int blocked_word;

static void private_futex_domains(void) {
    int ends[2];
    CHECK(pipe(ends)==0);
    pid_t child = fork();
    CHECK(child>=0);
    if (!child) {
        close(ends[0]);
        CHECK(write(ends[1],"R",1)==1);
        struct timespec timeout = {0, 30000000};
        errno = 0;
        CHECK(futex(&blocked_word,F_WAIT,0,&timeout,NULL,0)==-1 && errno==ETIMEDOUT);
        CHECK(write(ends[1],"T",1)==1);
        _exit(0);
    }

    close(ends[1]);
    char value;
    CHECK(read(ends[0],&value,1)==1 && value=='R');
    usleep(5000);
    CHECK(futex(&blocked_word,F_WAKE,INT_MAX,NULL,NULL,0)==0);
    CHECK(futex(&blocked_word,F_WAKE|F_PRIVATE,INT_MAX,NULL,NULL,0)==0);
    CHECK(read(ends[0],&value,1)==1 && value=='T');
    close(ends[0]);
    int status;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    puts("PASS private VM futex domains across fork with matching virtual addresses");
}

static void *blocked_worker(void *unused) {
    (void)unused;
    CHECK(sem_post(&ready)==0);
    for (;;)
        futex(&blocked_word, F_WAIT | F_PRIVATE, 0, NULL, NULL, 0);
    return NULL;
}

static void *last_worker(void *argument) {
    usleep(10000);
    CHECK(write((int)(intptr_t)argument,"L",1)==1);
    return NULL;
}

static void *faulting_worker(void *unused) {
    (void)unused;
    *(volatile int *)(uintptr_t)0x12000 = 42;
    return NULL;
}

static void group_lifetime(void) {
    /* fork is deliberately tested only after joined threads have exited. */
    pid_t child = fork();
    CHECK(child>=0);
    if (!child) {
        CHECK(sem_init(&ready,0,0)==0);
        pthread_t threads[3];
        for (int i = 0; i < 3; i++)
            CHECK(pthread_create(&threads[i],NULL,blocked_worker,NULL)==0);
        for (int i = 0; i < 3; i++)
            CHECK(sem_wait(&ready)==0);
        _exit(27);
    }

    int status;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==27);
    child = fork();
    CHECK(child>=0);
    if (!child) {
        CHECK(sem_init(&ready,0,0)==0);
        pthread_t waiting, crashing;
        CHECK(pthread_create(&waiting,NULL,blocked_worker,NULL)==0);
        CHECK(sem_wait(&ready)==0);
        CHECK(pthread_create(&crashing,NULL,faulting_worker,NULL)==0);
        for (;;)
            pause();
    }

    CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
    int ends[2];
    CHECK(pipe(ends)==0);
    child = fork();
    CHECK(child>=0);
    if (!child) {
        close(ends[0]);
        pthread_t thread;
        CHECK(pthread_create(&thread,NULL,last_worker,(void *)(intptr_t)ends[1])==0);
        pthread_exit(NULL);
    }

    close(ends[1]);
    char value;
    CHECK(read(ends[0],&value,1)==1 && value=='L');
    CHECK(read(ends[0],&value,1)==0);
    close(ends[0]);
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    puts("PASS exit_group, fatal thread faults and leader exit before final worker");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(40);
    futex_errors();
    mapping_resize();
    pthreads();
    descriptor_lifetime();
    directed_signal();
    recycling();
    private_futex_domains();
    group_lifetime();
    puts("THREAD CHECK PASS");
    return 0;
}
