#!/usr/bin/env python3
"""Run a real GNU C++ thread/exception consumer against the source-built DSO."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import threading
import traceback

from serve_repository import RepositoryServer
from source_tree import digest
from test_llvm import fixture
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--glibc-build', type=Path, required=True)
    parser.add_argument('--libgcc-build', type=Path)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    record = json.loads(args.build.read_text())
    if record['glibc_build_sha256'] != digest(args.glibc_build):
        parser.error('The GNU C++ and glibc SDK inputs must match')
    manifest = json.loads(args.iso.resolve().with_name('MANIFEST.json').read_text())
    if manifest['glibc_build_sha256'] != digest(args.glibc_build):
        parser.error('Use an image containing the matching source-built GNU runtime')
    archive = out / 'runtime.tar.gz'
    fixture(args.build, archive)
    libgcc, unwinder_archive = None, None
    if args.libgcc_build:
        libgcc = json.loads(args.libgcc_build.read_text())
        if (libgcc['glibc_build_sha256'] != digest(args.glibc_build) or
                record.get('libgcc_build_sha256') != digest(args.libgcc_build)):
            parser.error('Use the source-built unwinder selected by this GNU C++ build')
        unwinder_archive = out / 'unwinder.tar.gz'
        fixture(args.libgcc_build, unwinder_archive)
    sysroot = args.glibc_build.resolve().parent / 'runtime'
    libraries = sysroot / 'usr/lib/x86_64-linux-gnu'
    cxx = args.build.resolve().parent / 'runtime'
    headers = cxx / 'usr/include/c++/14.2.0'
    source = runtime.ROOT / 'ports/gnu-runtime/cxx_check.cc'
    consumer = out / 'gnu-cxx-check'
    private = '/home/user/gnu-cxx/runtime/usr/lib/x86_64-linux-gnu'
    command = ['g++', '-std=c++17', '-O2', '-g0', '-fno-pie', '-no-pie', '-mno-avx', '-mno-avx2',
        '--sysroot=' + str(sysroot), '-B' + str(libraries) + '/', '-nostdinc++',
        '-isystem', str(headers), '-isystem', str(headers / 'x86_64-coast-linux-gnu'),
        '-isystem', str(headers / 'backward'),
        '-idirafter', str(runtime.ROOT / 'ports/linux-uapi/include'),
        '-L' + str(cxx / 'usr/lib/x86_64-linux-gnu'),
        '-L' + str(libraries), '-Wl,-rpath-link,' + str(libraries),
        '-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2', '-Wl,-rpath,' + private,
        '-Wl,-z,noexecstack', str(source), '-pthread', '-ldl', '-o', str(consumer)]
    if libgcc:
        support = args.libgcc_build.resolve().parent / 'runtime'
        command[1:1] = ['-B' + str(support / 'usr/lib/gcc/x86_64-coast-linux-gnu/14.2.0') + '/',
            '-L' + str(support / 'usr/lib/x86_64-linux-gnu'),
            '-L' + str(support / 'usr/lib/gcc/x86_64-coast-linux-gnu/14.2.0')]
    subprocess.run(command, check=True)
    versions = subprocess.check_output(['readelf', '--version-info', str(consumer)], text=True)
    if max(tuple(map(int, x.split('.'))) for x in re.findall(r'GLIBC_([0-9.]+)', versions)) > (2, 36):
        raise ValueError('GNU C++ consumer requires the host glibc')
    report = dict(state='failed', checks=[], source_build_sha256=digest(args.build),
        glibc_build_sha256=digest(args.glibc_build), fixture_sha256=digest(archive),
        consumer_sha256=digest(consumer), compile_command=command, iso_sha256=digest(args.iso),
        source_files={str(p.relative_to(runtime.ROOT)): digest(p) for p in
            [source, runtime.ROOT / 'ports/build-tools/thread_check.cc', Path(__file__)]},
        target_execution_on_linux=False)
    if libgcc:
        report.update(libgcc_build_sha256=digest(args.libgcc_build), unwinder_fixture_sha256=digest(unwinder_archive))
    vm = server = thread = None
    try:
        server = RepositoryServer(('127.0.0.1', 0), args.repository)
        assets = [('runtime.tar.gz', archive), ('consumer', consumer)]
        if unwinder_archive:
            assets.append(('unwinder.tar.gz', unwinder_archive))
        for name, path in assets:
            stat = path.stat()
            server.repository['assets']['/distfiles/gnu-cxx-' + name] = dict(path=path,
                sha256=digest(path), identity=(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        origin = 'http://10.0.2.2:' + str(server.server_port)
        vm = runtime.VM(4, storage=False, network=True, memory='8G', accel='kvm',
            iso=args.iso.resolve(), label='gnu-cxx')
        vm.command('true')
        vm.shell_command('brine server ' + origin, expect='Package downloads and source builds now use', as_user=True)
        for command in ['mkdir -p /home/user/gnu-cxx/runtime',
            'curl -fsS ' + origin + '/distfiles/gnu-cxx-runtime.tar.gz -o /tmp/gnu-cxx.tar.gz',
            'tar -xzf /tmp/gnu-cxx.tar.gz -C /home/user/gnu-cxx/runtime',
            'curl -fsS ' + origin + '/distfiles/gnu-cxx-consumer -o /tmp/gnu-cxx-check && chmod 755 /tmp/gnu-cxx-check']:
            vm.shell_command(command, as_user=True, timeout=180)
        if libgcc:
            vm.shell_command('curl -fsS ' + origin + '/distfiles/gnu-cxx-unwinder.tar.gz -o /tmp/gnu-unwinder.tar.gz && '
                'tar -xzf /tmp/gnu-unwinder.tar.gz -C /home/user/gnu-cxx/runtime', as_user=True, timeout=180)
            vm.shell_command('sha256sum ' + private + '/libgcc_s.so.1',
                expect=libgcc['files']['usr/lib/x86_64-linux-gnu/libgcc_s.so.1']['sha256'], as_user=True)
            report['checks'].append('guest GNU unwinder bytes match the source-built library')
        library = next(n for n in record['files'] if n.startswith('usr/lib/') and '/libstdc++.so.6.0.' in n)
        vm.shell_command('sha256sum /home/user/gnu-cxx/runtime/' + library,
            expect=record['files'][library]['sha256'], as_user=True)
        report['checks'].append('guest GNU C++ DSO bytes match the source-built library')
        output = vm.shell_command('COAST_CXX_LIBRARY_PREFIX=' + private +
            (' COAST_UNWINDER_LIBRARY_PREFIX=' + private if libgcc else '') + ' /tmp/gnu-cxx-check',
            expect='CXX_THREADS_PASS', as_user=True, timeout=180)
        if 'GNU_CXX_LOADED ' + private not in output:
            raise ValueError('The source-built GNU C++ DSO was not selected')
        if libgcc and 'GNU_UNWINDER_LOADED ' + private not in output:
            raise ValueError('The source-built GNU unwinder DSO was not selected')
        (out / 'gnu-cxx-check.txt').write_text(output)
        report['checks'].extend(['GNU dynamic loader selects the isolated source-built C++ DSO',
            'GNU C++ threads, conditions, mutexes, futures, TLS destructors and exceptions execute correctly'])
        vm.command('check', expect='USERLAND CHECK PASS')
        report['checks'].append('Coast remains healthy after GNU C++ runtime execution')
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        raise
    finally:
        if vm:
            vm.close()
        if server:
            server.shutdown()
            server.server_close()
        if thread:
            thread.join(5)
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('GNU C++ runtime checks passed inside Coast', flush=True)


if __name__ == '__main__':
    main()
