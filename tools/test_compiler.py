#!/usr/bin/env python3
"""Compile/link/run real C/C++ programs inside the booted ModernOS SDK."""
import argparse
import hashlib
import re
import shlex
import time

from test_runtime import VM, WORK, prepare, run
from test_desktop import DesktopTest
from test_rebuilt_toolchain import CPP20, write_source


def exercise(cpus, storage=False, iso=None):
    WORK.mkdir(parents=True, exist_ok=True)
    if storage:
        prepare()
        source_hash = hashlib.sha256((WORK / 'linux.img').read_bytes()).digest()
    vm = VM(cpus, storage=storage, iso=iso, label='compiler')

    def command(line, expected=None, status=0, timeout=120):
        assert len(line.encode()) < 511 and '\n' not in line
        output = vm.command(line, expected, timeout=timeout)
        exits = re.findall(r'pid \d+ exited (\d+)', output)
        assert not exits if not status else exits and int(exits[-1]) == status, output
        assert not re.search(r'pid \d+ terminated by signal', output), output
        return output

    def shell(script, expected=None, status=0):
        return command('build-sh -c ' + shlex.quote(script), expected, status)

    try:
        command('compiler-check', 'COMPILER ABI CHECK PASS')
        command('gcc --version', '14.2.0')
        command('g++ --version', '14.2.0')
        shell('mkdir /tmp/sdk-test; cp -R /usr/src/modernos/examples/. /tmp/sdk-test/')
        shell('cd /tmp/sdk-test; make -j2 check', 'NATIVE_CPP_OK')
        shell('cd /tmp/sdk-test; make -q')
        write_source(shell, '/tmp/sdk-test/hello20.cpp', CPP20)
        shell('g++ -std=c++20 -O2 /tmp/sdk-test/hello20.cpp -o /tmp/sdk-test/hello20 && '
              '/tmp/sdk-test/hello20', 'NATIVE_CPP20_OK')
        shell('gcc -O2 /tmp/sdk-test/abi-check.c -o /tmp/sdk-test/abi-check')
        command('/tmp/sdk-test/abi-check', 'COMPILER ABI CHECK PASS')
        print('PASS native GCC/G++: compile, link, C++17 containers/exceptions, C++20 span and compiler ABI checks', flush=True)

        # Object archives and assembler/linker executables are genuine tools.
        shell('cd /tmp/sdk-test; ar rcs libhello.a hello.o; ranlib libhello.a; '
              'gcc libhello.a -o archived && ./archived', 'NATIVE_GCC_RUN_OK')
        shell('cd /tmp/sdk-test; gcc -S hello.c -o hello.s; '
              'as hello.s -o assembled.o; gcc assembled.o -o assembled && ./assembled',
              'NATIVE_GCC_RUN_OK')
        info = shell('readelf -hl /tmp/sdk-test/hello-c')
        assert 'EXEC (Executable file)' in info and 'INTERP' not in info, info
        for line in info.splitlines():
            if ' RWE ' in line:
                raise AssertionError(
                    'Compiler produced a writable/executable load segment: ' + line)

        shell('printf "int main( { broken; }\\n" > /tmp/sdk-test/bad.c')
        shell('gcc /tmp/sdk-test/bad.c -o /tmp/sdk-test/bad', 'error:', status=1)
        shell('test ! -e /tmp/sdk-test/bad')
        command('gcc --invalid-modernos-option', 'unrecognized command-line option', status=1)
        print('PASS native binutils/static ELF and compiler errors without false success', flush=True)

        # Run a native compile through the desktop's actual PTY-backed shell.
        vm.send('gui --trace\n')
        vm.until(b'GUI_READY', 30)
        desktop = DesktopTest(vm)
        desktop.screenshot(f'compiler-desktop-{cpus}cpu')
        desktop.launch('Terminal')
        script = 'gcc -O2 /tmp/sdk-test/hello.c -o /tmp/gui-compiled && /tmp/gui-compiled > /tmp/gui-compiler-result'
        line = 'build-sh -c ' + shlex.quote(script)
        vm.send(line + '\n')
        output = desktop.expect('TERMINAL_COMMAND', line)

        # Wait for the real PTY shell to regain input before asking to quit;
        # quitting a live compiler correctly opens a confirmation dialog.
        output = output[output.rfind('DESKTOP TERMINAL_COMMAND'):]
        if 'os:/home/user$ ' not in output:
            vm.until(rb'os:/home/user\$ ', 120)
        desktop.key('2', 'meta_l')

        assert 'workspace=2' in desktop.expect('WORKSPACE')
        desktop.key('1', 'meta_l')

        assert 'workspace=1' in desktop.expect('WORKSPACE')
        desktop.exit()
        command('cat /tmp/gui-compiler-result', 'NATIVE_GCC_RUN_OK')
        print('PASS native compiler from GUI Terminal and responsive workspace switching', flush=True)

        if storage:
            shell('cp /tmp/sdk-test/hello-c /mnt/nvme1n1p1/compiled-c; '
                  'cp /tmp/sdk-test/hello-cxx /mnt/nvme1n1p1/compiled-cxx; '
                  'cp /tmp/sdk-test/hello20 /mnt/nvme1n1p1/compiled-cxx20')
            command('/mnt/nvme1n1p1/compiled-c', 'NATIVE_GCC_RUN_OK')
            command('/mnt/nvme1n1p1/compiled-cxx', 'NATIVE_CPP_OK')
            command('/mnt/nvme1n1p1/compiled-cxx20', 'NATIVE_CPP20_OK')
            command('sync')
    finally:
        vm.close()

    if storage:
        image = str(WORK / 'fat.img') + '@@1048576'
        for name in ('compiled-c', 'compiled-cxx', 'compiled-cxx20'):
            exported = WORK / (name + '.elf')
            run('mcopy', '-D', 'o', '-i', image, '::/' + name, exported)
            elf = run('readelf', '-hl', exported).decode()
            assert 'EXEC (Executable file)' in elf and 'INTERP' not in elf, elf

        assert source_hash == hashlib.sha256((WORK / 'linux.img').read_bytes()).digest()
        checked = WORK / 'compiler-fat.volume'
        with (WORK / 'fat.img').open('rb') as disk, checked.open('wb') as output:
            disk.seek(2048 * 512)
            output.write(disk.read((WORK / 'fat.volume').stat().st_size))
        run('fsck.fat', '-n', checked)
        vm = VM(cpus, storage=True, iso=iso, label='compiler')
        try:
            vm.command('/mnt/nvme1n1p1/compiled-c', 'NATIVE_GCC_RUN_OK')
            vm.command('/mnt/nvme1n1p1/compiled-cxx', 'NATIVE_CPP_OK')
            vm.command('/mnt/nvme1n1p1/compiled-cxx20', 'NATIVE_CPP20_OK')
        finally:
            vm.close()

        print('PASS native compiled binaries persist/run after reboot; source unchanged; FAT fsck', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, default=1)
    parser.add_argument('--storage', action='store_true')
    parser.add_argument('--iso')
    arguments = parser.parse_args()
    exercise(arguments.cpus, arguments.storage, arguments.iso)
