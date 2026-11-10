#include <libc.h>
#include <uapi/clipboard.h>
static char text[OS_CLIPBOARD_LIMIT + 1];
static long clipboard_call(unsigned operation, size_t length) {
    /* Bash can exec a small client just before transferring the foreground.
       Wait briefly for that handoff; every retry still checks kernel authority.
       A background job remains denied and never receives clipboard bytes. */
    long result;
    for (unsigned attempt = 0;; attempt++) {
        result = syscall6(SYS_clipboard, operation, (long)text, length, 0, 0, 0);
        if (result != -E_PERM || attempt == 99)
            return result;
        struct os_timespec delay = {0, 2000000};
        if (call2(SYS_nanosleep, &delay, 0) < 0)
            return result;
    }
}
int main(int argc, char **argv) {
    if (argc != 2 || (!strcmp(argv[1], "--help"))) {
        puts("Usage: clip -c | -p | --clear\n"
             "Copy plain UTF-8 from stdin, paste to stdout, or clear the guest text clipboard.\n"
             "Limit: 64 KiB. Clipboard contents last for this boot.");
        return argc == 2 ? 0 : 2;
    }
    long result;
    if (!strcmp(argv[1], "-c")) {
        size_t length = 0;
        while (length < sizeof(text)) {
            long count = read(0, text + length, sizeof(text) - length);
            if (count == -E_INTR)
                continue;
            if (count < 0) {
                result = count;
                goto fail;
            }
            if (!count)
                break;
            length += count;
        }
        result = clipboard_call(OS_CLIPBOARD_WRITE, length);
    } else if (!strcmp(argv[1], "-p")) {
        result =
            clipboard_call(OS_CLIPBOARD_READ, OS_CLIPBOARD_LIMIT);
        if (result >= 0)
            result = write_all(1, text, result);
    } else if (!strcmp(argv[1], "--clear")) {
        result = call3(SYS_clipboard, OS_CLIPBOARD_CLEAR, 0, 0);
    } else {
        puts("clip: expected -c, -p or --clear");
        return 2;
    }
    if (result >= 0)
        return 0;
fail:
    printf("clip: %s (%ld)\n", error_text(result), result);
    return 1;
}
