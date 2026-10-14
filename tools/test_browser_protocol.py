#!/usr/bin/env python3
"""Exercise the actual static NetSurf process and its bounded surface protocol.

This host check catches frontend integration errors independently of QEMU;
the desktop/driver integration is tested separately inside ModernOS.
"""
import fcntl
import os
from pathlib import Path
import selectors
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'build/browser-protocol-test'
HEADER = struct.Struct('<IIIiiiiIII')
MAGIC = 0x3142574d


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    home = WORK / 'home'
    home.mkdir(exist_ok=True)
    first = WORK / 'first.html'
    second = WORK / 'second.html'
    first.write_text('<!doctype html><html><head><title>First native page</title></head>'
                     '<body style="background:#dcebe6"><h1>Real DOM and CSS</h1>'
                     '<a href="' + second.as_uri() + '">Second</a></body></html>')
    second.write_text('<!doctype html><title>Second native page</title><h1>Second document</h1>')
    env = os.environ | {'HOME': str(home),
                        'NETSURFRES': str(ROOT / 'userland/runtime/usr/share/netsurf')}
    log = (WORK / 'stderr.log').open('wb')
    process = subprocess.Popen([ROOT / 'userland/bin/netsurf', '-f', 'modernos', '-w', '800', '-h', '520',
                                first.as_uri()], env=env, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=log)
    fcntl.fcntl(process.stdin, fcntl.F_SETFL, os.O_NONBLOCK)
    fcntl.fcntl(process.stdout, fcntl.F_SETFL, os.O_NONBLOCK)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    pending = bytearray()
    frames = 0

    def send(kind, text='', width=0, height=0):
        value = text.encode()
        process.stdin.write(HEADER.pack(MAGIC, kind, len(
            value), 0, 0, width, height, 0, 0, 0) + value)
        process.stdin.flush()

    def until(kind, text='', timeout=30):
        nonlocal frames
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while len(pending) >= HEADER.size:
                fields = HEADER.unpack_from(pending)
                magic, message, length, x, y, width, height, canvas_width, canvas_height, flags = fields
                assert magic == MAGIC and length <= 32 * 1024 * 1024, fields
                if len(pending) < HEADER.size + length:
                    break

                body = bytes(pending[HEADER.size:HEADER.size + length])
                del pending[:HEADER.size + length]

                if message == 1:
                    assert length == width * height * 4 and x >= 0 and y >= 0, fields
                    assert x + width <= canvas_width and y + height <= canvas_height, fields
                    frames += 1
                else:
                    print('BROWSER_PACKET', message, body.decode(errors='replace'), flush=True)

                if message == kind and (not text or text.encode() in body):
                    return fields, body

            if process.poll() is not None:
                raise AssertionError('NetSurf exited: ' + (WORK / 'stderr.log').read_text(errors='replace'))
            if selector.select(.2):
                pending.extend(os.read(process.stdout.fileno(), 65536))

        raise AssertionError(
            f'Missing browser packet {kind}/{text!r}; stderr=' + (WORK / 'stderr.log').read_text(errors='replace'))

    try:
        until(2, 'First native page')
        fields, pixels = until(1)
        assert len(set(struct.iter_unpack('<I', pixels))) > 8, 'No actual page/glyph rendering'
        send(20, second.as_uri())
        until(2, 'Second native page');
        until(1)
        send(21)
        until(2, 'First native page');
        until(1)
        send(22)
        until(2, 'Second native page');
        until(1)
        send(18, width=900, height=600)

        while True:
            fields, _ = until(1)
            if fields[7:9] == (900, 600):
                break

        send(19)

        # The engine may flush its final invalidation before reading Quit.
        # Keep draining that stream instead of creating artificial backpressure.
        deadline = time.monotonic() + 15
        while process.poll() is None and time.monotonic() < deadline:
            if selector.select(.2):
                os.read(process.stdout.fileno(), 65536)
        process.wait(timeout=1)

        assert process.returncode == 0

        print(
            f'PASS genuine NetSurf protocol: rendered files, navigation/history, resize, clean exit; frames={frames}', flush=True)
    finally:
        if process.poll() is None:
            process.kill();
            process.wait()
        selector.close();
        log.close()


if __name__ == '__main__':
    main()
