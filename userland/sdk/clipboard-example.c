#include <coast/clipboard.h>
#include <unistd.h>
int main(void) {
    static const char text[] = "NATIVE SDK CLIPBOARD\n";
    long result =
        syscall(COAST_SYSCALL_CLIPBOARD, OS_CLIPBOARD_WRITE, text, sizeof(text) - 1, 0);
    return result == sizeof(text) - 1 ? 0 : 1;
}
