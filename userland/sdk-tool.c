#include <libc.h>

/* Keep GCC's actual argv[0] at its configured /usr prefix. Launching a copied
   driver in /bin would relocate its cc1, headers and libraries to /lib. */
int main(int argc, char **argv) {
    (void)argc;
    const char *name = argv[0];
    for (const char *p = name; *p; p++)
        if (*p == '/')
            name = p + 1;
    char path[OS_PATH_MAX] = "/usr/bin/";
    size_t n = strlen(name);
    if (!n || n > sizeof(path) - 10)
        return 126;
    memcpy(path + 9, name, n + 1);
    argv[0] = path;
    long error = call3(SYS_execve, path, argv, environ);
    printf("%s: native tool could not start: %s (%ld)\n", name, error_text(error), error);
    return error == -E_NOENT ? 127 : 126;
}
