#include <libc.h>

static void help(void) {
    puts("Usage: env [-i] [NAME=VALUE ...] [PROGRAM [ARG ...]]\nPrint the process environment, or "
         "run a program with changed variables.\n-i starts with an empty environment. Bare "
         "programs search PATH (default /bin).\nExample: env TERM=vt100 nano /tmp/notes.txt");
}

static int assignment(const char *s) {
    if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_')) {
        return 0;
    }
    int i = 1;
    while (s[i] && s[i] != '=') {
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
            return 0;
        }
        i++;
    }

    return s[i] == '=' ? i : 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        help();
        return 0;
    }
    char *values[OS_ENV_MAX + 1];
    int used = 0, i = 1;
    if (i < argc && !strcmp(argv[i], "-i")) {
        i++;
    } else if (environ) {
        for (int j = 0; environ[j]; j++) {
            if (used == OS_ENV_MAX) {
                printf("env: too many inherited variables (limit %u)\n", OS_ENV_MAX);
                return 2;
            }
            values[used++] = environ[j];
        }
    }

    for (; i < argc; i++) {
        int n = assignment(argv[i]);
        if (!n) {
            break;
        }
        int pos = 0;
        while (pos < used && !(strncmp(values[pos], argv[i], n) == 0 && values[pos][n] == '=')) {
            pos++;
        }
        if (pos == OS_ENV_MAX) {
            printf("env: environment limit is %u variables\n", OS_ENV_MAX);
            return 2;
        }
        values[pos] = argv[i];
        if (pos == used) {
            used++;
        }
    }

    values[used] = NULL;
    if (i == argc) {
        for (int j = 0; j < used; j++) {
            puts(values[j]);
        }
        return 0;
    }

    if (argv[i][0] == '-') {
        puts("env: unknown option");
        help();
        return 2;
    }
    char path[OS_PATH_MAX];
    int slash = 0;
    for (const char *p = argv[i]; *p; p++) {
        if (*p == '/') {
            slash = 1;
        }
    }
    const char *search = "/bin";
    for (int j = 0; j < used; j++) {
        if (!strncmp(values[j], "PATH=", 5)) {
            search = values[j] + 5;
        }
    }
    struct os_stat st;
    int found = 0;
    if (slash) {
        if (strlen(argv[i]) >= sizeof(path)) {
            puts("env: program path too long");
            return 2;
        }
        strcpy(path, argv[i]);
        found = 1;
    } else {
        for (;;) {
            size_t n = 0;
            while (search[n] && search[n] != ':') {
                n++;
            }
            if (n + strlen(argv[i]) + 2 < sizeof(path)) {
                memcpy(path, search, n);
                if (n) {
                    path[n++] = '/';
                }
                strcpy(path + n, argv[i]);
                if (!stat(path, &st) && (st.mode & 0170000) == S_IFREG && (st.mode & 0111)) {
                    found = 1;
                    break;
                }
            }

            while (*search && *search != ':') {
                search++;
            }
            if (!*search) {
                break;
            }
            search++;
        }
    }

    if (!found) {
        printf("env: %s: executable not found in PATH\n", argv[i]);
        return 127;
    }
    int pid = syscall6(SYS_spawnve, (long)path, (long)(argv + i), (long)values, 0, 0, 0);
    if (pid < 0) {
        printf("env: %s\n", error_text(pid));
        return 126;
    }
    int status;
    int result = waitpid(pid, &status, 0);
    if (result < 0) {
        printf("env: wait: %s\n", error_text(result));
        return 1;
    }
    return status & 127 ? 128 + (status & 127) : status >> 8;
}
