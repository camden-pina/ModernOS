#!/usr/bin/env python3
"""Check boot-file copy semantics and the exact bytes of a borrowed ELF."""
import argparse
import hashlib
import json
from pathlib import Path
from test_runtime import VM, ROOT, assert_success
from guest_ramfs import GuestRamFS, boot_kernel


def exercise(cpus, iso, memory, accel):
    iso = Path(iso).resolve()
    record_path = iso.parent / 'BUILD.json'
    if record_path.is_file():
        record = json.loads(record_path.read_text())
        expected = record.get('files', {}).get('bin/gui', {}).get('sha256') or record.get('programs', {}).get('gui')
    else:
        expected = None
    if not expected:
        with (ROOT / 'userland/bin/gui').open('rb') as source:
            expected = hashlib.file_digest(source, 'sha256').hexdigest()
    vm = VM(cpus, storage=False, iso=iso, network=False, label='boot-files', memory=memory, accel=accel)
    try:
        assert_success(vm.command('ramfs-check', 'RAMFS BOOT COPY CHECK PASS', timeout=40))
        assert_success(vm.command('check', timeout=40))
        vm.qmp('stop')

        try:
            fs = GuestRamFS(vm, boot_kernel(iso), ROOT / 'build/runtime-tests/boot-file-export')
            node = fs.find('/bin/gui')
            assert node['boot_data'] and not node['pages'], node
            exported = fs.export('/bin/gui', ROOT / 'build/runtime-tests/boot-file-export/gui.elf')
            assert exported == expected, (exported, expected)
        finally:
            vm.qmp('cont')

        print(
            f'PASS immutable boot backing, write/resize semantics and exact native desktop ELF ({cpus} CPUs)', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso', required=True)
    parser.add_argument('--memory', default='16G')
    parser.add_argument('--accel', choices=['tcg', 'kvm'], default='tcg')
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso, args.memory, args.accel)
