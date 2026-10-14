#include <libc.h>

/* Real exec aliases keep a single BusyBox binary without needing writable
   symlinks or a special initramfs node type. No fork or command emulation. */
int main(int argc, char **argv) {
    (void)argc;
    const char *name = argv[0];
    for (const char *p = name; *p; p++)
        if (*p == '/')
            name = p + 1;
    /* Upstream BusyBox dispatches by argv[0], so preserve the complete argv
       instead of spending an argument slot on a synthetic applet parameter. */
    long e = call3(SYS_execve, "/bin/busybox", argv, environ);
    printf("%s: %s (%ld)\n", name, error_text(e), e);
    return 126;
}
