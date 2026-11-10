#include "desktop.h"
#include <uapi/clipboard.h>

int clipboard_write(const char *text, size_t length) {
    if (length > OS_CLIPBOARD_LIMIT)
        return -E_2BIG;
    long result = call3(SYS_clipboard, OS_CLIPBOARD_WRITE, text, length);
    return result < 0 ? result : result == (long)length ? 0 : -E_IO;
}

int clipboard_read(char **text, size_t *length) {
    *text = NULL;
    *length = 0;
    char *copy = allocate(OS_CLIPBOARD_LIMIT + 1);
    if (!copy)
        return -E_NOMEM;
    int error = -E_AGAIN;
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        struct os_clipboard_info info;
        error = call3(SYS_clipboard, OS_CLIPBOARD_INFO, &info, sizeof(info));
        if (error)
            break;
        if (info.bytes > OS_CLIPBOARD_LIMIT || info.capacity != OS_CLIPBOARD_LIMIT) {
            error = -E_INVAL;
            break;
        }
        long count = syscall6(SYS_clipboard, OS_CLIPBOARD_READ, (long)copy, OS_CLIPBOARD_LIMIT,
                              info.generation, 0, 0);
        if (count == -E_AGAIN) {
            error = -E_AGAIN;
            continue;
        }
        if (count < 0) {
            error = count;
            break;
        }
        copy[count] = 0;
        *text = copy;
        *length = count;
        return 0;
    }
    deallocate(copy, OS_CLIPBOARD_LIMIT + 1);
    return error;
}

void clipboard_release(char *text) {
    if (text)
        deallocate(text, OS_CLIPBOARD_LIMIT + 1);
}

void clipboard_error(Window *w, const char *action, int error) {
    if (error == -E_2BIG)
        dialog_notice(w, action,
                      "The selection exceeds the 64 KiB text clipboard. Select a smaller range.");
    else if (error == -E_INVAL)
        dialog_notice(w, action,
                      "This selection contains invalid text or unsupported control characters. "
                      "Choose plain UTF-8 text.");
    else if (error == -E_AGAIN)
        dialog_notice(w, action, "The clipboard changed during this paste. Try pasting again.");
    else
        dialog_error(w, action, error);
}
