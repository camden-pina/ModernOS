#include <panic.h>
#include <printk.h>
#include <8250.h>
#include <stdarg.h>

void panic_early_init(void) {
}

noreturn void panic(const char *fmt, ...) {
    __asm__ volatile("cli" ::: "memory");
    char message[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    /* Panic must work even if the fault interrupted a held console lock. */
    serial_port_write("\nPANIC: ");
    serial_port_write(message);
    serial_port_write("\n");
    for (;;)
        __asm__ volatile("hlt");
}
