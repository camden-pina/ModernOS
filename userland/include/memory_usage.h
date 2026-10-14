#ifndef MODERNOS_MEMORY_USAGE_H
#define MODERNOS_MEMORY_USAGE_H
#include <stdint.h>

#define MODERNOS_MEMORY_USAGE 411

/* All user mappings have physical backing in the current eager VM. The
 * limit includes the executable, stacks and anonymous/shared mappings. */
struct os_memory_usage {
    uint64_t mapped_bytes, peak_mapped_bytes, limit_bytes;
};

#endif
