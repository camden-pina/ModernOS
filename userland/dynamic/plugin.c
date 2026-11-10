#include <stdio.h>
extern int dynamic_exported_value;
static __thread int thread_value = 21;
static int initialized;
__attribute__((constructor)) static void initialize(void) { initialized = 7; }
__attribute__((destructor)) static void finish(void) { puts("DYNAMIC PLUGIN FINI PASS"); }
int dynamic_plugin_value(void) { return thread_value + initialized + dynamic_exported_value; }
int dynamic_plugin_tls(int increment) { return thread_value += increment; }
