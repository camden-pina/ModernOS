#ifndef COAST_POWER_H
#define COAST_POWER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct task;
/* Roles are boot-assigned, never user-controlled. AML clients stay on BSP. */
enum { POWER_ROLE_ORDINARY, POWER_ROLE_COORDINATOR, POWER_ROLE_FIRMWARE,
       POWER_ROLE_TELEMETRY };
bool power_start(bool firmware);
long power_request(struct task *, uint64_t action, uint64_t flags);
void power_request_ready(unsigned pid);
size_t power_info(char *, size_t);
size_t power_acpi_info(char *, size_t);
void power_button_event(void);
bool power_frozen(void);
bool power_task_may_run(const struct task *);
void power_checkpoint(void);
void power_ap_checkpoint(void); /* IF=0, outside scheduler handoff/device IRQ. */
void power_transaction_begin(void);
void power_transaction_end(void);
void power_task_exit(void); /* final lifecycle accounting, IF=0 */
void power_mutex_acquired(void); /* caller holds irq_save guard */
void power_mutex_released(void); /* release precedes accounting, IF=0 */
#endif
