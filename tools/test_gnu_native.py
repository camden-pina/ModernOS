#!/usr/bin/env python3
"""Rebuild GNU libgcc and libstdc++ inside Coast with its rebuilt GCC.

The source-built glibc SDK is the bootstrap input. This test does not claim a
native glibc rebuild: it qualifies native GNU C++ libraries and their real
exception/thread consumer against the image's matching GNU C runtime.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tarfile
import threading
import traceback

from fetch_sources import read_manifest
from guest_ramfs import GuestRamFS
from serve_repository import RepositoryServer
from source_tree import digest
from test_llvm import fixture
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--glibc-build', type=Path, required=True)
    parser.add_argument('--toolchain-archive', type=Path, required=True)
    parser.add_argument('--toolchain-report', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    previous = json.loads(args.toolchain_report.read_text())
    stage = 'rebuild binutils, GCC, libgcc and POSIX libstdc++ from original sources inside Coast'
    if (not previous.get('native_rebuild') or previous.get('target_execution_on_linux') is not False or
            stage not in previous.get('checks', []) or
            previous.get('exports', {}).get('native-toolchain.tar.gz') != digest(args.toolchain_archive)):
        parser.error('The compiler archive must be proven by its native source rebuild report')
    for name in ['userland/sdk/bootstrap-toolchain.sh', 'userland/sdk/prepare-gcc-options.sh']:
        if previous['source_files'].get(name) != digest(runtime.ROOT / name):
            parser.error('The native compiler source changed: ' + name)
    manifest = json.loads(args.iso.resolve().with_name('MANIFEST.json').read_text())
    if manifest.get('glibc_build_sha256') != digest(args.glibc_build):
        parser.error('Select an image with the matching source-built GNU C runtime')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    glibc_archive = out / 'glibc-sdk.tar.gz'
    fixture(args.glibc_build, glibc_archive)
    inputs = ['LICENSE', 'tools/build_gnu_libgcc.py', 'tools/build_gnu_cxx.py', 'tools/fetch_sources.py',
        'tools/source_archive.py', 'tools/source_tree.py', 'userland/vendor/sources.json',
        'ports/gnu-runtime/cxx_check.cc', 'ports/build-tools/thread_check.cc']
    kit = out / 'source.tar.gz'
    with tarfile.open(kit, 'w:gz') as bundle:
        for name in inputs:
            bundle.add(runtime.ROOT / name, arcname=name, recursive=False)
    assets = {'source.tar.gz': kit, 'glibc-sdk.tar.gz': glibc_archive,
        'glibc-BUILD.json': args.glibc_build.resolve(strict=True),
        'native-compiler.tar.gz': args.toolchain_archive.resolve(strict=True)}
    for name in ['gcc-14.2.0.tar.gz', 'gmp-6.2.1.tar.xz', 'mpfr-4.1.0.tar.xz', 'mpc-1.2.1.tar.gz']:
        path = runtime.ROOT / 'userland/vendor' / name
        if digest(path) != read_manifest()[name]['sha256']:
            raise ValueError('GCC prerequisite source archive changed: ' + name)
        assets[name] = path
    report = dict(state='failed', checks=[], native_glibc_rebuilt=False, native_compiler_report_sha256=digest(args.toolchain_report),
        native_compiler_archive_sha256=digest(args.toolchain_archive), glibc_build_sha256=digest(args.glibc_build),
        iso_sha256=digest(args.iso), repository_manifest_sha256=digest(args.repository),
        source_files={p: digest(runtime.ROOT / p) for p in inputs}, harness_sha256=digest(Path(__file__)),
        target_execution_on_linux=False)
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
            add_asset(script.name, script)
            command = 'curl -fsS ' + origin + script.name + ' -o /tmp/gnu-native-command.sh && build-sh /tmp/gnu-native-command.sh'
        output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out / (str(len(report['checks'])) + '-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)

    def add_asset(name, path):
        path = path.resolve(strict=True)
        stat = path.stat()
        server.repository['assets']['/distfiles/gnu-native-' + name] = dict(path=path, sha256=digest(path),
            identity=(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))

    try:
        server = RepositoryServer(('127.0.0.1', 0), args.repository)
        for name, path in assets.items():
            add_asset(name, path)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        origin = 'http://10.0.2.2:' + str(server.server_port) + '/distfiles/gnu-native-'
        vm = runtime.VM(4, storage=False, network=True, memory='12G', accel='kvm',
            iso=args.iso.resolve(), label='native-gnu-cxx')
        vm.command('true')
        check('select the signed native dependency repository', 'coast server ' + origin.split('/distfiles/')[0],
            'Package downloads and source builds now use')
        check('refresh signed binary metadata', 'pacman -Syy --noconfirm', 'Synchronizing', user=False)
        check('refresh the matching signed file database', 'pacman -Fyy --noconfirm', 'Synchronizing', user=False)
        check('install native GNU runtime source build dependencies',
            'coast install coast-build --noconfirm', 'installing coast-build', timeout=600)
        base = '/home/user/gnu-native'
        check('create an isolated normal-user GNU source workspace',
            'mkdir -p ' + base + '/source/build/glibc/runtime /home/user/Builds/toolchain/install && echo GNU_WORKSPACE_READY',
            'GNU_WORKSPACE_READY')
        for asset, directory in [('source.tar.gz', base + '/source'), ('glibc-sdk.tar.gz', base + '/source/build/glibc/runtime'),
                                  ('native-compiler.tar.gz', '/home/user/Builds/toolchain/install')]:
            check('fetch and unpack ' + asset, 'curl -fsS ' + origin + asset + ' -o /tmp/gnu-input.tar.gz && '
                'tar -xzf /tmp/gnu-input.tar.gz -C ' + directory + ' && echo GNU_INPUT_READY', 'GNU_INPUT_READY', timeout=300)
        check('install the exact source-built GNU C SDK receipt', 'curl -fsS ' + origin +
            'glibc-BUILD.json -o ' + base + '/source/build/glibc/BUILD.json && echo GNU_C_SDK_READY', 'GNU_C_SDK_READY')
        check('verify the restored native compiler before building GNU libraries',
            '/home/user/Builds/toolchain/install/bin/g++ -std=c++17 -pthread ' + base +
            '/source/ports/build-tools/thread_check.cc -o /tmp/gnu-bootstrap-check && /tmp/gnu-bootstrap-check',
            'CXX_THREADS_PASS', timeout=240)
        path = 'export PATH=/home/user/Builds/toolchain/install/bin:/usr/bin:/bin; cd ' + base + '/source; '
        compiler = '/home/user/Builds/toolchain/install/bin/'
        check('rebuild the GNU unwinder and GCC support libraries entirely inside Coast', path +
            'python3 -B tools/build_gnu_libgcc.py --native --glibc-build build/glibc/BUILD.json '
            '--source-root build/gnu-sdk-src --cc ' + compiler + 'gcc --cxx ' + compiler +
            'g++ --jobs 4 > /tmp/native-gnu-libgcc.log 2>&1 && echo NATIVE_GNU_LIBGCC_PASS', 'NATIVE_GNU_LIBGCC_PASS', timeout=3600)
        check('rebuild GNU libstdc++ with the native compiler and newly built GNU unwinder', path +
            'python3 -B tools/build_gnu_cxx.py --native --compiler-family gcc --glibc-build build/glibc/BUILD.json '
            '--libgcc-build build/gnu-libgcc-native/BUILD.json --source-root build/gnu-sdk-src --cc ' + compiler +
            'gcc --cxx ' + compiler + 'g++ --jobs 4 > /tmp/native-gnu-cxx.log 2>&1 && echo NATIVE_GNU_CXX_PASS',
            'NATIVE_GNU_CXX_PASS', timeout=3600)
        libc = base + '/source/build/glibc/runtime/usr/lib/x86_64-linux-gnu'
        cxx = base + '/source/build/gnu-cxx-native/runtime'
        unwinder = base + '/source/build/gnu-libgcc-native/runtime'
        headers = cxx + '/usr/include/c++/14.2.0'
        support = unwinder + '/usr/lib/gcc/x86_64-coast-linux-gnu/14.2.0'
        check('compile a genuine GNU C++ exception and thread consumer with native GCC', path + compiler +
            'g++ -specs=build/gnu-cxx-native/gnu-driver.specs -std=c++17 -O2 -g0 -fno-pie -no-pie -mno-avx -mno-avx2 '
            '--sysroot=' + base + '/source/build/glibc/runtime -B' + support + '/ -B' + libc + '/ '
            '-nostdinc++ -isystem ' + headers + ' -isystem ' + headers + '/x86_64-coast-linux-gnu -isystem ' + headers + '/backward '
            '-idirafter /usr/lib/coast/linux-uapi/include -L' + cxx + '/usr/lib/x86_64-linux-gnu -L' + unwinder + '/usr/lib/x86_64-linux-gnu '
            '-L' + support + ' -L' + libc + ' -Wl,-rpath-link,' + libc +
            ' -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 -Wl,-rpath,' + cxx + '/usr/lib/x86_64-linux-gnu:' +
            unwinder + '/usr/lib/x86_64-linux-gnu -Wl,-z,noexecstack ports/gnu-runtime/cxx_check.cc '
            '-pthread -ldl -o /tmp/native-gnu-check && echo GNU_CONSUMER_COMPILED', 'GNU_CONSUMER_COMPILED', timeout=300)
        check('execute threads and exceptions with both native-built GNU libraries selected',
            'COAST_CXX_LIBRARY_PREFIX=' + cxx + '/usr/lib/x86_64-linux-gnu COAST_UNWINDER_LIBRARY_PREFIX=' +
            unwinder + '/usr/lib/x86_64-linux-gnu /tmp/native-gnu-check', 'CXX_THREADS_PASS', timeout=180)
        output = (out / (str(len(report['checks']) - 1) + '-output.txt')).read_text()
        for expected in ['GNU_CXX_LOADED ' + cxx, 'GNU_UNWINDER_LOADED ' + unwinder]:
            if expected not in output:
                raise ValueError('The native GNU consumer loaded a bootstrap runtime: ' + expected)
        check('archive native GNU runtime libraries and headers',
            'tar -czf /tmp/native-gnu-libgcc-runtime.tar.gz -C ' + unwinder + ' usr && '
            'tar -czf /tmp/native-gnu-cxx-runtime.tar.gz -C ' + cxx + ' usr && echo NATIVE_GNU_ARCHIVED',
            'NATIVE_GNU_ARCHIVED', timeout=300)
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('Coast remains healthy after native GNU source rebuilding and runtime execution')
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
                exports = {}
                for tool in ['libgcc', 'cxx']:
                    exports['/tmp/native-gnu-' + tool + '.log'] = 'native-gnu-' + tool + '.log'
                    exports['/tmp/native-gnu-' + tool + '-runtime.tar.gz'] = 'native-gnu-' + tool + '-runtime.tar.gz'
                    for name in ['BUILD.json', 'configure.log', 'configure-gcc.log', 'host-generators.log', 'target-headers.log',
                                 'library-configure.log', 'library-build.log', 'build.log', 'install.log']:
                        exports['/home/user/gnu-native/source/build/gnu-' + tool + '-native/' + name] = 'native-gnu-' + tool + '-' + name
                for source_path, name in exports.items():
                    try:
                        ramfs.export(source_path, out / name)
                        report.setdefault('exports', {})[name] = digest(out / name)
                    except FileNotFoundError:
                        pass
            except BaseException as error:
                report['export_error'] = repr(error)
                report['state'] = 'failed'
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
        raise SystemExit('Native GNU library qualification failed; inspect report.json')
    print('Native GNU C++ libraries rebuilt and qualified inside Coast', flush=True)


if __name__ == '__main__':
    main()
