#!/usr/bin/env python3
"""Exercise the real Coast QEMU binaries, disk formats, QMP and guest CPU."""
import argparse
import json
import os
from pathlib import Path
import socket
import shlex
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path)
    parser.add_argument('--sdk', action='store_true', help='also compile against the installed qemu-devel SDK')
    parser.add_argument('--runtime', type=Path, help='validate newly built programs, firmware and SDK before installing them')
    args = parser.parse_args()
    runtime = args.runtime.resolve() if args.runtime else Path('/')
    fixtures = runtime / 'usr/share/coast/qemu'
    firmware = runtime / 'usr/share/qemu'
    sdk = runtime / 'usr/lib/coast/qemu-sdk'
    if args.runtime:
        os.environ['PATH'] = str(runtime / 'usr/bin') + ':' + os.environ.get('PATH', '/usr/bin:/bin')
    work = Path(tempfile.mkdtemp(prefix='qemu-check-', dir=args.work or '/tmp'))
    checks = []

    def passed(name):
        checks.append(name)
        print('QEMU_PASS ' + name, flush=True)

    def run(*command, expected=0, timeout=60):
        if command[0] == 'qemu-system-x86_64':
            command = (command[0], '-L', str(firmware), *command[1:])
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=timeout)
        if result.returncode != expected:
            raise AssertionError(f'{command}: exit {result.returncode}\n{result.stdout}')
        return result.stdout

    for program in ('qemu-system-x86_64', 'qemu-img', 'qemu-io'):
        assert '10.2.4' in run(program, '--version')
    passed('three upstream program versions')
    run(str(fixtures / 'context-check'))
    passed('libucontext stack switching and POSIX signal masks')
    raw, qcow, restored = (work / name for name in ('data.raw', 'data.qcow2', 'restored.raw'))
    run('qemu-img', 'create', '-f', 'raw', str(raw), '2M')
    payload = b'Coast QEMU block data\x00' * 190
    with raw.open('r+b') as stream:
        stream.seek(32768)
        stream.write(payload)
    run('qemu-img', 'convert', '-f', 'raw', '-O', 'qcow2', str(raw), str(qcow))
    info = json.loads(run('qemu-img', 'info', '--output=json', str(qcow)))
    assert info['format'] == 'qcow2' and info['virtual-size'] == 2 * 1024 * 1024
    run('qemu-img', 'check', str(qcow))
    run('qemu-img', 'convert', '-f', 'qcow2', '-O', 'raw', str(qcow), str(restored))
    assert restored.read_bytes() == raw.read_bytes()
    passed('raw/qcow2 conversion preserves every byte')

    run('qemu-img', 'snapshot', '-c', 'before', str(qcow))
    run('qemu-io', '-f', 'qcow2', '-c', 'write -P 0x5a 65536 4096', str(qcow))
    run('qemu-io', '-f', 'qcow2', '-c', 'read -P 0x5a 65536 4096', str(qcow))
    run('qemu-img', 'snapshot', '-a', 'before', str(qcow))
    run('qemu-io', '-f', 'qcow2', '-c', 'read -P 0x00 65536 4096', str(qcow))
    run('qemu-img', 'snapshot', '-d', 'before', str(qcow))
    passed('qcow2 write/read and snapshot rollback')

    qmp = work / 'qmp.sock'
    vnc = work / 'vnc.sock'
    log = work / 'qmp.log'
    with log.open('w') as output:
        process = subprocess.Popen(['qemu-system-x86_64', '-L', str(firmware), '-machine', 'q35',
            '-accel', 'tcg,thread=single', '-m', '32M', '-S', '-display', 'none',
            '-nodefaults', '-nic', 'user,model=e1000e,restrict=on', '-device', 'VGA',
            '-vnc', f'unix:{vnc}', '-qmp', f'unix:{qmp},server=on,wait=off'],
            stdout=output, stderr=subprocess.STDOUT)
        try:
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.settimeout(10)
            deadline = time.monotonic() + 40
            while True:
                try:
                    connection.connect(str(qmp))
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise AssertionError('QMP startup failed: ' + log.read_text())
                    time.sleep(.05)
            with connection, connection.makefile('rwb', buffering=0) as wire:
                assert 'QMP' in json.loads(wire.readline())
                def command(name):
                    wire.write((json.dumps({'execute': name}) + '\n').encode())
                    while True:
                        response = json.loads(wire.readline())
                        if 'event' not in response:
                            assert 'return' in response, response
                            return response['return']
                command('qmp_capabilities')
                assert command('query-status')['status'] == 'prelaunch'
                assert len(command('query-cpus-fast')) == 1
                assert any(m['name'] == 'q35' or m.get('alias') == 'q35'
                           for m in command('query-machines'))
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as viewer:
                    viewer.settimeout(10)
                    viewer.connect(str(vnc))
                    assert viewer.recv(12).startswith(b'RFB 003.'), 'VNC handshake'
                command('quit')
            assert process.wait(timeout=15) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
    passed('QMP control, libslirp/e1000e initialization and local VNC handshake')

    output = run('qemu-system-x86_64', '-machine', 'pc', '-accel', 'tcg,thread=single',
        '-m', '32M', '-display', 'none', '-monitor', 'none', '-serial', 'stdio',
        '-nic', 'none', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
        '-drive', 'file=' + str(fixtures / 'boot.img') + ',format=raw,if=floppy,readonly=on',
        '-boot', 'a', expected=33, timeout=180)
    assert 'COAST_QEMU_GUEST_PASS 42 LONG64' in output and 'GUEST_FAIL' not in output, output
    passed('BIOS boots guest, enters x86-64 long mode, checks CPU/RAM, writes serial and exits')
    if args.sdk:
        sdk_environment = dict(os.environ, PKG_CONFIG_PATH=str(sdk / 'lib/pkgconfig'),
                               PKG_CONFIG_LIBDIR=str(sdk / 'lib/pkgconfig'),
                               PKG_CONFIG_SYSROOT_DIR=str(runtime))
        sdk_flags = subprocess.check_output(['pkg-config', '--static', '--cflags', '--libs',
            'glib-2.0', 'libpcre2-8', 'pixman-1', 'zlib', 'libffi', 'slirp', 'libucontext'],
            env=sdk_environment, text=True)
        consumer = work / 'sdk-check'
        run('gcc', '-O2', '-static', '-no-pie', '-fno-pie', '-mno-avx', '-mno-avx2',
            '-Wa,-mrelax-relocations=no', str(fixtures / 'sdk-check.c'),
            *shlex.split(sdk_flags), '-Wl,-z,max-page-size=4096,-z,noexecstack',
            '-o', str(consumer), timeout=90)
        assert 'QEMU_SDK_PASS' in run(str(consumer))
        passed('Coast GCC builds and runs a consumer of all seven dependency libraries')
    print('QEMU_ALL_PASS ' + str(len(checks)), flush=True)


if __name__ == '__main__':
    main()
