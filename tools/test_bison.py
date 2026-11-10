#!/usr/bin/env python3
"""Qualify the cross-built and source-rebuilt Bison in a disposable Coast VM."""
import argparse
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
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    record = json.loads(args.build.read_text())
    if record['target_host_executed'] is not False or record['build_mode'] != 'linux-cross':
        parser.error('A completed static cross-build is required')
    for name, expected in record['source_sha256'].items():
        if digest(runtime.ROOT / name) != expected:
            raise ValueError('Build source changed: ' + name)
    fixture = out / 'fixture.tar.gz'
    with tarfile.open(fixture, 'w:gz') as bundle:
        directories = set()
        for name, asset in record['files'].items():
            source = Path(asset['path'])
            if digest(source) != asset['sha256']:
                raise ValueError('Build product changed: ' + name)
            for parent in reversed(Path(name).parents):
                if str(parent) != '.' and str(parent) not in directories:
                    info = tarfile.TarInfo(str(parent))
                    info.type, info.mode = tarfile.DIRTYPE, 0o755
                    bundle.addfile(info)
                    directories.add(str(parent))
            info = tarfile.TarInfo(name)
            info.size, info.mode = source.stat().st_size, asset['mode']
            with source.open('rb') as stream:
                bundle.addfile(info, stream)
    kit = out / 'source.tar.gz'
    inputs = ['LICENSE', 'ports/gnu-runtime/SOURCES.json', 'ports/gnu-runtime/bison_check.py',
        'tools/build_bison.py', 'tools/fetch_sources.py', 'tools/source_archive.py', 'tools/source_tree.py']
    with tarfile.open(kit, 'w:gz') as bundle:
        for name in inputs:
            bundle.add(runtime.ROOT / name, arcname=name, recursive=False)
    vendor = args.build.resolve().parent / 'vendor' / record['source']['archive']
    if digest(vendor) != record['source']['sha256']:
        raise ValueError('Upstream Bison source changed')
    report = dict(state='failed', checks=[], build_host='Coast', guest_execution_on_linux=False,
        iso_sha256=digest(args.iso), fixture_sha256=digest(fixture), source_kit_sha256=digest(kit),
        source_files={name: digest(runtime.ROOT / name) for name in inputs},
        seed_build_sha256=digest(args.build), harness_sha256=digest(Path(__file__)))
    kernel = out / 'kernel.elf'
    subprocess.run(['xorriso', '-osirrox', 'on', '-indev', str(args.iso.resolve()),
        '-extract', '/boot/kernel.elf', str(kernel)], check=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    vm = server = thread = None

    def check(name, command, expected, timeout=240):
        print('START ' + name, flush=True)
        output = vm.shell_command(command, expect=expected, as_user=True, timeout=timeout)
        (out / (str(len(report['checks'])) + '-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)

    try:
        server = RepositoryServer(('127.0.0.1', 0), args.repository)
        for name, path in [('fixture.tar.gz', fixture), ('source.tar.gz', kit), (vendor.name, vendor)]:
            path = path.resolve(strict=True)
            stat = path.stat()
            server.repository['assets']['/distfiles/bison-' + name] = dict(path=path,
                sha256=digest(path), identity=(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        origin = 'http://10.0.2.2:' + str(server.server_port)
        vm = runtime.VM(4, storage=False, network=True, memory='8G', accel='kvm',
            iso=args.iso.resolve(), label='native-bison')
        vm.command('true')
        check('select the signed source fixture repository', 'brine server ' + origin,
            'Package downloads and source builds now use')
        check('create an isolated normal-user source workspace',
            'mkdir -p /home/user/bison/source/build/bison-native/vendor /home/user/bison/fixture && echo BISON_WORKSPACE_READY', 'BISON_WORKSPACE_READY')
        for name, directory in [('fixture.tar.gz', 'fixture'), ('source.tar.gz', 'source')]:
            check('unpack ' + name, 'curl -fsS ' + origin + '/distfiles/bison-' + name +
                ' -o /tmp/bison.tar.gz && tar -xzf /tmp/bison.tar.gz -C /home/user/bison/' + directory + ' && echo BISON_UNPACKED', 'BISON_UNPACKED')
        check('qualify real parser generation, compilation, execution and error handling',
            'python3 /home/user/bison/source/ports/gnu-runtime/bison_check.py --runtime /home/user/bison/fixture', 'BISON_ALL_PASS')
        check('fetch the original checksum-pinned source archive', 'curl -fsS ' + origin + '/distfiles/bison-' + vendor.name +
            ' -o /home/user/bison/source/build/bison-native/vendor/' + vendor.name + ' && echo BISON_SOURCE_READY', 'BISON_SOURCE_READY')
        check('rebuild GNU Bison entirely inside Coast', 'cd /home/user/bison/source && '
            'python3 -B tools/build_bison.py --native --jobs 4 > /tmp/native-bison.log 2>&1 && echo NATIVE_BISON_BUILD_PASS', 'NATIVE_BISON_BUILD_PASS', timeout=1800)
        check('qualify the native-built parser generator',
            'python3 /home/user/bison/source/ports/gnu-runtime/bison_check.py --runtime /home/user/bison/source/build/bison-native/runtime', 'BISON_ALL_PASS')
        check('archive the genuine native runtime',
            'tar -czf /tmp/native-bison-runtime.tar.gz -C /home/user/bison/source/build/bison-native/runtime usr && echo NATIVE_BISON_ARCHIVED', 'NATIVE_BISON_ARCHIVED')
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
                exports = {'/tmp/native-bison.log': 'native-bison.log',
                    '/tmp/native-bison-runtime.tar.gz': 'native-bison-runtime.tar.gz'}
                for name in ['BUILD.json', 'configure.log', 'build.log', 'install.log']:
                    exports['/home/user/bison/source/build/bison-native/' + name] = 'native-' + name
                address = ramfs.find('/tmp')['children']
                while address:
                    node = ramfs.node(address)
                    if node['name'].startswith('bison-check-'):
                        for name in ['report.json', 'check.log']:
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
        raise RuntimeError('Native Bison qualification failed; see report.json')


if __name__ == '__main__':
    main()
