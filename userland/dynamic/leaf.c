int dynamic_leaf_data = 35;
unsigned char dynamic_leaf_bss[2 * 1024 * 1024];
static int initialized;
static __thread int thread_value = 13;

__attribute__((constructor)) static void initialize(void) { initialized = 4; }
int dynamic_leaf_value(void) { return dynamic_leaf_data + initialized; }
int dynamic_leaf_tls(int increment) { return thread_value += increment; }
