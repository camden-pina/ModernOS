#!/usr/bin/env python3
"""Test real musl pthreads/futexes and group teardown in the custom guest."""
import argparse
from test_runtime import VM, WORK, assert_success


def exercise(cpus, iso=None):
    WORK.mkdir(parents=True, exist_ok=True)
    vm = VM(cpus, storage=False, iso=iso, label='threads')
    try:
        output = vm.command('thread-check', 'THREAD CHECK PASS', timeout=100)
        assert_success(output)
        assert output.count('PASS ') >= 6, output
        print(f'PASS real guest threads/futexes ({cpus} CPUs)', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso')
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso)
