#!/usr/bin/env python3
"""Real guest GLib/GObject/Gio execution; this is not a webpage renderer test."""
import argparse
from test_runtime import VM, WORK, assert_success


def exercise(cpus, iso=None):
    WORK.mkdir(parents=True, exist_ok=True)
    vm = VM(cpus, storage=False, iso=iso, label='glib')
    try:
        output = vm.command('glib-check', 'GLIB CHECK PASS', timeout=60)
        assert_success(output)
        assert output.count('PASS ') >= 4, output
        print(f'PASS genuine guest GLib/GObject/Gio ({cpus} CPUs)', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso')
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso)
