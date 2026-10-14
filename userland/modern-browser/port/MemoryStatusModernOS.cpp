// Read actual, private process accounting without a Linux /proc dependency.
#include "config.h"
#include <wtf/linux/CurrentProcessMemoryStatus.h>
#include "../../include/memory_usage.h"
#include <unistd.h>

namespace WTF {

void currentProcessMemoryStatus(ProcessMemoryStatus &status) {
    os_memory_usage usage = {};
    if (syscall(MODERNOS_MEMORY_USAGE, &usage))
        return;
    status.size = usage.mapped_bytes;
    status.resident = usage.mapped_bytes;
}
}
