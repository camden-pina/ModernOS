#ifndef TTY_H
#define TTY_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <uapi/abi.h>

void tty_input(char c);
long tty_read(void *data, size_t size, bool nonblock);
bool tty_is_foreground(unsigned pid);
int tty_get_config(struct os_termios *config);
int tty_set_config(const struct os_termios *config, bool flush);
unsigned tty_available(void);
void tty_transfer_foreground(unsigned from, unsigned to);
void tty_reset_config(unsigned pid);
unsigned tty_job_foreground(void);
void tty_job_restore(unsigned from, unsigned to, const struct os_termios *);
void input_task(void *arg);
#endif
