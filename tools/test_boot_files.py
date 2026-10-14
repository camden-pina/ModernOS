#!/usr/bin/env python3
"""Check boot-file copy semantics and the exact bytes of a borrowed ELF."""
import argparse
import hashlib
from test_runtime import VM, ROOT, assert_success
from guest_ramfs import GuestRamFS, boot_kernel


def exercise(cpus, iso):
    vm = VM(cpus, storage=False, iso=iso, network=False, label='boot-files')
    try:
        assert_success(vm.command('ramfs-check', 'RAMFS BOOT COPY CHECK PASS', timeout=40))
        assert_success(vm.command('check', timeout=40))
        assert_success(vm.command('compiler-check', 'COMPILER ABI CHECK PASS', timeout=90))
        vm.qmp('stop')

        try:
            fs = GuestRamFS(vm, boot_kernel(iso), ROOT / 'build/runtime-tests/boot-file-export')
            node = fs.find('/bin/web-renderer')
            assert node['boot_data'] and not node['pages'], node
            exported = fs.export('/bin/web-renderer', ROOT / 'build/runtime-tests/boot-file-export/renderer.elf')
            with (ROOT / 'userland/bin/web-renderer').open('rb') as source:
                expected = hashlib.file_digest(source, 'sha256').hexdigest()
            assert exported == expected, (exported, expected)
        finally:
            vm.qmp('cont')

        print(
            f'PASS immutable boot backing, write/resize semantics, exact ELF, memory and child accounting ({cpus} CPUs)', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--iso', required=True)
    args = parser.parse_args()
    for cpus in args.cpus:
        exercise(cpus, args.iso)
