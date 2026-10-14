#!/usr/bin/env python3
"""Exercise real kernel IPC/shared objects and explicit browser capabilities."""
import argparse
from test_runtime import VM, WORK, assert_success


def exercise(cpus, iso=None):
    WORK.mkdir(parents=True, exist_ok=True)
    vm = VM(cpus, storage=False, iso=iso, label='browser-ipc')
    try:
        assert_success(vm.command('ipc-check', 'IPC CHECK PASS', timeout=110))
        output = vm.command('web-capability-check', 'WEB CAPABILITY CHECK PASS', timeout=180)
        assert_success(output)

        for expected in ('PASS immutable origins', 'PASS threaded broker',
                         'PASS bounded pipe capacity',
                         'PASS scoped broker renderer cleanup',
                         'JAVASCRIPTCORE CHECK PASS', 'JAVASCRIPTCORE AGENTS PASS',
                         'GLIB CHECK PASS'):
            assert expected in output, (expected, output)

        print(
            f'PASS guest IPC/shared memory and browser launch capabilities ({cpus} CPUs)', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso')
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso)
