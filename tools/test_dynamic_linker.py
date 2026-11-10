#!/usr/bin/env python3
"""Exercise the actual shared musl ABI and compiler inside Coast guests."""
import argparse
import re

from test_runtime import VM, WORK, assert_success

FIXTURES = '/usr/lib/coast/dynamic'


def exercise(cpus, iso=None, accel='tcg'):
    WORK.mkdir(parents=True, exist_ok=True)
    vm = VM(cpus, storage=False, iso=iso, label='dynamic-linker', accel=accel)
    try:
        output = vm.command('dynamic-driver-check', 'DYNAMIC DRIVER CHECK PASS', timeout=180)
        assert_success(output)
        assert output.count('DYNAMIC CHECK PASS') == 2, output
        assert output.count('DYNAMIC APP FINI PASS') == 2, output
        assert output.count('DYNAMIC PLUGIN FINI PASS') == 6, output
        assert 'DYNAMIC TRANSITIVE PASS' in output, output
        assert 'DYNAMIC ARGV0 PASS' in output and 'DYNAMIC SYMLINK ORIGIN PASS' in output, output
        assert 'Error loading shared library libdynamic-leaf.so' in output, output
        assert output.count('PASS executable text, RELRO, NX data and rejected W+X mappings') == 2, output

        # Changing the default library search path is a real ABI feature, not
        # only a fixture RPATH. Explicit identity auxv permits musl to honor it.
        output = vm.shell_command(
            "LD_LIBRARY_PATH=" + FIXTURES + "/lib " +
            FIXTURES + "/missing-library; test $? -eq 39 && echo DYNAMIC_ENV_SEARCH_PASS",
            'DYNAMIC_ENV_SEARCH_PASS', timeout=60)
        assert_success(output)
        output = vm.shell_command(
            "/usr/bin/ldd " + FIXTURES + "/dynamic-transitive",
            'libdynamic-middle.so', timeout=60)
        assert_success(output)
        assert 'libdynamic-leaf.so' in output, output
        # The installed native compiler is used here, including its real
        # startup objects, native linker and the packaged shared libc.
        output = vm.shell_command(
            "cd /tmp; /usr/bin/gcc-dynamic /usr/src/coast/dynamic/hello.c "
            "-o dynamic-native && ./dynamic-native && readelf -hl dynamic-native",
            'DYNAMIC NATIVE COMPILE PASS', timeout=180)
        assert_success(output)
        assert 'DYN (Position-Independent Executable file)' in output and 'INTERP' in output, output
        assert '/lib/ld-musl-x86_64.so.1' in output, output
        assert not re.search(r'LOAD[^\n]* RWE ', output), output
        output = vm.shell_command(
            "mkdir -p /tmp/dynamic-sdk; cp /usr/src/coast/dynamic/*.c "
            "/usr/src/coast/dynamic/Makefile /tmp/dynamic-sdk; "
            "cd /tmp/dynamic-sdk; PATH=/usr/bin:/bin make -j1 check",
            'DYNAMIC NATIVE COMPILE PASS', timeout=180)
        assert_success(output)
        assert 'DYNAMIC TRANSITIVE PASS' in output, output
        print(f'PASS guest dynamic ELF, dependency errors, TLS, W^X and native shared SDK ({cpus} CPUs)',
              flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso')
    parser.add_argument('--accel', choices=('tcg', 'kvm'), default='tcg')
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso, args.accel)
