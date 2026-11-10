#!/usr/bin/env python3
"""Verify real 2 GiB QEMU guest RAM and data above the old native mapping cap."""
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time


def main():
    work = Path(tempfile.mkdtemp(prefix='qemu-large-', dir='/tmp'))
    control = work / 'control.sock'
    memory = work / 'memory.bin'
    sentinel = 0x434f41535451454d
    command = ['qemu-system-x86_64', '-L', '/usr/share/qemu', '-machine', 'pc',
        '-accel', 'tcg,thread=single', '-m', '2048M', '-S', '-nodefaults',
        '-display', 'none', '-monitor', 'none', '-serial', 'none', '-nic', 'none',
        '-qmp', f'unix:{control},server=on,wait=off',
        '-device', f'loader,addr=0x40001000,data=0x{sentinel:x},data-len=8']
    with (work / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 90
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(20)
                while True:
                    try:
                        connection.connect(str(control))
                        break
                    except (FileNotFoundError, ConnectionRefusedError):
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise AssertionError('Large QEMU RAM startup failed: ' + (work / 'qemu.log').read_text())
                        time.sleep(.05)
                with connection.makefile('rwb', buffering=0) as wire:
                    assert 'QMP' in json.loads(wire.readline())

                    def execute(name, arguments=None):
                        wire.write((json.dumps(dict(execute=name, arguments=arguments or {})) + '\n').encode())
                        while True:
                            response = json.loads(wire.readline())
                            if 'event' not in response:
                                assert 'return' in response, response
                                return response['return']

                    execute('qmp_capabilities')
                    assert execute('query-memory-size-summary')['base-memory'] == 2 * 1024**3
                    result = execute('human-monitor-command', {'command-line': f'pmemsave 0x40001000 8 {json.dumps(str(memory))}'})
                    assert result == '', result
                    assert memory.read_bytes() == struct.pack('<Q', sentinel)
                    execute('quit')
            assert process.wait(timeout=30) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=30)
    print('QEMU_LARGE_RAM_PASS 2GiB guest allocation and exact memory bytes above 1GiB; clean shutdown', flush=True)


if __name__ == '__main__':
    main()
