/* Unprivileged package helper. Installation uses authenticated sudo/pacman. */
#include <unistd.h>
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc > 128) return 2;
    char *args[136];
    args[0] = "/usr/bin/python3.14";
    args[1] = "-I"; args[2] = "-S"; args[3] = "-B";
    args[4] = "/usr/lib/coast/packages/client.py";
    for (int i = 1; i < argc; i++) args[i + 4] = argv[i];
    args[argc + 4] = NULL;
    /* Keep the real profile's HOME for makepkg's user configuration. The
       isolated interpreter ignores inherited Python paths/startup settings. */
    extern char **environ;
    execve(args[0], args, environ);
    perror("coast: Python runtime unavailable");
    return 127;
}
