#ifndef KERNEL_FMT_H
#define KERNEL_FMT_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

// determines the maximum number of arguments that can be passed to fmt_format.
// extra arguments are ignored. the `max_args` parameter to fmt_format is clamped
// to this value.
#define FMT_MAX_ARGS 10

int fmt_format(const char *format, char *buf, size_t buf_size, size_t max_args, va_list args);

#endif
