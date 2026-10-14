#ifndef _LINUX_PRINTK_H
#define _LINUX_PRINTK_H

#include <console.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Linux Kernel Log Levels (ASCII SOH prefixes)
 */
#define KERN_SOH "\001"
#define KERN_SOH_ASCII '\001'

#define KERN_EMERG KERN_SOH "0" /* System is unusable */
#define KERN_ALERT KERN_SOH "1" /* Action must be taken immediately */
#define KERN_CRIT KERN_SOH "2" /* Critical conditions */
#define KERN_ERR KERN_SOH "3" /* Error conditions */
#define KERN_WARNING KERN_SOH "4" /* Warning conditions */
#define KERN_NOTICE KERN_SOH "5" /* Normal but significant condition */
#define KERN_INFO KERN_SOH "6" /* Informational */
#define KERN_DEBUG KERN_SOH "7" /* Debug-level messages */
#define KERN_CONT KERN_SOH "c" /* Continuation line (no timestamp) */

#define KERN_DEFAULT KERN_INFO

/*
 * ANSI Color Palette for Serial / Terminal Consoles
 */
#define KLOG_ANSI_RESET "\033[0m"
#define KLOG_ANSI_BOLD "\033[1m"
#define KLOG_ANSI_DIM "\033[2m"

#define KLOG_ANSI_TIME "\033[0;32m" /* Green timestamp [    0.000000] */
#define KLOG_ANSI_EMERG "\033[1;37;41m" /* Bold White on Red */
#define KLOG_ANSI_ALERT "\033[1;31m" /* Bold Red */
#define KLOG_ANSI_CRIT "\033[1;31m" /* Bold Red */
#define KLOG_ANSI_ERR "\033[0;31m" /* Red */
#define KLOG_ANSI_WARNING "\033[0;33m" /* Yellow */
#define KLOG_ANSI_NOTICE "\033[1;37m" /* Bold White */
#define KLOG_ANSI_INFO "\033[0m" /* Default terminal text */
#define KLOG_ANSI_DEBUG "\033[0;36m" /* Cyan */

/*
 * Linux pr_* Convenience Macros
 */
#define pr_emerg(fmt, ...) printk(KERN_EMERG fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...) printk(KERN_ALERT fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...) printk(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...) printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_warning pr_warn
#define pr_notice(fmt, ...) printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) printk(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) printk(KERN_DEBUG fmt, ##__VA_ARGS__)
#define pr_cont(fmt, ...) printk(KERN_CONT fmt, ##__VA_ARGS__)

/* Primary printk API */
int printk(const char *fmt, ...);
int vprintk(const char *fmt, va_list args);

/* Subsystem Configuration & Hooks */
void printk_early_init(void);
void printk_set_uptime_fn(uint64_t (*fn)(void));
uint64_t printk_uptime_us(void);
void printk_set_color_enabled(bool enabled);

/* Legacy & Compatibility Aliases */
#define kprintf(fmt, ...) printk(fmt, ##__VA_ARGS__)
#define kprintf_early_init() printk_early_init()

void kprintf_kputs(const char *str);
void kvfprintf(const char *format, va_list args);

/* Formatter Prototypes */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int snprintf(char *buf, size_t size, const char *fmt, ...);
void lai_vsnprintf(char *buf, size_t len, const char *fmt, va_list arg);

#endif /* _LINUX_PRINTK_H */
