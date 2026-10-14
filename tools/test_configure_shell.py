#!/usr/bin/env python3
"""Regression for actual Autoconf shell substitutions and asynchronous signals."""
import argparse
import re
import shlex
import tarfile

from test_runtime import VM, ROOT, WORK, assert_success


def exercise(cpus, iso=None, iterations=40):
    # Use the unmodified release's actual compatibility probe. SIGCHLD during
    # command substitution previously corrupted RCX/R11 and produced false
    # feature detection, malloc faults and hung parallel configure processes.
    with tarfile.open(ROOT / 'userland/vendor/binutils-2.37.tar.gz') as archive:
        configure = archive.extractfile('binutils-2.37/libiberty/configure').read().decode()
        intl = archive.extractfile('binutils-2.37/intl/configure').read().decode()
    handler = intl.index('sigfpe_handler')
    fpe = intl[intl.rfind('#include <stdlib.h>', 0, handler):intl.index('_ACEOF', handler)]
    start = configure.index("as_nl='\n'")
    end = configure.index('# The user is always right.', start)
    script = configure[start:end] + '''test "$as_echo" = 'printf %s\\n' || exit 1
'''
    WORK.mkdir(parents=True, exist_ok=True)
    vm = VM(cpus, storage=False, iso=iso, label='configure-shell')
    try:
        def command(line, expected=None, timeout=120):
            output = vm.command(line, expected, timeout=timeout)
            assert_success(output)
            assert not re.search(
                r'exception \d+|Segmentation fault|BAD_AUTOCONF_ECHO', output), output
            return output

        command('build-check --registers', 'BUILD REGISTER CHECK PASS')
        command('build-check --suspend', 'BUILD SUSPEND CHECK PASS')

        for index, line in enumerate(fpe.splitlines()):
            value = ('write' if not index else 'append') + ' /tmp/configure-fpe.c ' + shlex.quote(line)
            assert len(value.encode()) < 511
            command(value)

        command('gcc -std=gnu11 -O2 /tmp/configure-fpe.c -o /tmp/configure-fpe')
        command('build-sh -c ' + shlex.quote('/tmp/configure-fpe && printf "UPSTREAM_FPE_OK\\n"'),
                '\nUPSTREAM_FPE_OK\n')

        for index, line in enumerate(script.splitlines()):
            value = ('write' if not index else 'append') + ' /tmp/configure-shell.sh ' + shlex.quote(line)
            assert len(value.encode()) < 511
            command(value)

        sequential = (f'i=0; while test "$i" -lt {iterations}; do '
                      '/bin/sh /tmp/configure-shell.sh || exit 1; i=$((i+1)); done; '
                      'printf "CONFIGURE_SEQUENTIAL_OK\\n"')
        command('build-sh -c ' + shlex.quote(sequential), '\nCONFIGURE_SEQUENTIAL_OK\n')
        parallel = (f'n=0; jobs=; while test "$n" -lt 4; do '
                    f'(i=0; while test "$i" -lt {iterations}; do '
                    '/bin/sh /tmp/configure-shell.sh || exit 1; i=$((i+1)); done) & '
                    'jobs="$jobs $!"; n=$((n+1)); done; '
                    'status=0; for job in $jobs; do wait "$job" || status=1; done; '
                    'test "$status" = 0 && printf "CONFIGURE_PARALLEL_OK\\n"')
        command('build-sh -c ' + shlex.quote(parallel), '\nCONFIGURE_PARALLEL_OK\n', timeout=180)

        print(f'PASS signal registers, upstream divide probe and genuine Autoconf shell: {iterations} sequential, '
              f'{4 * iterations} parallel probes on {cpus} CPU(s)', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--iso')
    parser.add_argument('--iterations', type=int, default=40)
    args = parser.parse_args()
    exercise(args.cpus, args.iso, args.iterations)
