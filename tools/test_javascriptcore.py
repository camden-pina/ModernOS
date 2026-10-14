#!/usr/bin/env python3
"""Real modern JS component execution in the guest; this is not a website test."""
import argparse
from test_runtime import VM, WORK, assert_success


def exercise(cpus, iso=None):
    WORK.mkdir(parents=True, exist_ok=True)
    vm = VM(cpus, storage=False, iso=iso, label='javascriptcore')
    try:
        assert_success(vm.command('thread-cxx-check', 'CXX THREAD CHECK PASS', timeout=90))
        output = vm.command('jsc /usr/share/modern-browser/engine-check.js',
                            'JAVASCRIPTCORE CHECK PASS', timeout=120)
        assert_success(output)
        assert_success(vm.command('jsc -m /usr/share/modern-browser/module-check.mjs',
                                  'JAVASCRIPTCORE MODULE PASS', timeout=90))
        assert_success(vm.command('jsc /usr/share/modern-browser/agent-check.js',
                                  'JAVASCRIPTCORE AGENTS PASS', timeout=90))

        print(
            f'PASS guest JavaScriptCore component ({cpus} CPUs): modern JS, ICU, jobs, GC, modules and threaded agents', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso')
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso)
