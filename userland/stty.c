#include <libc.h>

static void help(void) {
    puts("Usage: stty [-a|size|raw|cooked|echo|-echo]\nInspect or configure this terminal's "
         "supported line discipline.\nraw: immediate bytes without echo/signals; cooked: canonical "
         "input with echo.\nsize prints actual rows and columns. Unsupported options are errors.");
}

int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        help();
        return 0;
    }
    struct os_termios t;
    int e = ioctl(0, TCGETS, &t);
    if (e) {
        printf("stty: %s\n", error_text(e));
        return 1;
    }
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-a"))) {
        struct os_winsize size;
        e = ioctl(0, TIOCGWINSZ, &size);
        if (!e) {
            printf("rows %u; columns %u; ", size.rows, size.cols);
        }
        printf("%sicanon %secho %sisig %sicrnl; min %u; time %u\n", t.lflag & ICANON ? "" : "-",
               t.lflag & ECHO ? "" : "-", t.lflag & ISIG ? "" : "-", t.iflag & ICRNL ? "" : "-",
               t.cc[VMIN], t.cc[VTIME]);
        return 0;
    }

    if (argc != 2) {
        help();
        return 2;
    }
    if (!strcmp(argv[1], "size")) {
        struct os_winsize size;
        e = ioctl(0, TIOCGWINSZ, &size);
        if (!e) {
            printf("%u %u\n", size.rows, size.cols);
            return 0;
        }
    } else {
        if (!strcmp(argv[1], "raw")) {
            t.iflag = t.oflag = t.lflag = 0;
            t.cc[VMIN] = 1;
            t.cc[VTIME] = 0;
        } else if (!strcmp(argv[1], "cooked")) {
            t.iflag = ICRNL;
            t.oflag = 0;
            t.lflag = ISIG | ICANON | ECHO;
            t.cc[VMIN] = 1;
            t.cc[VTIME] = 0;
        } else if (!strcmp(argv[1], "echo")) {
            t.lflag |= ECHO;
        } else if (!strcmp(argv[1], "-echo")) {
            t.lflag &= ~ECHO;
        } else {
            puts("stty: unsupported option");
            help();
            return 2;
        }
        e = ioctl(0, TCSETS, &t);
        if (!e) {
            return 0;
        }
    }

    printf("stty: %s\n", error_text(e));
    return 1;
}
