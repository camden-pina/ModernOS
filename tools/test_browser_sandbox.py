#!/usr/bin/env python3
"""Exercise kernel browser restrictions with an intentionally hostile child."""
import argparse
from test_runtime import VM, WORK, assert_success


def exercise(cpus, iso=None):
    WORK.mkdir(parents=True, exist_ok=True)
    vm = VM(cpus, storage=False, iso=iso, label='browser-sandbox')
    try:
        output = vm.command('browser-sandbox-check', 'BROWSER SANDBOX CHECK PASS', timeout=120)
        assert_success(output)
        assert output.count('SANDBOX CHILD PASS:') == 2, output
        print(
            f'PASS browser kernel sandbox ({cpus} CPUs): denied escapes, retained browser capabilities', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso')
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso)
