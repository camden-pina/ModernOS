#!/usr/bin/env python3
"""Exercise real UEFI boots, SMP, graphics and reserved module backing.

Build each ISO with make iso BOOTLOADER=<name> ISO_IMAGE=build/<name>.iso,
then pass --limine/--grub/--refind. --bios also checks the Limine BIOS path.
"""
import argparse
import hashlib
import re
from pathlib import Path

from test_runtime import VM, ROOT, WORK, assert_success
from guest_ramfs import GuestRamFS, boot_kernel


def exercise(loader, iso, cpus, accel, bios=False, display=None):
    vm = VM(cpus, bios=bios, storage=False, iso=iso, network=False,
            label='boot-' + loader, accel=accel)
    try:
        online = vm.command('cpus')
        assert len(re.findall(r'cpu\d+ lapic=\d+ online=1', online)) == cpus, online
        assert_success(vm.command('check', 'USERLAND CHECK PASS', timeout=60))
        assert_success(vm.command('ramfs-check', 'RAMFS BOOT COPY CHECK PASS', timeout=40))
        assert_success(vm.command('thread-check', 'THREAD CHECK PASS', timeout=100))
        if display:
            assert_success(vm.command('gui --info', f'{display} XRGB8888'))
        vm.qmp('stop')

        try:
            directory = WORK / ('bootloader-export-' + loader + '-' + str(cpus))
            fs = GuestRamFS(vm, boot_kernel(iso), directory)
            node = fs.find('/bin/check')
            assert node['boot_data'] and not node['pages'], node
            digest = fs.export('/bin/check', directory / 'check.elf')

            # Compare the guest bytes against its own immutable boot image,
            # since host userland may have been rebuilt since ISO creation.
            import subprocess
            import tarfile

            archive = directory / 'initramfs.tar'
            subprocess.run(['xorriso', '-osirrox', 'on', '-indev', str(iso),
                            '-extract', '/boot/initramfs.tar', str(archive)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            with tarfile.open(archive) as tar:
                entry = next(item for item in tar if item.name.lstrip('./') == 'bin/check')
                with tar.extractfile(entry) as source:
                    expected = hashlib.file_digest(source, 'sha256').hexdigest()

            assert digest == expected, (digest, expected)
            archive.unlink()
        finally:
            vm.qmp('cont')

        log = (WORK / (vm.name + '.log')).read_text(errors='replace')
        assert 'unified handoff v1' in log, log

        print(f'PASS {loader} {"BIOS" if bios else "UEFI"}: {cpus} CPUs, '
              'graphics/syscalls, initramfs writes, threads, exact reserved boot bytes', flush=True)
    finally:
        vm.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for loader in ('limine', 'grub', 'refind'):
        parser.add_argument('--' + loader, type=Path)
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--accel', choices=['tcg', 'kvm'], default='tcg')
    parser.add_argument('--bios', action='store_true')
    parser.add_argument('--display', help='Also check gui --info for WIDTHxHEIGHT (full initramfs)')
    args = parser.parse_args()

    selected = [(name, getattr(args, name)) for name in ('limine', 'grub', 'refind')
                if getattr(args, name)]
    if not selected:
        parser.error('pass at least one --limine, --grub or --refind ISO')

    for loader, iso in selected:
        for cpus in args.cpus:
            exercise(loader, iso.resolve(), cpus, args.accel, display=args.display)
            if loader == 'limine' and args.bios:
                exercise(loader, iso.resolve(), cpus, args.accel, bios=True, display=args.display)


if __name__ == '__main__':
    main()
