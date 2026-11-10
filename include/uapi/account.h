#ifndef COAST_ACCOUNT_ABI_H
#define COAST_ACCOUNT_ABI_H
#include <stdint.h>
/* Local guest accounts. The first home is shared with the separately confined Electron UID 1000. */
#define OS_ACCOUNT_FIRST 1000u
#define OS_ACCOUNT_COUNT 32u
#define OS_ACCOUNT_ADMIN 1u
#define OS_ACCOUNT_PASSWORD 2u
#define OS_ACCOUNT_ROLE 1u
#define OS_ACCOUNT_TIMEOUT 2u
#define OS_ACCOUNT_SET_PASSWORD 4u
#define OS_ACCOUNT_QUERY 0u
#define OS_ACCOUNT_ENSURE 1u
#define OS_ACCOUNT_UPDATE 2u
#define OS_ACCOUNT_AUTH 3u
#define OS_ACCOUNT_ENTER 4u
#define OS_ACCOUNT_SUDO 5u
#define OS_ACCOUNT_FORGET 6u
#define OS_ACCOUNT_VALIDATE 7u
#define OS_ACCOUNT_FILE_SCOPE 8u /* Trusted root desktop broker only. */
#define OS_ACCOUNT_CURRENT 9u
#define OS_ACCOUNT_LOGIN 10u
#define OS_ACCOUNT_CONSOLE_LOGIN 11u
#define OS_ACCOUNT_CONSOLE_SYNC 12u
#define OS_ACCOUNT_CODE_LAUNCH 13u /* Protected static code frontend only. */
#define OS_ACCOUNT_BROWSER_LAUNCH 14u /* Root-owned Chromium display bridge. */
struct os_account_request {
    uint32_t uid, actor, flags, timeout_minutes, changes, reserved;
    char password[128], new_password[128];
};
#endif
