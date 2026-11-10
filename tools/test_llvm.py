#!/usr/bin/env python3
"""Test source-built LLVM and optionally rebuild it entirely inside Coast."""
import argparse
import json
from pathlib import Path, PurePosixPath
import posixpath
import subprocess
import tarfile
import threading
import traceback

from guest_ramfs import GuestRamFS
from serve_repository import RepositoryServer
from source_tree import digest
import test_runtime as runtime


def fixture(receipt, destination):
    record = json.loads(receipt.read_text())
    if record['target_host_executed'] is not False:
        raise ValueError('Use a cross-built fixture: ' + str(receipt))
    for name, expected in record['source_sha256'].items():
        if digest(runtime.ROOT / name) != expected:
            raise ValueError('Fixture source changed: ' + name)
    with tarfile.open(destination, 'w:gz') as bundle:
        directories = set()
        for name, asset in record['files'].items():
            if PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts:
                raise ValueError('Unsafe LLVM fixture path')
            source = Path(asset['path'])
            if digest(source) != asset['sha256']:
                raise ValueError('Fixture bytes changed: ' + name)
            for parent in reversed(PurePosixPath(name).parents):
                if str(parent) != '.' and str(parent) not in directories:
                    info = tarfile.TarInfo(str(parent))
                    info.mode, info.type = 0o755, tarfile.DIRTYPE
                    bundle.addfile(info)
                    directories.add(str(parent))
            info = tarfile.TarInfo(name)
            info.mode, info.size = asset.get('mode', 0o755 if source.stat().st_mode & 0o111 else 0o644), source.stat().st_size
            with source.open('rb') as stream:
                bundle.addfile(info, stream)
        for name, target in record.get('aliases', {}).items():
            if (PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts or
                    PurePosixPath(target).is_absolute() or target in ('', '.', '..')):
                raise ValueError('Unsafe LLVM fixture alias')
            resolved, seen = name, set()
            while resolved in record.get('aliases', {}):
                if resolved in seen:
                    raise ValueError('Cyclic fixture alias')
                seen.add(resolved)
                link = record['aliases'][resolved]
                if PurePosixPath(link).is_absolute():
                    raise ValueError('Absolute fixture alias')
                resolved = posixpath.normpath(posixpath.join(posixpath.dirname(resolved), link))
                if resolved == '..' or resolved.startswith('../'):
                    raise ValueError('Fixture alias escapes its runtime')
            if resolved not in record['files']:
                raise ValueError('Fixture alias target is absent: ' + name)
            info = tarfile.TarInfo(name)
            info.type, info.mode, info.linkname = tarfile.SYMTYPE, 0o777, target
            bundle.addfile(info)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--llvm-build', type=Path, required=True)
    parser.add_argument('--gcc-build', type=Path, required=True)
    parser.add_argument('--cmake-build', type=Path, required=True)
    parser.add_argument('--ninja', type=Path, required=True)
    parser.add_argument('--python-build', type=Path, help='qualify an isolated source-built native Python fixture')
    parser.add_argument('--native-rebuild', action='store_true')
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    assets, records = {}, {}
    for name in ['llvm', 'gcc', 'cmake']:
        receipt = getattr(args, name + '_build').resolve(strict=True)
        archive = out / (name + '.tar.gz')
        record = fixture(receipt, archive)
        (out / (name + '-BUILD.json')).write_bytes(receipt.read_bytes())
        records[name] = dict(receipt_sha256=digest(receipt), archive_sha256=digest(archive))
        assets[archive.name] = archive
    if args.python_build:
        archive = out / 'python.tar.gz'
        fixture(args.python_build.resolve(strict=True), archive)
        records['python'] = dict(receipt_sha256=digest(args.python_build), archive_sha256=digest(archive))
        assets[archive.name] = archive
    pin = json.loads((runtime.ROOT / 'ports/llvm/SOURCES.json').read_text())
    upstream = args.llvm_build.resolve().parent / 'vendor' / pin['archive']
    if digest(upstream) != pin['sha256']:
        raise ValueError('LLVM upstream source changed')
    assets[pin['archive']] = upstream
    zlib = json.loads((runtime.ROOT / 'userland/vendor/qemu-sources.json').read_text())['zlib']
    zlib_source = args.llvm_build.resolve().parent / 'vendor' / zlib['archive']
    if digest(zlib_source) != zlib['sha256']:
        raise ValueError('LLVM zlib source changed')
    assets[zlib['archive']] = zlib_source
    assets['ninja'] = args.ninja.resolve(strict=True)
    inputs = ['LICENSE', 'tools/build_llvm.py', 'tools/source_tree.py', 'tools/source_archive.py',
        'tools/linux_uapi.py', 'ports/llvm/SOURCES.json', 'ports/llvm/native-spawn.patch',
        'ports/llvm/check.py', 'ports/build-tools/thread_check.cc', 'userland/vendor/qemu-sources.json',
        'ports/python/spawn_check.py']
    kit = out / 'source.tar.gz'
    with tarfile.open(kit, 'w:gz') as bundle:
        for name in inputs:
            bundle.add(runtime.ROOT / name, arcname=name, recursive=False)
    assets[kit.name] = kit
    wrapper = out / 'compiler-wrapper'
    wrapper.write_text('#!/bin/sh\ncase "${0##*/}" in g++|c++) compiler=g++;; *) compiler=gcc;; esac\n'
        'exec /home/user/llvm/sdk/usr/bin/$compiler -L/home/user/llvm/sdk/usr/lib "$@"\n')
    assets['compiler-wrapper'] = wrapper
    report = dict(state='failed', checks=[], fixtures=records, source_files={p: digest(runtime.ROOT / p) for p in inputs},
        harness_sha256=digest(Path(__file__)), repository_manifest_sha256=digest(args.repository),
        iso_sha256=digest(args.iso), native_rebuild=args.native_rebuild, target_execution_on_linux=False)
    kernel = out / 'kernel.elf'
    subprocess.run(['xorriso', '-no_rc', '-osirrox', 'on', '-indev', str(args.iso.resolve()),
        '-extract', '/boot/kernel.elf', str(kernel)], check=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    vm = server = thread = None

    def check(name, command, expected, user=True, timeout=240):
        print('START ' + name, flush=True)
        if len(command) > 230 or '\n' in command:
            script = out / ('command-' + str(len(report['checks'])) + '.sh')
            script.write_text('set -e\n' + command + '\n')
            stat = script.stat()
            server.repository['assets']['/distfiles/llvm-' + script.name] = dict(path=script,
                sha256=digest(script), identity=(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
            command = 'curl -fsS ' + origin + script.name + ' -o /tmp/llvm-command.sh && build-sh /tmp/llvm-command.sh'
        output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out / (str(len(report['checks'])) + '-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)

    try:
        server = RepositoryServer(('127.0.0.1', 0), args.repository)
        for name, path in assets.items():
            path = path.resolve(strict=True)
            stat = path.stat()
            server.repository['assets']['/distfiles/llvm-' + name] = dict(path=path,
                sha256=digest(path), identity=(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        origin = 'http://10.0.2.2:' + str(server.server_port) + '/distfiles/llvm-'
        vm = runtime.VM(4, storage=False, network=True, memory='24G', accel='kvm',
            iso=args.iso.resolve(), label='native-llvm')
        vm.command('true')
        check('select the signed dependency repository', 'brine server ' + origin.split('/distfiles/')[0],
            'Package downloads and source builds now use')
        check('refresh signed binary metadata', 'pacman -Syy --noconfirm', 'Synchronizing', user=False)
        check('refresh the matching signed file database', 'pacman -Fyy --noconfirm', 'Synchronizing', user=False)
        check('install the source UAPI dependency', 'brine install coast-linux-uapi --noconfirm', 'coast-linux-uapi')
        check('create an isolated normal-user LLVM workspace',
            'mkdir -p /home/user/llvm/fixture /home/user/llvm/sdk /home/user/llvm/cmake /home/user/llvm/source /home/user/llvm/bin && echo LLVM_WORKSPACE_READY', 'LLVM_WORKSPACE_READY')
        for asset, directory in [('llvm.tar.gz', 'fixture'), ('gcc.tar.gz', 'sdk'), ('cmake.tar.gz', 'cmake'), ('source.tar.gz', 'source')]:
            check('fetch and unpack ' + asset, 'curl -fsS ' + origin + asset + ' -o /tmp/llvm-fixture.tar.gz && '
                'tar -xzf /tmp/llvm-fixture.tar.gz -C /home/user/llvm/' + directory + ' && echo LLVM_FIXTURE_READY', 'LLVM_FIXTURE_READY', timeout=300)
        python = 'python3'
        if args.python_build:
            check('install the isolated source-built native Python fixture',
                'mkdir -p /home/user/llvm/python && curl -fsS ' + origin + 'python.tar.gz -o /tmp/llvm-python.tar.gz && '
                'tar -xzf /tmp/llvm-python.tar.gz -C /home/user/llvm/python && echo LLVM_PYTHON_READY', 'LLVM_PYTHON_READY', timeout=300)
            python = 'PYTHONHOME=/home/user/llvm/python/usr /home/user/llvm/python/usr/bin/python3.14'
            check('native Python preserves parent state across 128 threaded subprocesses',
                python + ' /home/user/llvm/source/ports/python/spawn_check.py', 'PYTHON_THREADED_SPAWN_PASS', timeout=600)
        command = python + ' /home/user/llvm/source/ports/llvm/check.py --runtime /home/user/llvm/fixture --sysroot /home/user/llvm/sdk'
        check('qualify real C/C++, threads, subprocesses, LLD, ThinLTO and archive workflows', command, 'LLVM_ALL_PASS', timeout=900)
        if args.native_rebuild:
            check('prepare the native compiler and Ninja',
                'curl -fsS ' + origin + 'compiler-wrapper -o /home/user/llvm/bin/gcc && chmod 755 /home/user/llvm/bin/gcc && '
                'cp /home/user/llvm/bin/gcc /home/user/llvm/bin/g++ && '
                'ln -s ../sdk/usr/bin/ar /home/user/llvm/bin/ar && ln -s ../sdk/usr/bin/ranlib /home/user/llvm/bin/ranlib && '
                'curl -fsS ' + origin + 'ninja -o /tmp/ninja && chmod 755 /tmp/ninja && /tmp/ninja --version', '1.13.2')
            check('fetch the exact original LLVM source archive',
                'mkdir -p /home/user/llvm/source/build/llvm/vendor && curl -fsS ' + origin + pin['archive'] +
                ' -o /home/user/llvm/source/build/llvm/vendor/' + pin['archive'] + ' && echo LLVM_SOURCE_READY', 'LLVM_SOURCE_READY', timeout=300)
            check('fetch the matching zlib source for compressed debug sections',
                'curl -fsS ' + origin + zlib['archive'] +
                ' -o /home/user/llvm/source/build/llvm/vendor/' + zlib['archive'] + ' && echo LLVM_ZLIB_SOURCE_READY',
                'LLVM_ZLIB_SOURCE_READY', timeout=180)
            check('rebuild Clang, LLD and LLVM tools entirely inside Coast',
                'cd /home/user/llvm/source && python3 -B tools/build_llvm.py --native --jobs 4 '
                '--native-toolchain /home/user/llvm/bin --cmake /home/user/llvm/cmake/usr/bin/cmake --ninja /tmp/ninja '
                '> /tmp/native-llvm.log 2>&1 && echo NATIVE_LLVM_BUILD_PASS', 'NATIVE_LLVM_BUILD_PASS', timeout=14400)
            check('qualify the newly native-built compiler, linker and tools',
                'cd /home/user/llvm/source && python3 ports/llvm/check.py --runtime build/llvm-native/runtime '
                '--sysroot /home/user/llvm/sdk', 'LLVM_ALL_PASS', timeout=900)
            check('archive the native LLVM runtime',
                'tar -czf /tmp/native-llvm-runtime.tar.gz -C /home/user/llvm/source/build/llvm-native/runtime usr && echo NATIVE_LLVM_ARCHIVED',
                'NATIVE_LLVM_ARCHIVED', timeout=300)
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('Coast remains healthy after LLVM compiler and linker execution')
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
                exports = {'/tmp/native-llvm.log': 'native-llvm.log',
                    '/tmp/native-llvm-runtime.tar.gz': 'native-llvm-runtime.tar.gz'}
                for name in ['BUILD.json', 'configure.log', 'build.log']:
                    exports['/home/user/llvm/source/build/llvm-native/' + name] = 'native-' + name
                address = ramfs.find('/tmp')['children']
                while address:
                    node = ramfs.node(address)
                    if node['name'].startswith('llvm-check-'):
                        for name in ['report.json', 'check.log']:
                            exports['/tmp/' + node['name'] + '/' + name] = node['name'] + '-' + name
                    address = node['next']
                for name, destination in exports.items():
                    try:
                        ramfs.export(name, out / destination)
                        report.setdefault('exports', {})[destination] = digest(out / destination)
                    except FileNotFoundError:
                        pass
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
    if report['state'] != 'passed':
        raise RuntimeError('LLVM qualification failed; see report.json')


if __name__ == '__main__':
    main()
