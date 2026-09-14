#include <printf.h>
#include <panic.h>
#include <8250.h>
#include <stdint.h>
#include <fmt.h>

#define BUFFER_SIZE 512

static void *impl_arg;
static int (*kprintf_puts_impl)(void *, const char *);

static struct early_kprintf {
  // mtx_t lock;
  uint16_t port;
} early_kprintf = {
  .port = COM1_PORT,
};

static int early_kprintf_puts(void *arg, const char *s) {
  struct early_kprintf *p = arg;
  // mtx_spin_lock(&p->lock);
  while (*s) {
    serial_port_write_char(p->port, *s);
    s++;
  }
  // mtx_spin_unlock(&p->lock);
  return 0;
}

//

void kprintf_early_init() {
  serial_port_init(early_kprintf.port);
  impl_arg = &early_kprintf;
  // mtx_init(&early_kprintf.lock, MTX_SPIN, "early_kprintf_lock");
  kprintf_puts_impl = early_kprintf_puts;
}

void kprintf_kputs(const char *str) {
  kprintf_puts_impl(impl_arg, str);
}

// MARK: Public API

void kprintf(const char *format, ...) {
  char str[BUFFER_SIZE];
  va_list valist;
  va_start(valist, format);
  fmt_format(format, str, BUFFER_SIZE, FMT_MAX_ARGS, valist);
  va_end(valist);
  kprintf_puts_impl(impl_arg, str);
}

void kvfprintf(const char *format, va_list valist) {
  char str[BUFFER_SIZE];
  fmt_format(format, str, BUFFER_SIZE, FMT_MAX_ARGS, valist);
  kprintf_puts_impl(impl_arg, str);
}
