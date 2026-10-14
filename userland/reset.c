#include <libc.h>

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: reset\nRestore canonical terminal input, echo, signal keys, and clear the "
             "screen.");
        return 0;
    }
    if (argc != 1) {
        puts("Usage: reset");
        return 2;
    }
    struct os_termios t = {.iflag = ICRNL,
                           .cflag = 0xbf,
                           .lflag = ISIG | ICANON | ECHO,
                           .cc = {[VINTR] = 3, [VERASE] = 127, [VEOF] = 4, [VMIN] = 1}};
    int e = ioctl(0, TCSETSF, &t);
    if (e) {
        printf("reset: %s\n", error_text(e));
        return 1;
    }
    const char *sequence = "\033[0m\033[r\033[2J\033[H";
    return write_all(1, sequence, strlen(sequence)) ? 1 : 0;
}
