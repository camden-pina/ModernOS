#ifndef MODERNOS_WEB_PROCESS_H
#define MODERNOS_WEB_PROCESS_H
#include <stdint.h>

#define MODERNOS_SPAWN_WEB 409
#define MODERNOS_WEB_IDENTITY 410

enum { OS_WEB_NONE, OS_WEB_BROKER, OS_WEB_RENDERER };
struct os_spawn_web {
    uint32_t role, reserved;
    int32_t ipc_fd, stdout_fd, stderr_fd, reserved2;
    uint64_t origin, home;
};

struct os_web_identity {
    uint32_t role, process, domain, reserved;
    char origin[256];
};

#ifdef __cplusplus
static_assert(sizeof(struct os_spawn_web) == 40, "web launch ABI");
static_assert(sizeof(struct os_web_identity) == 272, "web identity ABI");
#else
_Static_assert(sizeof(struct os_spawn_web) == 40, "web launch ABI");
_Static_assert(sizeof(struct os_web_identity) == 272, "web identity ABI");
#endif
#endif
