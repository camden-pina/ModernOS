#include <libc.h>

/* The PID-1 and desktop command shell retain their OS commands. Build scripts
   use the actual upstream ash and utilities with explicit environment values. */
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: build-sh [SCRIPT [ARGS...] | -c COMMAND]\n"
             "Start the native BusyBox ash build environment. With no arguments,\n"
             "open an interactive shell. Scripts, pipes, redirects, variables,\n"
             "wildcards and upstream /usr/bin text utilities are supported.\n"
             "PATH=/usr/bin:/bin; SHELL and CONFIG_SHELL=/bin/ash.\n"
             "Native GCC/G++, binutils and static development libraries are installed.\n"
             "Example: build-sh -c 'printf \"b\\na\\n\" | sort'");
        return 0;
    }

    char *values[OS_ENV_MAX + 1];
    unsigned count = 0;
    values[count++] = "PATH=/usr/bin:/bin";
    values[count++] = "SHELL=/bin/ash";
    values[count++] = "CONFIG_SHELL=/bin/ash";
    for (unsigned i = 0; environ && environ[i]; i++) {
        const char *v = environ[i];
        if (!strncmp(v, "PATH=", 5) || !strncmp(v, "SHELL=", 6) ||
            !strncmp(v, "CONFIG_SHELL=", 13)) {
            continue;
        }
        if (count == OS_ENV_MAX) {
            printf("build-sh: environment exceeds %u variables\n", OS_ENV_MAX);
            return 2;
        }
        values[count++] = environ[i];
    }

    values[count] = NULL;
    argv[0] = "ash";
    long e = call3(SYS_execve, "/bin/ash", argv, values);
    printf("build-sh: %s (%ld)\n", error_text(e), e);
    return 126;
}
