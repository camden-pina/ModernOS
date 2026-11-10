#!/usr/bin/env python3
"""Build QEMU and Coast inside Coast, then boot that ISO with that QEMU."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import threading
import traceback

from guest_ramfs import GuestRamFS
from serve_repository import RepositoryServer
import test_runtime as runtime


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=runtime.ROOT / 'build/Coast-browser.iso')
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error('--jobs must be 1..8')
    image = args.iso.resolve(strict=True)
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    report = dict(state='failed', checks=[], iso_sha256=sha(image),
        repository_manifest_sha256=sha(args.repository), guest_execution_on_linux=False,
        build_host='Coast', emulator_backend='TCI', extra_disks=False)
    kernel = out / 'boot-kernel.elf'
    subprocess.run(['xorriso', '-osirrox', 'on', '-indev', str(image),
        '-extract', '/boot/kernel.elf', str(kernel)], check=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    vm = server = thread = None

    def check(name, command, expected, user=True, timeout=180):
        print('START ' + name, flush=True)
        # Package operations may invoke sudo internally. Authenticate a fixed
        # command first, never send a password in response to package output.
        if command.startswith(('brine server ', 'brine install ')):
            vm.command('true')
        output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out / (f'{len(report["checks"]):02d}-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)
        return output

    try:
        server = RepositoryServer(('127.0.0.1', 0), args.repository)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        vm = runtime.VM(4, storage=False, network=True, memory='20G', accel='kvm',
                        iso=image, label='native-qemu-build')
        vm.command('true')
        check('select candidate with existing repository signing key',
              'brine server http://10.0.2.2:' + str(server.server_port), 'Package downloads and source builds now use')
        check('verify signed repository metadata', 'pacman -Syy --noconfirm', 'Synchronizing', user=False)
        check('refresh the matching signed file database', 'pacman -Fyy --noconfirm', 'Synchronizing', user=False)
        check('install complete native QEMU build closure', 'brine install qemu-build --noconfirm',
              'installing qemu-build', timeout=600)
        check('verify Python build-tool package ownership', 'pacman -Qkk python-pip python-setuptools qemu-build', '0 altered files')
        check('native Python runs the packaged build frontend', 'python3 -m pip --version', 'pip 25.2')
        check('unpack the signed native build kit as the desktop user',
              'mkdir -p /home/user/Builds/qemu && cd /home/user/Builds/qemu && '
              'tar -xzf /usr/src/coast/qemu-build/coast-qemu-10.2.4.tar.gz --strip-components=1 && echo QEMU_KIT_READY', 'QEMU_KIT_READY')
        check('compile Ninja, all seven libraries, QEMU and x86 firmware with native GCC',
              'cd /home/user/Builds/qemu && python3 -B tools/build_qemu.py --native --jobs ' + str(args.jobs) +
              ' > /tmp/qemu-build.log 2>&1 && echo QEMU_NATIVE_BUILD_PASS', 'QEMU_NATIVE_BUILD_PASS', timeout=3600)
        check('source-built emulator passes disk, coroutine, QMP, VNC, BIOS and SDK workflows',
              'cd /home/user/Builds/qemu && python3 -B ports/qemu/check.py --runtime build/qemu-native/runtime --sdk',
              'QEMU_ALL_PASS', timeout=600)
        check('install complete Coast source and ISO build closure', 'brine install coast-build --noconfirm',
              'installing coast-build', timeout=600)
        check('unpack the complete OS sources as the desktop user',
              'mkdir -p /home/user/Builds/coast && cd /home/user/Builds/coast && '
              'tar -xJf /usr/src/coast/os/coast-source.tar.xz --strip-components=1 && echo COAST_KIT_READY', 'COAST_KIT_READY')
        check('compile the kernel, desktop, BusyBox, Limine and bootable ISO inside Coast',
              'cd /home/user/Builds/coast && python3 -B tools/build_native_iso.py --jobs ' + str(args.jobs) +
              ' > /tmp/coast-image-build.log 2>&1 && echo COAST_NATIVE_IMAGE_PASS', 'COAST_NATIVE_IMAGE_PASS', timeout=1800)
        check('native QEMU boots the native-built Coast ISO, checks its ABI and runs its graphical desktop',
              'cd /home/user/Builds/qemu && python3 -B ports/qemu/boot_coast.py '
              '--iso ../coast/build/native-image/Coast-native.iso '
              '--qemu ./build/qemu-native/runtime/usr/bin/qemu-system-x86_64 '
              '--firmware ./build/qemu-native/runtime/usr/share/qemu --gui '
              '> /tmp/coast-nested.log 2>&1 && cat /tmp/coast-nested.log', 'COAST_NESTED_ALL_PASS', timeout=2400)
        check('archive only the native-built runtime and SDK for review',
              'cd /home/user/Builds/qemu/build/qemu-native/runtime && '
              'tar -czf /tmp/qemu-native-runtime.tar.gz usr/bin usr/share usr/lib && echo QEMU_RUNTIME_ARCHIVED', 'QEMU_RUNTIME_ARCHIVED', timeout=240)
        vm.command('check', expect='USERLAND CHECK PASS', timeout=120)
        report['checks'].append('outer Coast remains healthy after native compilation and nested desktop execution')
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
                exports = {
                    '/tmp/qemu-build.log': 'qemu-build.log',
                    '/tmp/coast-image-build.log': 'coast-image-build.log',
                    '/tmp/coast-nested.log': 'coast-nested.log',
                    '/tmp/coast-nested/report.json': 'nested-report.json',
                    '/tmp/coast-nested/serial.log': 'nested-serial.log',
                    '/tmp/coast-nested/desktop.ppm': 'nested-desktop.ppm',
                    '/tmp/qemu-native-runtime.tar.gz': 'qemu-native-runtime.tar.gz',
                    '/home/user/Builds/qemu/build/qemu-native/BUILD.json': 'qemu-build.json',
                    '/home/user/Builds/qemu/build/qemu-native/host/bin/ninja': 'ninja-native',
                    '/home/user/Builds/qemu/build/qemu-native/ninja-1.13.2/COPYING': 'ninja-COPYING',
                    '/home/user/Builds/qemu/build/qemu-native/logs/qemu-configure.log': 'qemu-configure.log',
                    '/home/user/Builds/qemu/build/qemu-native/logs/qemu-build.log': 'qemu-compile.log',
                    '/home/user/Builds/coast/build/native-image/BUILD.json': 'coast-image-build.json',
                    '/home/user/Builds/coast/build/native-image/Coast-native.iso': 'Coast-native.iso',
                }
                for source, name in exports.items():
                    try:
                        ramfs.export(source, out / name)
                        if name == 'ninja-native':
                            (out / name).chmod(0o755)
                    except FileNotFoundError:
                        continue
                    report.setdefault('exports', {})[name] = sha(out / name)
            except BaseException as error:
                report['export_error'] = repr(error)
                if report['state'] == 'passed':
                    report['state'] = 'failed'
                    raise
            finally:
                vm.close()
        if server:
            server.shutdown()
            server.server_close()
        if thread:
            thread.join(5)
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
