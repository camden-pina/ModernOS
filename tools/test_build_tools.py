#!/usr/bin/env python3
"""Qualify the threaded SDK, CMake and GN inside an owned Coast VM."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import threading
import traceback

from guest_ramfs import GuestRamFS
from serve_repository import RepositoryServer
from source_tree import digest
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=runtime.ROOT / 'build/Coast-browser.iso')
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--gcc-build', type=Path, required=True)
    parser.add_argument('--cmake-build', type=Path, required=True)
    parser.add_argument('--gn-build', type=Path, required=True)
    parser.add_argument('--ninja', type=Path, required=True)
    parser.add_argument('--native-rebuild', action='store_true', help='also rebuild GCC, CMake and GN entirely inside Coast')
    parser.add_argument('--native-toolchain-archive', type=Path,
                        help='resume consumers using the compiler exported by a previous native build')
    parser.add_argument('--native-toolchain-report', type=Path,
                        help='qualification report proving the native compiler archive and its source inputs')
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    if bool(args.native_toolchain_archive) != bool(args.native_toolchain_report) or args.native_toolchain_archive and not args.native_rebuild:
        parser.error('A native compiler continuation requires both archive and report, with --native-rebuild')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    records = {}
    assets = {}
    for name in ('gcc', 'cmake', 'gn'):
        path = getattr(args, name + '_build').resolve(strict=True)
        record = json.loads(path.read_text())
        (out / ('fixture-' + name + '-build.json')).write_bytes(path.read_bytes())
        if record['target_host_executed'] is not False:
            parser.error('Select verified cross-built fixtures for the initial guest qualification')
        for relative, expected in record['source_sha256'].items():
            if digest(runtime.ROOT / relative) != expected:
                raise ValueError('Build source changed: ' + relative)
        archive = out / (name + '.tar.gz')
        with tarfile.open(archive, 'w:gz') as bundle:
            directories = set()
            for relative, asset in record['files'].items():
                source = Path(asset['path'])
                if digest(source) != asset['sha256']:
                    raise ValueError('Build fixture changed: ' + relative)
                for directory in reversed(Path(relative).parents):
                    if str(directory) != '.' and str(directory) not in directories:
                        member = tarfile.TarInfo(str(directory))
                        member.mode, member.type = 0o755, tarfile.DIRTYPE
                        bundle.addfile(member)
                        directories.add(str(directory))
                info = tarfile.TarInfo(relative)
                info.mode, info.size = asset['mode'], source.stat().st_size
                with source.open('rb') as stream:
                    bundle.addfile(info, stream)
        assets[name + '.tar.gz'] = archive
        records[name] = dict(receipt_sha256=digest(path), archive_sha256=digest(archive))
    kit = out / 'coast-build-tools.tar.gz'
    inputs = ['LICENSE', 'ports/build-tools/SOURCES.json', 'ports/build-tools/Coast.cmake',
        'ports/build-tools/cmake-spawn.patch', 'ports/build-tools/gn-spawn.patch',
        'ports/build-tools/cmake_check.py', 'ports/build-tools/gn_check.py', 'ports/build-tools/thread_check.cc',
        'ports/build-tools/spawn_check.c', 'ports/build-tools/tls_check.py', 'ports/build-tools/tls_check.c',
        'ports/gnu-runtime/grep_check.py', 'ports/gnu-runtime/bison_check.py',
        'tools/build_cmake.py', 'tools/build_gn.py', 'tools/fetch_sources.py', 'tools/linux_uapi.py',
        'tools/source_archive.py', 'tools/source_tree.py', 'userland/sdk/bootstrap-toolchain.sh',
        'userland/sdk/prepare-gcc-options.sh']
    with tarfile.open(kit, 'w:gz') as bundle:
        for relative in inputs:
            bundle.add(runtime.ROOT / relative, arcname=relative, recursive=False)
    assets['source.tar.gz'] = kit
    assets['ninja'] = args.ninja.resolve(strict=True)
    if args.native_toolchain_archive:
        previous = json.loads(args.native_toolchain_report.read_text())
        stage = 'rebuild binutils, GCC, libgcc and POSIX libstdc++ from original sources inside Coast'
        archive_sha = digest(args.native_toolchain_archive)
        if (not previous.get('native_rebuild') or previous.get('target_execution_on_linux') is not False or
                stage not in previous.get('checks', []) or
                previous.get('exports', {}).get('native-toolchain.tar.gz') != archive_sha):
            parser.error('The supplied report does not prove this native compiler archive')
        for name in ['userland/sdk/bootstrap-toolchain.sh', 'userland/sdk/prepare-gcc-options.sh']:
            if previous['source_files'].get(name) != digest(runtime.ROOT / name):
                parser.error('Native compiler source changed: ' + name)
        assets['native-toolchain.tar.gz'] = args.native_toolchain_archive.resolve(strict=True)
        records['native_compiler_continuation'] = dict(archive_sha256=archive_sha,
            report_sha256=digest(args.native_toolchain_report), original_iso_sha256=previous['iso_sha256'])
    compiler_wrapper = out / 'compiler-wrapper'
    compiler_wrapper.write_text('#!/bin/sh\ncase "${0##*/}" in g++|c++) compiler=g++;; *) compiler=gcc;; esac\n'
        'exec /home/user/build-tools/sdk/usr/bin/$compiler -L/home/user/build-tools/sdk/usr/lib "$@"\n')
    assets['compiler-wrapper'] = compiler_wrapper
    sources = json.loads((runtime.ROOT / 'ports/build-tools/SOURCES.json').read_text())
    for record in sources.values():
        archive = runtime.ROOT / 'build/build-tools/vendor' / record['archive']
        if digest(archive) != record['sha256']:
            raise ValueError('Upstream source fixture changed')
        assets[record['archive']] = archive
    report = dict(state='failed', checks=[], fixtures=records,
        repository_manifest_sha256=digest(args.repository),
        iso_sha256=digest(args.iso.resolve()), source_kit_sha256=digest(kit),
        source_files={p: digest(runtime.ROOT / p) for p in inputs},
        harness_sha256=digest(Path(__file__)),
        native_rebuild=args.native_rebuild, target_execution_on_linux=False)
    kernel = out / 'boot-kernel.elf'
    subprocess.run(['xorriso', '-osirrox', 'on', '-indev', str(args.iso.resolve()),
        '-extract', '/boot/kernel.elf', str(kernel)], check=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    vm = server = thread = None
    export_failed = False

    def check(name, command, expected, user=True, timeout=180):
        print('START ' + name, flush=True)
        if command.startswith(('brine server ', 'brine install ')):
            vm.command('true')
        # The login console accepts 511-byte lines. Deliver longer build
        # scripts through the owned fixture server and execute them in Coast.
        if len(command.encode()) > 240 or '\n' in command:
            script = out / ('command-' + str(len(report['checks'])) + '.sh')
            script.write_text('set -e\n' + command + '\n')
            stat = script.stat()
            server.repository['assets']['/distfiles/build-tools-' + script.name] = dict(path=script,
                sha256=digest(script), identity=(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
            command = 'curl -fsS ' + origin + script.name + ' -o /tmp/build-tools-command.sh && build-sh /tmp/build-tools-command.sh'
        output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out / (f'{len(report["checks"]):02d}-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)
        return output

    try:
        server = RepositoryServer(('127.0.0.1', 0), args.repository)
        for name, path in assets.items():
            path = path.resolve(strict=True)
            stat = path.stat()
            server.repository['assets']['/distfiles/build-tools-' + name] = dict(path=path,
                sha256=digest(path), identity=(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        origin = 'http://10.0.2.2:' + str(server.server_port) + '/distfiles/build-tools-'
        vm = runtime.VM(4, storage=False, network=True, memory='24G', accel='kvm',
            iso=args.iso.resolve(), label='native-build-tools')
        vm.command('true')
        check('select the signed native dependency repository', 'brine server ' + origin.split('/distfiles/')[0],
            'Package downloads and source builds now use')
        check('verify repository signatures', 'pacman -Syy --noconfirm', 'Synchronizing', user=False)
        check('refresh the matching signed file database', 'pacman -Fyy --noconfirm', 'Synchronizing', user=False)
        check('create an isolated normal-user build workspace',
            'mkdir -p /home/user/build-tools/source /home/user/build-tools/sdk /home/user/build-tools/cmake /home/user/build-tools/gn /home/user/build-tools/bin && echo WORKSPACE_READY', 'WORKSPACE_READY')
        for asset, directory in [('gcc.tar.gz', 'sdk'), ('cmake.tar.gz', 'cmake'), ('gn.tar.gz', 'gn'), ('source.tar.gz', 'source')]:
            check('fetch and unpack ' + asset,
                'curl -fsS ' + origin + asset + ' -o /tmp/' + asset + ' && tar -xzf /tmp/' + asset +
                ' -C /home/user/build-tools/' + directory + ' && echo FIXTURE_READY', 'FIXTURE_READY', timeout=240)
        check('fetch the verified native Ninja', 'curl -fsS ' + origin + 'ninja -o /tmp/ninja && chmod 755 /tmp/ninja && /tmp/ninja --version', '1.13.2')
        check('select the isolated compiler runtime for linking',
            'curl -fsS ' + origin + 'compiler-wrapper -o /home/user/build-tools/bin/gcc && '
            'chmod 755 /home/user/build-tools/bin/gcc && '
            'cp /home/user/build-tools/bin/gcc /home/user/build-tools/bin/g++ && '
            'cp /home/user/build-tools/bin/gcc /home/user/build-tools/bin/cc && '
            'cp /home/user/build-tools/bin/gcc /home/user/build-tools/bin/c++ && echo ISOLATED_SDK_READY', 'ISOLATED_SDK_READY')
        for record in sources.values():
            check('fetch checksum-pinned ' + record['archive'],
                'curl -fsS ' + origin + record['archive'] + ' -o /tmp/' + record['archive'] +
                ' && echo UPSTREAM_SOURCE_READY', 'UPSTREAM_SOURCE_READY')
        # Keep the fixtures separate from all package-owned compiler/runtime files.
        path = 'export PATH=/home/user/build-tools/bin:/usr/bin:/bin; '
        check('native compiler reports its POSIX thread model', path + 'g++ -v 2>&1', 'Thread model: posix')
        check('compile and run C++ mutex, condition variable, TLS, exception, future and shared-lock checks',
            path + 'g++ -std=c++17 -O2 -static -no-pie -pthread '
            '/home/user/build-tools/source/ports/build-tools/thread_check.cc -o /tmp/thread-check && /tmp/thread-check', 'CXX_THREADS_PASS')
        check('parallel worker spawn preserves parent state and cleans up successful and failed children',
            path + 'gcc -std=c11 -O2 -Wall -Wextra -Werror -static -no-pie -pthread '
            '/home/user/build-tools/source/ports/build-tools/spawn_check.c -o /tmp/spawn-check && /tmp/spawn-check',
            'NATIVE_SPAWN_PASS', timeout=240)
        check('zero-filled TLS outside mapped segments works and malformed TLS remains rejected', path +
            'python3 /home/user/build-tools/source/ports/build-tools/tls_check.py', 'TLS_ALL_PASS', timeout=180)
        check('CMake and CTest build, run, install and package through Make and Ninja', path +
            'python3 /home/user/build-tools/source/ports/build-tools/cmake_check.py '
            '--runtime /home/user/build-tools/cmake --ninja /tmp/ninja', 'CMAKE_ALL_PASS', timeout=600)
        check('GN generates native graphs and captures real threaded Python subprocesses', path +
            'python3 /home/user/build-tools/source/ports/build-tools/gn_check.py '
            '--gn /home/user/build-tools/gn/usr/bin/gn --ninja /tmp/ninja', 'GN_ALL_PASS', timeout=600)
        if args.native_rebuild:
            check('install the qualified GNU grep fallback',
                'brine install grep --noconfirm', 'grep', timeout=600)
            check('install the qualified native parser generator',
                'brine install bison --noconfirm', 'bison', timeout=600)
            check('qualify the installed native GNU grep on large real pipes',
                'python3 /home/user/build-tools/source/ports/gnu-runtime/grep_check.py', 'GREP_ALL_PASS', timeout=180)
            check('qualify the installed native Bison and m4 parser workflow',
                'python3 /home/user/build-tools/source/ports/gnu-runtime/bison_check.py', 'BISON_ALL_PASS', timeout=180)
            check('install native build tools and original offline source closure',
                'brine install coast-build --noconfirm', 'installing coast-build', timeout=600)
            if args.native_toolchain_archive:
                check('restore the compiler previously rebuilt from original sources inside Coast',
                    'mkdir -p /home/user/Builds/toolchain/install && curl -fsS ' + origin +
                    'native-toolchain.tar.gz -o /tmp/native-toolchain.tar.gz && '
                    'tar -xzf /tmp/native-toolchain.tar.gz -C /home/user/Builds/toolchain/install && '
                    '/home/user/Builds/toolchain/install/bin/gcc -dumpfullversion', '14.2.0', timeout=600)
                check('qualify the restored native compiler on C++ threads, futures, TLS and exceptions',
                    '/home/user/Builds/toolchain/install/bin/g++ -std=c++17 -O2 -pthread '
                    '/home/user/build-tools/source/ports/build-tools/thread_check.cc -o /tmp/restored-native-thread-check && '
                    '/tmp/restored-native-thread-check', 'CXX_THREADS_PASS', timeout=240)
            else:
                check('rebuild binutils, GCC, libgcc and POSIX libstdc++ from original sources inside Coast',
                'build-sh /home/user/build-tools/source/userland/sdk/bootstrap-toolchain.sh '
                '--work /home/user/Builds/toolchain --posix-threads -j4 > /tmp/native-toolchain.log 2>&1 &\n'
                'native_pid=$!\n'
                'while kill -0 "$native_pid" 2>/dev/null; do\n'
                '  if test -f /home/user/Builds/toolchain/.coast-stage; then\n'
                '    printf "NATIVE_TOOLCHAIN_STAGE="; cat /home/user/Builds/toolchain/.coast-stage\n'
                '  fi\n'
                '  tail -n 2 /tmp/native-toolchain.log || :\n'
                '  sleep 30\n'
                'done\nwait "$native_pid"\n'
                    'echo NATIVE_THREADED_TOOLCHAIN_PASS', 'NATIVE_THREADED_TOOLCHAIN_PASS', timeout=14400)
            check('archive the genuine native toolchain before building its consumers',
                'cd /home/user/Builds/toolchain/install && tar -czf /tmp/native-toolchain.tar.gz . && echo NATIVE_TOOLCHAIN_ARCHIVED',
                'NATIVE_TOOLCHAIN_ARCHIVED', timeout=300)
            path = ('export PATH=/home/user/Builds/toolchain/install/bin:/usr/bin:/bin '
                'CC=/home/user/Builds/toolchain/install/bin/gcc '
                'CXX=/home/user/Builds/toolchain/install/bin/g++; ')
            check('rebuild CMake and all of its bundled libraries with the rebuilt native compiler', path +
                'cd /home/user/build-tools/source && mkdir -p build/build-tools/vendor && '
                'cp /tmp/cmake-4.4.5.tar.gz build/build-tools/vendor/ && '
                'python3 -B tools/build_cmake.py --native --native-toolchain /home/user/Builds/toolchain/install/bin --jobs 4 '
                '> /tmp/native-cmake.log 2>&1 && '
                'tar -czf /tmp/native-cmake-runtime.tar.gz -C build/cmake-native/runtime usr && '
                'echo NATIVE_CMAKE_BUILD_PASS', 'NATIVE_CMAKE_BUILD_PASS', timeout=7200)
            check('qualify the newly native-built CMake and CTest', path +
                'cd /home/user/build-tools/source && python3 ports/build-tools/cmake_check.py '
                '--runtime build/cmake-native/runtime --ninja /tmp/ninja', 'CMAKE_ALL_PASS', timeout=600)
            check('rebuild the exact Chromium GN with the native compiler', path +
                'cd /home/user/build-tools/source && cp /tmp/gn-3357c4f51b1a9e676378c695dd9c7e9911c35ee6.tar.gz build/build-tools/vendor/ && '
                'python3 -B tools/build_gn.py --native --native-toolchain /home/user/Builds/toolchain/install/bin '
                '--ninja /tmp/ninja --jobs 4 > /tmp/native-gn.log 2>&1 && '
                'tar -czf /tmp/native-gn-runtime.tar.gz -C build/gn-native/runtime usr && echo NATIVE_GN_BUILD_PASS',
                'NATIVE_GN_BUILD_PASS', timeout=3600)
            check('qualify newly native-built GN, Python helpers and Ninja C++ consumers', path +
                'cd /home/user/build-tools/source && python3 ports/build-tools/gn_check.py '
                '--gn build/gn-native/runtime/usr/bin/gn --ninja /tmp/ninja', 'GN_ALL_PASS', timeout=600)
            check('archive the native-built CMake and GN runtimes',
                'test -f /tmp/native-cmake-runtime.tar.gz && test -f /tmp/native-gn-runtime.tar.gz && '
                'echo NATIVE_BUILD_TOOLS_ARCHIVED', 'NATIVE_BUILD_TOOLS_ARCHIVED', timeout=300)
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('Coast remains healthy after compiler, threads and subprocess validation')
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
                exports = {'/tmp/native-toolchain.log': 'native-toolchain.log',
                    '/tmp/native-cmake.log': 'native-cmake.log', '/tmp/native-gn.log': 'native-gn.log',
                    '/tmp/native-toolchain.tar.gz': 'native-toolchain.tar.gz',
                    '/tmp/native-cmake-runtime.tar.gz': 'native-cmake-runtime.tar.gz',
                    '/tmp/native-gn-runtime.tar.gz': 'native-gn-runtime.tar.gz',
                    '/home/user/build-tools/source/build/cmake-native/BUILD.json': 'native-cmake.json',
                    '/home/user/build-tools/source/build/gn-native/BUILD.json': 'native-gn.json'}
                for tool in ['cmake', 'gn']:
                    for name in ['bootstrap.log', 'configure.log', 'build.log', 'install.log']:
                        exports['/home/user/build-tools/source/build/' + tool + '-native/' + name] = 'native-' + tool + '-' + name
                # Capture integration logs even when a helper fails before its receipt.
                temporary = ramfs.find('/tmp')
                address = temporary['children']
                while address:
                    node = ramfs.node(address)
                    if node['name'].startswith(('cmake-check-', 'gn-check-', 'tls-check-')):
                        for name in ('check.log', 'report.json'):
                            exports['/tmp/' + node['name'] + '/' + name] = node['name'] + '-' + name
                    address = node['next']
                for source, name in exports.items():
                    try:
                        ramfs.export(source, out / name)
                        report.setdefault('exports', {})[name] = digest(out / name)
                    except FileNotFoundError:
                        pass
            except BaseException as error:
                report['export_error'] = repr(error)
                if args.native_rebuild and report['state'] == 'passed':
                    report['state'] = 'failed'
                    export_failed = True
            finally:
                vm.close()
        if server:
            server.shutdown()
            server.server_close()
        if thread:
            thread.join(5)
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if export_failed:
        raise RuntimeError('Native build artifacts could not be exported; see report.json')


if __name__ == '__main__':
    main()
