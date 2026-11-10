#!/usr/bin/env python3
"""Qualify source-built GNU ABI libraries and real threads in a Coast ISO."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import threading
import traceback

from serve_repository import RepositoryServer
from source_tree import digest
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--glibc-build', type=Path, required=True)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    image = args.iso.resolve(strict=True)
    record = json.loads(args.glibc_build.read_text())
    manifest = image.parent / 'MANIFEST.json'
    overlay = json.loads(manifest.read_text())
    if (overlay['glibc_build_sha256'] != digest(args.glibc_build) or
            overlay['iso_sha256'] != digest(image) or record['target_host_executed'] is not False):
        parser.error('Use the unchanged source-built GNU runtime qualification image')
    for name, expected in record['source_sha256'].items():
        if digest(runtime.ROOT / name) != expected:
            raise ValueError('GNU runtime source changed: ' + name)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    source = runtime.ROOT / 'ports/gnu-runtime/glibc_check.c'
    fixture = out / 'glibc-check'
    sysroot = args.glibc_build.resolve().parent / 'runtime'
    libraries = sysroot / 'usr/lib/x86_64-linux-gnu'
    command = ['gcc', '--sysroot=' + str(sysroot), '-B' + str(libraries) + '/',
        '-idirafter', str(runtime.ROOT / 'ports/linux-uapi/include'),
        '-O2', '-g0', '-fno-pie', '-no-pie', '-mno-avx', '-mno-avx2',
        '-Wl,-z,noexecstack', '-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2',
        '-L' + str(libraries), str(source), '-pthread', '-ldl', '-lm', '-o', str(fixture)]
    subprocess.run(command, check=True)
    versions = subprocess.check_output(['readelf', '--version-info', str(fixture)], text=True)
    if max(tuple(map(int, n.split('.'))) for n in re.findall(r'GLIBC_([0-9.]+)', versions)) > (2, 36):
        raise ValueError('GNU fixture accidentally requires the host GNU runtime')
    report = dict(state='failed', checks=[], iso_sha256=digest(image),
        glibc_build_sha256=digest(args.glibc_build), manifest_sha256=digest(manifest),
        fixture_sha256=digest(fixture), compile_command=command,
        source_sha256={str(source.relative_to(runtime.ROOT)): digest(source),
                       str(Path(__file__).relative_to(runtime.ROOT)): digest(Path(__file__))},
        target_execution_on_linux=False)
    server = thread = vm = None
    try:
        server = RepositoryServer(('127.0.0.1', 0), args.repository)
        stat = fixture.stat()
        server.repository['assets']['/distfiles/glibc-check'] = dict(path=fixture,
            sha256=digest(fixture), identity=(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        origin = 'http://10.0.2.2:' + str(server.server_port)
        vm = runtime.VM(4, storage=False, network=True, memory='8G', accel='kvm',
            iso=image, label='source-glibc')
        vm.command('true')
        vm.shell_command('brine server ' + origin,
            expect='Package downloads and source builds now use', as_user=True, timeout=180)
        report['checks'].append('fixture downloads use the selected repository network policy')
        for name in ['usr/lib/x86_64-linux-gnu/libc.so.6', 'lib64/ld-linux-x86-64.so.2']:
            output = vm.shell_command('sha256sum /' + name, expect=overlay['files'][name], as_user=True)
            (out / (Path(name).name + '-sha256.txt')).write_text(output)
        report['checks'].append('running guest loader and libc match the source build exactly')
        vm.shell_command('curl -fsS ' + origin + '/distfiles/glibc-check -o /tmp/glibc-check && chmod 755 /tmp/glibc-check',
            as_user=True, timeout=180)
        output = vm.shell_command('/tmp/glibc-check', expect='GLIBC_ALL_PASS', as_user=True, timeout=240)
        (out / 'glibc-check.txt').write_text(output)
        for expected in ['GLIBC_THREADS_PASS', 'GLIBC_MEMORY_PASS', 'GLIBC_LOADER_NSS_SIGNALS_PASS']:
            if expected not in output:
                raise ValueError('Incomplete GNU runtime check: ' + expected)
        report['checks'].extend(['four GNU pthread workers spawn and reap 24 real processes with exact parent identity and TLS',
            'GNU libc allocates and protects real memory and preserves zero-filled allocation bytes',
            'GNU loader resolves libm symbols; NSS resolves localhost and signal handlers execute'])
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('native Coast ABI remains healthy after GNU runtime validation')
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
    print('GNU runtime checks passed inside Coast', flush=True)


if __name__ == '__main__':
    main()
