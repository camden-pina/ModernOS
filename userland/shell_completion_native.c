#include "include/shell_completion.h"
#include <libc.h>
static int scan(void *unused, const char *path, sc_visit visit, void *data) {
    (void)unused;
    int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return fd;
    char bytes[4096];
    unsigned count = 0;
    int result = 0;
    for (;;) {
        long n = call3(SYS_getdents64, fd, bytes, sizeof(bytes));
        if (n < 0) {
            result = n;
            break;
        }
        if (!n)
            break;
        for (long off = 0; off < n;) {
            struct os_dirent *d = (void *)(bytes + off);
            if (n - off < 20 || d->reclen < 20 || d->reclen > n - off) {
                result = -E_IO;
                goto done;
            }
            unsigned len = 0, cap = d->reclen - 19;
            while (len < cap && d->name[len])
                len++;
            if (len == cap) {
                result = -E_IO;
                goto done;
            }
            if (++count > 4096) {
                result = -E_2BIG;
                goto done;
            }
            if (!visit(data, d->name, d->type == 4 ? 2 : 3))
                goto done;
            off += d->reclen;
        }
    }
done:
    close(fd);
    return result;
}
static int kind(void *unused, const char *path, bool executable) {
    (void)unused;
    struct os_stat st;
    if (stat(path, &st))
        return 0;
    if ((st.mode & 0170000) == S_IFDIR)
        return 2;
    return (st.mode & 0170000) == S_IFREG && (!executable || (st.mode & 0111)) ? 1 : 0;
}
int shell_complete_native(struct sc_result *r, char *line, size_t cap, int *cursor, const char *cwd,
                          const char *path) {
    static const struct sc_fs fs = {scan, kind};
    return shell_complete(r, line, cap, cursor, cwd, path, &fs, NULL);
}
const char *shell_search_path(void) {
    if (environ)
        for (char **p = environ; *p; p++)
            if (!strncmp(*p, "PATH=", 5))
                return *p + 5;
    return "/bin:/usr/bin";
}
