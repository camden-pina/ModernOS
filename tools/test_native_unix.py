#!/usr/bin/env python3
"""Compare pathname Unix IPC and daemon sessions with an actual Linux guest."""
import argparse
import base64
import gzip
import hashlib
import json
import os
from pathlib import Path
import select
import subprocess
import time
import traceback
import test_runtime as runtime

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--linux-kernel', type=Path, default=Path('/usr/lib/modules/6.18.55-2-cachyos-lts/vmlinuz'))
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    fixture = out / 'native-unix-parity'
    conf = ['bwrap', '--unshare-all', '--die-with-parent', '--ro-bind', '/', '/',
            '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    subprocess.run([*conf, '--', str(ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-gcc'),
                    '-O2', '-static', '-no-pie', '-mno-avx', '-mno-avx2', '-Wall', '-Wextra', '-Werror',
                    '-Wl,-z,noexecstack', str(ROOT / 'tools/native-unix-parity.c'), '-o', str(fixture)], check=True,
                   stdin=subprocess.DEVNULL)
    binary = fixture.read_bytes()
    archive = bytearray()

    def entry(name, mode, data=b'', rdev=(0, 0)):
        fields = [1, mode, 0, 0, 1, 0, len(data), 0, 0, *rdev, len(name.encode())+1, 0]
        archive.extend(b'070701' + ''.join(f'{v:08x}' for v in fields).encode() + name.encode() + b'\0')
        archive.extend(b'\0' * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b'\0' * (-len(archive) % 4))
    entry('.', 0o40755)
    entry('dev', 0o40755)
    entry('dev/console', 0o20600, rdev=(5, 1))
    entry('dev/null', 0o20666, rdev=(1, 3))
    entry('init', 0o100755, binary)
    entry('TRAILER!!!', 0)
    initrd = out / 'linux-initramfs.cpio'
    initrd.write_bytes(archive)
    command = ['qemu-system-x86_64', '-enable-kvm', '-cpu', 'host', '-smp', '2', '-m', '512M',
               '-kernel', str(args.linux_kernel), '-initrd', str(initrd), '-append', 'console=ttyS0 quiet panic=-1',
               '-display', 'none', '-serial', 'stdio', '-nic', 'none', '-no-reboot']
    linux = vm = None
    report = dict(state='failed', target_host_executed=False, fixture_sha256=hashlib.sha256(binary).hexdigest(),
                  linux_kernel=str(args.linux_kernel), iso=str(args.iso.resolve()))
    try:
        linux = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, start_new_session=True)
        data = bytearray()
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline and b'NATIVE_UNIX_PARITY_DONE' not in data:
            if select.select([linux.stdout], [], [], 0.1)[0]:
                chunk = os.read(linux.stdout.fileno(), 65536)
                if not chunk:
                    break
                data.extend(chunk)
        (out / 'linux.log').write_bytes(data)
        assert b'NATIVE_UNIX_PARITY_DONE' in data, data.decode(errors='replace')[-2500:]
        linux.terminate()
        linux.wait(10)
        runtime.WORK = out
        vm = runtime.VM(2, storage=False, network=False, memory='16G', accel='kvm', iso=args.iso, label='unix-parity')
        encoded = base64.b64encode(gzip.compress(binary, mtime=0)).decode()
        for i in range(0, len(encoded), 400):
            vm.command(('write' if not i else 'append') + ' /tmp/unix.b64 ' + encoded[i:i+400])
        vm.shell_command('/bin/busybox base64 -d /tmp/unix.b64 | /bin/busybox gzip -d > /tmp/unix-parity; chmod 755 /tmp/unix-parity')
        output = vm.shell_command('/tmp/unix-parity', expect='NATIVE_UNIX_PARITY_DONE', timeout=45)
        (out / 'coast.log').write_text(output)

        def traces(text):
            return [line[line.index('UNIX '):].strip() for line in text.splitlines() if 'UNIX ' in line]
        report.update(linux=traces(data.decode(errors='replace')), coast=traces(output))
        assert report['linux'] == report['coast'], 'IPC traces differ; see report.json'
        assert all(line == 'UNIX case-exit 0' for line in report['coast'] if 'case-exit' in line)
        report['state'] = 'passed'
        print('PASS identical Linux / Coast Unix socket DAC, peer credentials, inode reuse and daemon session traces')
    except BaseException as error:
        report['error'] = repr(error)
        (out / 'exception.txt').write_text(traceback.format_exc())
        raise
    finally:
        if vm:
            vm.close()
        if linux and linux.poll() is None:
            linux.terminate()
            linux.wait(10)
        report['owned_guests_stopped'] = bool(linux and linux.poll() is not None and (not vm or vm.process.poll() is not None))
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
