"""Drive the real offscreen browser and reconstruct its displayed pixels."""
import os
from pathlib import Path
import selectors
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct('<IIIiiiiIII')
MAGIC = 0x3142574d


class Browser:
    def __init__(self, work, url, width=800, height=520, arguments=()):
        self.work = Path(work)
        self.work.mkdir(parents=True, exist_ok=True)
        home = self.work / 'home'
        home.mkdir(exist_ok=True)
        self.log = (self.work / 'engine.log').open('wb')
        env = os.environ | {'HOME': str(home), 'NETSURFRES': str(
            ROOT / 'userland/runtime/usr/share/netsurf')}
        self.process = subprocess.Popen([ROOT / 'userland/bin/netsurf', *arguments, '-f', 'modernos',
                                         '-w', str(width), '-h', str(height), url],
                                        env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, bufsize=0)
        os.set_blocking(self.process.stdout.fileno(), False)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.pending = bytearray()
        self.pixels = bytearray()
        self.width = self.height = self.frames = 0
        self.flags = 0
        self.title = ''

    def send(self, kind, text='', x=0, y=0, width=0, height=0, flags=0):
        value = text.encode()
        self.process.stdin.write(HEADER.pack(MAGIC, kind, len(
            value), x, y, width, height, 0, 0, flags) + value)

    def packet(self, timeout=10):
        deadline = time.monotonic() + timeout
        while True:
            if len(self.pending) >= HEADER.size:
                fields = HEADER.unpack_from(self.pending)
                magic, kind, length, x, y, width, height, cw, ch, flags = fields
                assert magic == MAGIC and length <= 64 * 1024 * 1024, fields

                if len(self.pending) >= HEADER.size + length:
                    body = bytes(self.pending[HEADER.size:HEADER.size + length])
                    del self.pending[:HEADER.size + length]
                    if kind == 1:
                        assert 300 <= cw <= 4096 and 200 <= ch <= 4096, fields
                        assert x >= 0 and y >= 0 and width > 0 and height > 0, fields
                        assert x + width <= cw and y + height <= ch and length == width * height * 4, fields

                        if (cw, ch) != (self.width, self.height):
                            self.width, self.height = cw, ch
                            self.pixels = bytearray(b'\xff' * (cw * ch * 4))
                        for row in range(height):
                            offset = ((y + row) * cw + x) * 4
                            self.pixels[offset:offset + width * 4] = body[row * width * 4:(row + 1) * width * 4]
                        self.frames += 1
                    elif kind == 2:
                        self.title = body.decode(errors='replace')
                    elif kind == 6:
                        self.flags = flags
                    return kind, body

            if self.process.poll() is not None:
                raise AssertionError('Browser exited: ' + (self.work / 'engine.log').read_text(errors='replace'))
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            if self.selector.select(min(remaining, .2)):
                self.pending.extend(os.read(self.process.stdout.fileno(), 256 * 1024))

    def ready(self, title, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.packet(min(1, deadline - time.monotonic()))
            if title in self.title and not self.flags & 4 and self.frames:
                self.drain()
                return

        raise AssertionError(
            f'Browser did not finish {title!r}: ' + (self.work / 'engine.log').read_text(errors='replace')[-5000:])

    def drain(self, seconds=.5):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.packet(min(.1, deadline - time.monotonic()))

    def navigate(self, url, title):
        self.send(20, url)
        self.ready(title)

    def key(self, code):
        self.send(16, x=code, flags=1)
        self.send(16, x=code)
        self.drain()

    def click(self, x, y):
        self.send(17, x=x, y=y)
        self.send(17, x=x, y=y, flags=1)
        self.send(17, x=x, y=y)
        self.drain()

    def rgb(self, x, y):
        at = (y * self.width + x) * 4
        b, g, r = self.pixels[at:at + 3]
        return r, g, b

    def screenshot(self, name):
        rgb = bytearray(self.width * self.height * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = self.pixels[2::4], self.pixels[1::4], self.pixels[0::4]
        path = self.work / (name + '.ppm')
        path.write_bytes(f'P6\n{self.width} {self.height}\n255\n'.encode() + rgb)
        return path

    def close(self):
        if self.process.poll() is None:
            self.send(19)
            deadline = time.monotonic() + 10
            while self.process.poll() is None and time.monotonic() < deadline:
                if self.selector.select(.1):
                    os.read(self.process.stdout.fileno(), 256 * 1024)
            if self.process.poll() is None:
                self.process.kill()

        self.process.wait()
        self.selector.close()
        self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()
