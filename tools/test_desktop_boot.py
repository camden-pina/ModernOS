#!/usr/bin/env python3
"""Boot a freshly assembled native/full desktop in owned diskless QEMU guests."""
import argparse
import fcntl
import json
from pathlib import Path
import re
import traceback

from source_tree import digest
import test_desktop as desktop
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--cpus', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--memory', default='16G')
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='kvm')
    args = parser.parse_args()
    if any(not 1 <= count <= 16 for count in args.cpus):
        parser.error('--cpus must be 1..16')
    iso = args.iso.resolve(strict=True)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'HARNESS.py').write_bytes(Path(__file__).read_bytes())
    report = dict(state='failed', iso=str(iso), iso_sha256=digest(iso),
        harness_sha256=digest(Path(__file__)), cases=[], network=False, extra_disks=False)
    with Path(str(iso) + '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        try:
            for bios in (False, True):
                for cpus in args.cpus:
                    name = ('bios' if bios else 'uefi') + '-' + str(cpus) + 'cpu'
                    directory = out / name
                    directory.mkdir()
                    runtime.WORK = desktop.WORK = directory
                    case = dict(firmware='BIOS' if bios else 'UEFI', cpus=cpus, checks=[])
                    report['cases'].append(case)
                    vm = None
                    try:
                        vm = runtime.VM(cpus, bios=bios, iso=iso, memory=args.memory,
                            accel=args.accel, storage=False, network=False, label='fresh-desktop-boot')
                        assert 'GUI_READY: Coast desktop' in Path(vm.log.name).read_text(errors='replace'), (
                            'Fresh Coast boot did not start the desktop')
                        case['checks'].append('desktop starts automatically on fresh boot')
                        online = vm.command('cpus')
                        assert len(re.findall(r'cpu\d+ lapic=\d+ online=1', online)) == cpus, online
                        case['checks'].append('all requested CPUs are online')
                        health = vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
                        (directory / 'health.txt').write_text(health)
                        case['checks'].append('native kernel/syscall/userland health checks')
                        information = vm.command('gui --info')
                        assert 'XRGB8888' in information, information
                        (directory / 'display.txt').write_text(information)
                        vm.send('gui --trace --stats\n')
                        vm.until(rb'GUI_READY:', 90)
                        display = desktop.DesktopTest(vm)
                        pixels = display.screenshot('desktop')
                        assert pixels and len(set(pixels)) > 32, 'Native desktop pixels missing'
                        window = display.launch('Files')
                        assert window > 1, window
                        display.screenshot('files')
                        case['checks'].append('native desktop and Files window respond to real USB input')
                        display.key('g', 'ctrl', 'alt')
                        vm.until_prompt()
                        assert '\n1000\n' in vm.raw_command('id -u')
                        display.key('g', 'ctrl', 'alt')
                        vm.until(rb'GUI_READY: Coast desktop', 40)
                        display.screenshot('desktop-return')
                        case['checks'].append('Ctrl+Alt+G switches from desktop to owner console and back')
                        display.exit()
                        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
                        case['checks'].append('desktop exits and the shell remains healthy')
                        case['state'] = 'passed'
                        print('PASS ' + name + ': kernel, shell, desktop, USB and clean exit', flush=True)
                    finally:
                        if vm:
                            vm.close()
                        case['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
            if digest(iso) != report['iso_sha256']:
                raise ValueError('The boot-tested image changed')
            report['image_unchanged'] = True
            report['state'] = 'passed'
        except BaseException as error:
            report['error'] = repr(error)
            (out / 'exception.txt').write_text(traceback.format_exc())
            raise
        finally:
            (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
