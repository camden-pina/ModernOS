#!/usr/bin/env python3
"""Actual guest WebCore pixels and click in bounded, immutable-origin fixtures.

This does not assert production browser support, public websites, history,
cross-origin frames, keyboard/forms/scrolling or network resource routing.
"""
import argparse
from test_runtime import VM, WORK, assert_success


def exercise(cpus, iso=None):
    WORK.mkdir(parents=True, exist_ok=True)
    vm = VM(cpus, storage=False, iso=iso, label='webcore-render')
    try:
        output = vm.command('webcore-render-check', timeout=200)
        assert_success(output)
        assert 'WEBCORE RENDER CHECK PASS' in output, output
        assert 'PASS genuine WebCore Grid/canvas/text pixels' in output, output
        assert output.count('PASS genuine WebCore fixture DOM') == 2, output
        assert output.count('PASS native WebKit run-loop activity observers') == 2, output
        assert output.count(
            'PASS real WebCore idle deadlines, microtasks, continuation, runnable timeout and cancellation') == 2, output

        print(
            f'PASS genuine controlled WebCore pixels/click in two renderer origins ({cpus} CPUs); production browser unfinished', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso')
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso)
