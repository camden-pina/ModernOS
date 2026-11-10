#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "DYNAMIC CHECK FAIL line %d: %s (errno=%d)\n", __LINE__, #test, errno); \
    exit(1); } } while (0)

extern int dynamic_middle_value(void), dynamic_leaf_tls(int);
extern int dynamic_leaf_data, dynamic_middle_constructor;
extern unsigned char dynamic_leaf_bss[];
extern int dynamic_missing_weak(void) __attribute__((weak));
int dynamic_exported_value = 14;
static int (*const protected_call)(void) = dynamic_middle_value;
static int (*plugin_value)(void), (*plugin_tls)(int);
static int ready, worker_started;
static unsigned objects;
static uintptr_t relro_address;

__attribute__((destructor)) static void finish(void) { puts("DYNAMIC APP FINI PASS"); }

static void *worker(void *unused) {
    (void)unused;
    __atomic_store_n(&worker_started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) sched_yield();
    CHECK(dynamic_leaf_tls(0) == 13);
    CHECK(dynamic_leaf_tls(9) == 22);
    CHECK(plugin_value() == 42 && plugin_tls(3) == 24 && plugin_value() == 45);
    return (void *)(uintptr_t)42;
}

static int inspect(struct dl_phdr_info *info, size_t size, void *unused) {
    (void)size; (void)unused;
    objects++;
    for (unsigned i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        CHECK(p->p_type != PT_LOAD || (p->p_flags & (PF_W | PF_X)) != (PF_W | PF_X));
        if ((uintptr_t)info->dlpi_phdr == getauxval(AT_PHDR) && p->p_type == PT_GNU_RELRO) {
            uintptr_t first = (info->dlpi_addr + p->p_vaddr) & ~(uintptr_t)4095;
            uintptr_t last = (info->dlpi_addr + p->p_vaddr + p->p_memsz) & ~(uintptr_t)4095;
            if (last > first) relro_address = first;
        }
    }
    return 0;
}

static void expect_fault(void (*action)(void)) {
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) { action(); _exit(0); }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS));
}

static void write_text(void) {
    *(volatile unsigned char *)(uintptr_t)dynamic_middle_value = 0xc3;
}
static void write_relro(void) {
    *(volatile unsigned char *)relro_address = 0;
}
static void execute_data(void) {
    unsigned char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) _exit(2);
    p[0] = 0xc3;
    ((void (*)(void))p)();
}

int main(int argc, char **argv) {
    CHECK(argc > 0 && argv[0] && getauxval(AT_PAGESZ) == 4096);
    CHECK(getauxval(AT_PHDR) && getauxval(AT_PHENT) == sizeof(ElfW(Phdr)));
    CHECK(getauxval(AT_PHNUM) && getauxval(AT_ENTRY) && getauxval(AT_BASE));
    CHECK(getauxval(AT_EXECFN) && *(char *)getauxval(AT_EXECFN));
    CHECK(!getauxval(AT_SECURE));
    puts("PASS dynamic process stack and auxiliary vector");
    CHECK(dynamic_middle_constructor == 39 && dynamic_middle_value() == 42);
    CHECK(protected_call() == 42 && !dynamic_missing_weak);
    CHECK(dynamic_leaf_data == 35);
    dynamic_leaf_data++;
    CHECK(dynamic_middle_value() == 43);
    dynamic_leaf_data--;
    CHECK(!dynamic_leaf_bss[0] && !dynamic_leaf_bss[2 * 1024 * 1024 - 1]);
    dynamic_leaf_bss[2 * 1024 * 1024 - 1] = 19;
    puts("PASS transitive DT_NEEDED, RUNPATH, constructors, data and BSS relocations");

    CHECK(dynamic_leaf_tls(5) == 18);
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, worker, NULL));
    while (!__atomic_load_n(&worker_started, __ATOMIC_ACQUIRE)) sched_yield();
    void *plugin = dlopen("libdynamic-plugin.so", RTLD_NOW | RTLD_LOCAL);
    CHECK(plugin != NULL);
    plugin_value = (int (*)(void))dlsym(plugin, "dynamic_plugin_value");
    plugin_tls = (int (*)(int))dlsym(plugin, "dynamic_plugin_tls");
    CHECK(plugin_value && plugin_tls && plugin_value() == 42);
    CHECK(!dlsym(plugin, "coast_symbol_that_does_not_exist") && dlerror());
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    void *answer;
    CHECK(!pthread_join(thread, &answer) && (uintptr_t)answer == 42);
    CHECK(dynamic_leaf_tls(0) == 18 && plugin_tls(0) == 21);
    CHECK(!dlclose(plugin));
    puts("PASS dlopen/dlsym, exported symbols and TLS with existing pthreads");

    void *offset_a = dlopen("libdynamic-offset-a.so", RTLD_NOW | RTLD_LOCAL);
    void *offset_b = dlopen("libdynamic-offset-b.so", RTLD_NOW | RTLD_LOCAL);
    CHECK(offset_a && offset_b && offset_a != offset_b);
    int (*a_value)(void) = (int (*)(void))dlsym(offset_a, "dynamic_plugin_value");
    int (*b_value)(void) = (int (*)(void))dlsym(offset_b, "dynamic_plugin_value");
    CHECK(a_value && b_value && a_value != b_value && a_value() == 42 && b_value() == 42);
    CHECK(!dlclose(offset_a) && !dlclose(offset_b));
    puts("PASS relocatable shared libraries with nonzero virtual-address hints");

    CHECK(!dlopen("libdynamic-unresolved.so", RTLD_NOW) && dlerror());
    CHECK(!dlopen("libdynamic-does-not-exist.so", RTLD_NOW) && dlerror());
    CHECK(!dlopen("libdynamic-bad.so", RTLD_NOW) && dlerror());
    CHECK(!dlopen("libdynamic-wx.so", RTLD_NOW) && dlerror());
    CHECK(!dlopen("libdynamic-bad-metadata.so", RTLD_NOW) && dlerror());
    CHECK(!dlopen("libdynamic-no-null.so", RTLD_NOW) && dlerror());
    CHECK(!dlopen("libdynamic-bad-span.so", RTLD_NOW) && dlerror());
    CHECK(!dlopen("libdynamic-overlap.so", RTLD_NOW) && dlerror());
    puts("PASS dependency, missing-symbol, malformed-library and W+X errors");

    CHECK(!dl_iterate_phdr(inspect, NULL) && objects >= 4 && relro_address);
    expect_fault(write_text);
    expect_fault(write_relro);
    expect_fault(execute_data);
    errno = 0;
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL);
    unsigned char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    errno = 0;
    CHECK(mprotect(page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == -1 && errno == EINVAL);
    CHECK(!munmap(page, 4096));
    puts("PASS executable text, RELRO, NX data and rejected W+X mappings");
    puts("DYNAMIC CHECK PASS");
    return 0;
}
