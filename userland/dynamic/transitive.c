#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
extern int dynamic_middle_value(void);
int main(int argc, char **argv) {
    const char *execfn = (const char *)getauxval(AT_EXECFN);
    if (argc < 1 || !execfn || *execfn != '/') return 2;
    if (!strcmp(argv[0], "unrelated-argv0")) {
        if (strcmp(execfn, "/usr/lib/coast/dynamic/dynamic-transitive")) return 3;
        puts("DYNAMIC ARGV0 PASS");
    }
    if (!strcmp(argv[0], "/usr/bin/dynamic-transitive-link")) {
        if (strcmp(execfn, argv[0])) return 4;
        puts("DYNAMIC SYMLINK ORIGIN PASS");
    }
    if (dynamic_middle_value() != 42) return 1;
    puts("DYNAMIC TRANSITIVE PASS");
    return 0;
}
