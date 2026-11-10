#!/usr/bin/env python3
"""Qualify signed GCC/CMake/GN package upgrades using only installed /usr tools."""
import argparse
import json
from pathlib import Path
import subprocess
import threading
import traceback

from guest_ramfs import GuestRamFS
from serve_repository import RepositoryServer
from source_tree import digest
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--with-llvm', action='store_true', help='also install and qualify the updated Python and LLVM packages')
    args = parser.parse_args()
    repository = args.repository.resolve(strict=True)
    snapshot = json.loads(repository.read_text())
    versions = {name: snapshot['packages'][name]['version']
                for name in ('gcc', 'ninja', 'cmake', 'gn', 'cmake-build', 'gn-build')}
    if args.with_llvm:
        versions.update({name: snapshot['packages'][name]['version'] for name in ('python', 'llvm', 'llvm-build')})
    if versions['gcc'] != '14.2.0-2':
        parser.error('Select the POSIX GCC upgrade candidate')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    report = dict(state='failed', checks=[], versions=versions,
        repository_manifest_sha256=digest(repository), iso_sha256=digest(args.iso.resolve()),
        harness_sha256=digest(Path(__file__)), target_execution_on_linux=False)
    kernel = out / 'kernel.elf'
    subprocess.run(['xorriso', '-no_rc', '-osirrox', 'on', '-indev', str(args.iso.resolve()),
        '-extract', '/boot/kernel.elf', str(kernel)], check=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    vm = server = thread = None

    def check(name, command, expected, user=True, timeout=240):
        print('START ' + name, flush=True)
        if command.startswith(('coast server ', 'coast install ')):
            vm.command('true')
        if len(command.encode()) > 240 or '\n' in command:
            path = out / ('command-' + str(len(report['checks'])) + '.sh')
            path.write_text('set -e\nexport PATH=/usr/bin:/bin\n' + command + '\n')
            stat = path.stat()
            server.repository['assets']['/distfiles/installed-tools-' + path.name] = dict(path=path,
                sha256=digest(path), identity=(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
            command = 'curl -fsS ' + origin + '/distfiles/installed-tools-' + path.name + ' -o /tmp/installed-tools.sh && build-sh /tmp/installed-tools.sh'
        output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out / (f'{len(report["checks"]):02d}-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)

    try:
        server = RepositoryServer(('127.0.0.1', 0), repository)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        origin = 'http://10.0.2.2:' + str(server.server_port)
        vm = runtime.VM(4, storage=False, network=True, memory='12G', accel='kvm',
            iso=args.iso.resolve(), label='installed-build-tools')
        vm.command('true')
        check('select the signed build-tools candidate', 'coast server ' + origin,
            'Package downloads and source builds now use')
        check('verify signed package metadata', 'pacman -Syy --noconfirm', 'Synchronizing', user=False)
        check('verify the matching signed file database', 'pacman -Fyy --noconfirm', 'Synchronizing', user=False)
        for name in versions:
            check('install signed ' + name + ' ' + versions[name], 'coast install ' + name + ' --noconfirm', name, timeout=600)
        check('installed package ownership and bytes agree', 'pacman -Qkk ' + ' '.join(versions), '0 altered files', timeout=300)
        check('the installed C++ compiler enables POSIX threads', 'g++ -v 2>&1', 'Thread model: posix')
        check('the installed compiler bootstrap has its owned helper', 'bootstrap-toolchain --help', '--posix-threads')
        prefix = '/home/user/installed-tools/coast-cmake-' + versions['cmake'].rsplit('-', 1)[0]
        check('unpack the installed offline source kit',
            'mkdir -p /home/user/installed-tools && tar -xzf /usr/src/coast/cmake-build/coast-cmake-' +
            versions['cmake'].rsplit('-', 1)[0] + '.tar.gz -C /home/user/installed-tools && echo INSTALLED_SOURCES_READY',
            'INSTALLED_SOURCES_READY')
        check('installed GCC runs threads, futures, TLS and exceptions',
            'g++ -std=c++17 -O2 -mno-avx -mno-avx2 -static -no-pie -pthread ' +
            prefix + '/ports/build-tools/thread_check.cc -o /tmp/installed-thread-check && /tmp/installed-thread-check',
            'CXX_THREADS_PASS')
        check('installed compiler and runtime preserve parallel spawn state',
            'gcc -std=c11 -O2 -mno-avx -mno-avx2 -Wall -Wextra -Werror -static -no-pie -pthread ' +
            prefix + '/ports/build-tools/spawn_check.c -o /tmp/installed-spawn-check && /tmp/installed-spawn-check',
            'NATIVE_SPAWN_PASS')
        check('installed CMake builds, tests, installs and packages with Make and Ninja',
            'python3 ' + prefix + '/ports/build-tools/cmake_check.py', 'CMAKE_ALL_PASS', timeout=900)
        check('installed GN and Ninja build consumers and propagate Python failures',
            'python3 ' + prefix + '/ports/build-tools/gn_check.py', 'GN_ALL_PASS', timeout=600)
        if args.with_llvm:
            check('installed Python runs 128 threaded subprocesses with intact parent state',
                'python3 /usr/share/coast/python/spawn_check.py', 'PYTHON_THREADED_SPAWN_PASS', timeout=600)
            check('installed Clang finds the normal system C SDK and produces a working executable',
                "printf '%s\\n' '#include <stdio.h>' 'int main(void){puts(\"LLVM_SYSTEM_C_PASS\");return 0;}' > /tmp/llvm-system.c\n"
                'clang -O2 /tmp/llvm-system.c -o /tmp/llvm-system && /tmp/llvm-system', 'LLVM_SYSTEM_C_PASS')
            check('installed Clang++ and LLD use the normal C++ thread SDK',
                'clang++ -std=c++17 -O2 -pthread -fuse-ld=lld ' + prefix +
                '/ports/build-tools/thread_check.cc -o /tmp/llvm-system-threads && /tmp/llvm-system-threads', 'CXX_THREADS_PASS')
            llvm_version = versions['llvm'].rsplit('-', 1)[0]
            llvm_prefix = '/home/user/installed-llvm/coast-llvm-' + llvm_version
            check('unpack the installed complete LLVM and zlib source kit',
                'mkdir -p /home/user/installed-llvm && tar -xzf /usr/src/coast/llvm-build/coast-llvm-' + llvm_version +
                '.tar.gz -C /home/user/installed-llvm && echo LLVM_SOURCE_KIT_READY', 'LLVM_SOURCE_KIT_READY', timeout=600)
            check('installed LLVM and Python pass C/C++, threads, LLD, ThinLTO and concurrent compiler workflows',
                'python3 ' + llvm_prefix + '/ports/llvm/check.py --runtime / --sysroot /', 'LLVM_ALL_PASS', timeout=900)
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('Coast remains healthy after signed tool upgrades')
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        raise
    finally:
        if vm:
            try:
                vm.qmp('stop')
                ramfs = GuestRamFS(vm, kernel, out / 'memory')
                address = ramfs.find('/tmp')['children']
                while address:
                    node = ramfs.node(address)
                    if node['name'].startswith(('cmake-check-', 'gn-check-')):
                        for name in ('check.log', 'report.json'):
                            target = node['name'] + '-' + name
                            ramfs.export('/tmp/' + node['name'] + '/' + name, out / target)
                            report.setdefault('exports', {})[target] = digest(out / target)
                    address = node['next']
            except BaseException as error:
                report.update(state='failed', export_error=repr(error))
            finally:
                vm.close()
        if server:
            server.shutdown()
            server.server_close()
        if thread:
            thread.join(5)
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
