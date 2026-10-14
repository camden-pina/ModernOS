#include <libc.h>

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: tty\nIdentify whether standard input is the console or an inherited PTY "
             "session.");
        return 0;
    }
    if (argc != 1) {
        puts("Usage: tty");
        return 2;
    }
    struct os_termios mode;
    int e = ioctl(0, TCGETS, &mode);
    if (e) {
        puts("not a tty");
        return 1;
    }
    unsigned pid;
    if (!ioctl(0, OS_PTY_GET_FOREGROUND, &pid)) {
        puts("pseudoterminal session (descriptor access)");
    } else {
        puts("/dev/tty");
    }
    return 0;
}
