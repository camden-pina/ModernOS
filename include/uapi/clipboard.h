#ifndef COAST_CLIPBOARD_ABI_H
#define COAST_CLIPBOARD_ABI_H
#include <stdint.h>
#define OS_CLIPBOARD_LIMIT 65536u
#define COAST_SYSCALL_CLIPBOARD 414
enum { OS_CLIPBOARD_INFO, OS_CLIPBOARD_READ, OS_CLIPBOARD_WRITE, OS_CLIPBOARD_CLEAR,
       OS_CLIPBOARD_WRITE_IF_CURRENT };
struct os_clipboard_info {
    uint64_t generation;
    uint32_t bytes, capacity;
};
#endif
