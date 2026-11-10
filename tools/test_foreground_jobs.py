#!/usr/bin/env python3
"""Verify bounded foreground console groups against a real disposable guest."""
import argparse
import re
import time
from test_runtime import VM, assert_success


def exercise(cpus,iso,accel,label):
    vm=VM(cpus,storage=False,network=False,iso=iso,accel=accel,label=label)
    try:
        result=vm.command('foreground-check','FOREGROUND CHECK PASS',60)
        assert_success(result)
        for marker in ('FOREGROUND BACKGROUND REFUSAL PASS','FOREGROUND GROUP ABI PASS','FOREGROUND CONTROLLER EXIT PASS'):
            assert marker in result,result
        original=vm.shell_command('stty -a')
        for _ in range(4):
            output=vm.shell_command('set -e; check --tty-denied < /dev/tty & p=$!; wait "$p"; echo FG_BACKGROUND_PROTECTION_PASS','FG_BACKGROUND_PROTECTION_PASS')
            assert_success(output)
        # A producer far larger than pipe capacity must still exit promptly
        # when its slow pager closes early. The shell must reclaim the terminal.
        vm.send("build-sh -c 'seq 1 1000000 | less; echo FG_SLOW_READER_RETURNED'\n")
        vm.until(rb'\n1\n[\s\S]*standard input',30)
        time.sleep(.2)
        vm.send('q')
        result=vm.until_prompt(30)
        assert 'FG_SLOW_READER_RETURNED\n' in result,result
        assert_success(result)
        assert vm.shell_command('stty -a')==original
        # SIGINT goes to every foreground pipeline member, while the waiting
        # shell restores the terminal before applying the interrupt status.
        vm.send("build-sh -c 'seq 1 1000000 | cat | less; echo FG_INTERRUPT_MUST_ABORT'\n")
        vm.until(rb'\n1\n[\s\S]*standard input',30)
        vm.send('\x03')
        result=vm.until_prompt(30)
        assert '\nFG_INTERRUPT_MUST_ABORT\n' not in result,result
        assert re.search(r'terminated by signal 2|exited 130',result),result
        assert vm.shell_command('stty -a')==original
        result=vm.command("build-sh -c 'seq 1 1000000 | less /tmp/foreground-missing'",'No such file',30)
        assert re.search(r'exited 1',result),result
        assert vm.shell_command('stty -a')==original
        assert_success(vm.command('echo FOREGROUND_SHELL_RESTORED','\nFOREGROUND_SHELL_RESTORED\n'))
        print(f'PASS {cpus}CPU {accel}: foreground groups, background refusal, controller death, slow reader, SIGINT, errors and shell mode',flush=True)
    finally:
        vm.close()

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iso',required=True);p.add_argument('--cpus',type=int,nargs='+',default=[1,4])
    p.add_argument('--accel',choices=('tcg','kvm'),default='tcg');p.add_argument('--label',required=True)
    a=p.parse_args()
    for count in a.cpus:exercise(count,a.iso,a.accel,a.label)
