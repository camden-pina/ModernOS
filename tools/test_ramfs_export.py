#!/usr/bin/env python3
"""Prove read-only export against real guest data and its guest SHA-256."""
import hashlib
import shlex

from guest_ramfs import GuestRamFS, boot_kernel
from test_runtime import VM, WORK, assert_success, default_iso


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    elf = boot_kernel(default_iso())
    vm = VM(4, storage=False, network=False, label='ramfs-export')
    expected = hashlib.sha256(b'BEGIN\n' + bytes(3 * 1024 * 1024 + 3) + b'END\n').hexdigest()
    try:
        script = ('mkdir -p /tmp/export/child; '
                  '(printf "BEGIN\\n"; head -c 3145731 /dev/zero; printf "END\\n") > /tmp/export/child/binary; '
                  'touch /tmp/export/child/empty; sha256sum /tmp/export/child/binary')
        output = vm.command('build-sh -c ' + shlex.quote(script), expected)
        assert_success(output)
        vm.qmp('stop')

        try:
            fs = GuestRamFS(vm, elf, WORK / 'ramfs-export')
            actual = fs.export('/tmp/export/child/binary', WORK / 'ramfs-export.bin')
            assert actual == expected, (actual, expected)
            assert fs.export('/tmp/export/child/empty', WORK / 'ramfs-export-empty.bin') == hashlib.sha256(b'').hexdigest()

            try:
                fs.find('/tmp/export/child/missing')
            except FileNotFoundError:
                pass
            else:
                raise AssertionError('Missing file was reported as present')
        finally:
            vm.qmp('cont')

        vm.command('build-sh -c ' + shlex.quote('sha256sum /tmp/export/child/binary'), expected)

        print('PASS RAMFS export: exact multi-page bytes/guest digest, empty/missing files and unchanged resumed guest', flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    main()
