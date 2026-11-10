#!/usr/bin/env python3
"""Verify large native build mappings and files in an owned Coast VM."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import traceback

import test_runtime as runtime

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--initramfs', type=Path, required=True)
    parser.add_argument('--limine', type=Path, default=ROOT / 'build/limine-gnu/output/bin')
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--qemu-runtime', type=Path, help='native-built QEMU runtime to test with 2 GiB guest RAM')
    parser.add_argument('--repository', type=Path, default=ROOT / 'build/Coast-repository-server/repository.json')
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    fixture = out / 'large-build-check'
    subprocess.run([str(ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-gcc'), '-O2', '-g0',
        '-static', '-no-pie', '-fno-pie', '-mno-avx', '-mno-avx2', '-Wall', '-Wextra', '-Werror',
        '-Wl,-z,noexecstack', str(ROOT / 'ports/qemu/large-build-check.c'), '-o', str(fixture)], check=True)
    initramfs = out / 'initramfs.tar'
    with tarfile.open(args.initramfs) as original, tarfile.open(initramfs, 'w') as target:
        directories = set()
        def parents(name):
            for parent in reversed(PurePosixPath(name).parents):
                if str(parent) == '.' or str(parent) in directories:
                    continue
                member = tarfile.TarInfo(str(parent))
                member.type, member.mode = tarfile.DIRTYPE, 0o755
                target.addfile(member)
                directories.add(str(parent))
        for member in original:
            target.addfile(member, original.extractfile(member) if member.isfile() else None)
            if member.isdir():
                directories.add(member.name.rstrip('/'))
        member = tarfile.TarInfo('bin/large-build-check')
        member.mode, member.uid, member.gid, member.size = 0o755, 0, 0, fixture.stat().st_size
        with fixture.open('rb') as stream:
            target.addfile(member, stream)
        if args.qemu_runtime:
            repository = json.loads(args.repository.read_text())
            name = '/repo/x86_64/python-' + repository['packages']['python']['version'] + '-x86_64.pkg.tar.gz'
            record = repository['host_files'][name]
            python = args.repository.resolve().parent / record['path']
            if sha(python) != record['sha256']:
                raise ValueError('Python package checksum mismatch')
            with tarfile.open(python) as bundle:
                for member in bundle:
                    if not member.name.startswith('.'):
                        target.addfile(member, bundle.extractfile(member) if member.isfile() else None)
            for directory in ('usr/bin', 'usr/share/qemu'):
                for path in sorted((args.qemu_runtime / directory).rglob('*')):
                    if not path.is_file() or path.is_symlink():
                        continue
                    member = tarfile.TarInfo(str(path.relative_to(args.qemu_runtime)))
                    parents(member.name)
                    member.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
                    member.size = path.stat().st_size
                    with path.open('rb') as stream:
                        target.addfile(member, stream)
            path = ROOT / 'ports/qemu/large_ram_check.py'
            member = tarfile.TarInfo('usr/share/coast/qemu/large_ram_check.py')
            parents(member.name)
            member.mode, member.size = 0o644, path.stat().st_size
            with path.open('rb') as stream:
                target.addfile(member, stream)
    image = out / 'Coast-large.iso'
    subprocess.run(['python3', '-B', str(ROOT / 'tools/build_boot_iso.py'), '--bootloader', 'limine',
        '--config', str(ROOT / 'arch/x86/boot/boot.conf'), '--kernel', str(args.kernel.resolve()),
        '--initramfs', str(initramfs), '--source-root', str(ROOT), '--root', str(out / 'iso-root'),
        '--output', str(image), '--limine-dir', str(args.limine.resolve())], check=True)
    report = dict(state='failed', checks=[], kernel_sha256=sha(args.kernel),
        fixture_source_sha256=sha(ROOT / 'ports/qemu/large-build-check.c'),
        target_host_executed=False, extra_disks=False, network=False)
    vm = None
    try:
        vm = runtime.VM(4, storage=False, network=False, memory='8G', accel='kvm',
                        iso=image, label='large-native-builds')
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('ordinary userland ABI health before large operations')
        output = vm.shell_command('/bin/large-build-check', expect='LARGE_BUILD_ALL_PASS', as_user=True, timeout=240)
        (out / 'fixture-output.txt').write_text(output)
        report['checks'] += [line for line in output.splitlines()
                             if line.startswith(('LARGE_MAPPING_PASS', 'LARGE_FILE_PASS'))]
        if args.qemu_runtime:
            output = vm.shell_command('python3 /usr/share/coast/qemu/large_ram_check.py',
                expect='QEMU_LARGE_RAM_PASS', as_user=True, timeout=240)
            (out / 'qemu-output.txt').write_text(output)
            report['checks'].append('native-built QEMU allocates 2 GiB guest RAM and preserves data above 1 GiB')
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('ordinary userland ABI health after large operations and reclaim')
        report['state'] = 'passed'
        print('LARGE_NATIVE_BUILDS_PASS', flush=True)
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        raise
    finally:
        if vm:
            vm.close()
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
