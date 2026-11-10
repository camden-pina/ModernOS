#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/utsname.h>

__attribute__((constructor)) static void verify_gnu_runtime() {
    struct utsname system;
    Dl_info library = {};
    const char *prefix = std::getenv("COAST_CXX_LIBRARY_PREFIX");
    void *symbol = dlsym(RTLD_DEFAULT, "__cxa_throw");
    if (uname(&system) || std::strcmp(system.sysname, "Coast") || !prefix ||
        !symbol || !dladdr(symbol, &library) || !library.dli_fname ||
        std::strncmp(library.dli_fname, prefix, std::strlen(prefix)) ||
        !std::strstr(library.dli_fname, "libstdc++.so.6")) {
        std::fputs("GNU_CXX_FAIL source-built library or Coast execution identity\n", stderr);
        std::_Exit(1);
    }
    std::printf("GNU_CXX_LOADED %s\n", library.dli_fname);
    const char *unwinder_prefix = std::getenv("COAST_UNWINDER_LIBRARY_PREFIX");
    if (unwinder_prefix) {
        symbol = dlsym(RTLD_DEFAULT, "_Unwind_RaiseException");
        if (!symbol || !dladdr(symbol, &library) || !library.dli_fname ||
            std::strncmp(library.dli_fname, unwinder_prefix, std::strlen(unwinder_prefix)) ||
            !std::strstr(library.dli_fname, "libgcc_s.so.1")) {
            std::fputs("GNU_UNWINDER_FAIL source-built library identity\n", stderr);
            std::_Exit(1);
        }
        std::printf("GNU_UNWINDER_LOADED %s\n", library.dli_fname);
    }
}

// Exercise the same full thread/exception consumer with GNU dynamic linking.
#include "../build-tools/thread_check.cc"
