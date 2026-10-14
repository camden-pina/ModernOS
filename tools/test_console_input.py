#!/usr/bin/env python3
"""Keep partially typed input through a native fork/stdin-redirection handoff."""
import argparse
import shlex

from test_runtime import VM, WORK, assert_success

SOURCE = r'''
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void) {
    int channel[2];
    if (pipe(channel)) return 1;
    puts("TTY_BEFORE_FORK"); fflush(stdout);
    usleep(150000);
    int child=fork();
    if (child<0) return 2;
    if (!child) {
        close(0); close(channel[0]);
        if (write(channel[1],"Y",1)!=1) _exit(3);
        _exit(0);
    }
    close(channel[1]);
    char ready, line[128]; int status;
    if (read(channel[0],&ready,1)!=1 || ready!='Y') return 4;
    if (waitpid(child,&status,0)!=child || status) return 5;
    puts("TTY_AFTER_FORK"); fflush(stdout);
    ssize_t n=read(0,line,sizeof(line));
    if (n!=21 || memcmp(line,"TTY_PENDING_INPUT_OK\n",21)) {
        fprintf(stderr,"TTY_INPUT_LOST: received %ld bytes\n",(long)n); return 6;
    }
    puts("TTY_HANDOFF_INPUT_OK"); return 0;
}
'''


def exercise(cpus, iso=None):
    WORK.mkdir(parents=True, exist_ok=True)
    vm = VM(cpus, storage=False, network=False, iso=iso, label='console-input')
    try:
        for index, line in enumerate(SOURCE.splitlines()):
            if not line and not index:
                continue
            operation = 'write' if line.startswith('#include <stdio.h>') else 'append'
            assert_success(vm.command(operation + ' /tmp/console-input.c ' + shlex.quote(line)))

        assert_success(vm.command('gcc -O2 /tmp/console-input.c -o /tmp/console-input'))

        for _ in range(8):
            vm.send('/tmp/console-input\n')
            vm.until(rb'TTY_BEFORE_FORK\r?\n', 30)
            vm.send('TTY_PENDING_INPUT_OK')
            vm.until(rb'TTY_AFTER_FORK\r?\n', 30)
            vm.send('\n')
            assert_success(vm.until_prompt(30))

        print(
            f'PASS {cpus} CPU console: partial canonical input survives fork, stdin close and child reaping', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--iso')
    args = parser.parse_args()
    exercise(args.cpus, args.iso)
