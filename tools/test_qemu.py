#!/usr/bin/env python3
"""Install signed QEMU packages and run them only in an owned Coast VM."""
import argparse
import hashlib
import json
from pathlib import Path
import threading
import traceback

from serve_repository import RepositoryServer
import test_runtime as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=runtime.ROOT / 'build/Coast-codeoss.iso')
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--cpus', type=int, choices=(1, 4), default=4)
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    vm = server = thread = None
    report = dict(state='failed', checks=[], cpus=args.cpus, iso=str(args.iso.resolve()),
                  target_host_executed=False, extra_disks=False, listeners='127.0.0.1 only')

    def check(name, command, expected=None, user=True, timeout=120):
        output = vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out / (f'{len(report["checks"]):02d}-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS ' + name, flush=True)
        return output

    try:
        server = RepositoryServer(('127.0.0.1', 0), args.repository)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        origin = 'http://10.0.2.2:' + str(server.server_port)
        vm = runtime.VM(args.cpus, storage=False, network=True, memory='16G', accel='kvm',
                        iso=args.iso, label='qemu-port')
        vm.command('true')
        check('select repository with existing signing key', 'brine server ' + origin,
              'Package downloads and source builds now use')
        check('verify signed pacman metadata', 'pacman -Syy --noconfirm', 'Synchronizing', user=False)
        check('refresh matching signed file database metadata', 'pacman -Fyy --noconfirm', 'Synchronizing', user=False)
        check('find emulator through Brine search', 'brine search --repo qemu', 'coast/qemu 10.2.4-')
        check('install signed emulator and matching firmware', 'brine install qemu --noconfirm', 'installing qemu', timeout=180)
        check('install dependency SDK through package resolution', 'brine install qemu-devel --noconfirm', 'installing qemu-devel', timeout=240)
        check('verify installed archive ownership and bytes', 'pacman -Qkk qemu qemu-common glib2 libffi pcre2 pixman libslirp libucontext zlib meson qemu-devel', '0 altered files')
        check('native Python runs packaged Meson', 'meson --version', '1.10.2')
        check('native pkg-config resolves private static dependency SDK',
              'PKG_CONFIG_PATH=/usr/lib/coast/qemu-sdk/lib/pkgconfig pkg-config --static --libs glib-2.0 pixman-1 slirp libffi zlib', '-lglib-2.0')
        output = check('disk workflows, QMP and BIOS guest execution',
              'python3 /usr/share/coast/qemu/check.py --sdk', 'QEMU_ALL_PASS', timeout=300)
        report['emulator_checks'] = [line.removeprefix('QEMU_PASS ') for line in output.splitlines()
                                     if line.startswith('QEMU_PASS ')]
        vm.command('check', expect='USERLAND CHECK PASS', timeout=90)
        report['checks'].append('Coast remains healthy after nested emulation')
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
        with args.iso.open('rb') as stream:
            report['iso_sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
