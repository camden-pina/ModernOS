#!/usr/bin/env python3
"""Run a local QEMU guest and view its desktop in Coast's Chromium browser."""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import threading
from urllib.parse import urlsplit


def main():
    allocation_limit = resource.getrlimit(resource.RLIMIT_DATA)[0]
    maximum_memory = 16384 if allocation_limit == resource.RLIM_INFINITY else min(16384, allocation_limit // (1024 * 1024) - 32)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--memory', type=int, default=384 if maximum_memory <= 480 else 4096,
                        help='guest RAM in MiB (maximum depends on the running kernel)')
    parser.add_argument('--cpus', type=int, choices=range(1, 5), default=1)
    parser.add_argument('--http-port', type=int, default=6080)
    parser.add_argument('--websocket-port', type=int, default=5701)
    parser.add_argument('--vnc-display', type=int, choices=range(1, 100), default=1)
    parser.add_argument('--novnc', type=Path, default=Path('/usr/share/novnc'))
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--firmware', type=Path)
    parser.add_argument('--serial', action='store_true', help='also attach the serial console to this terminal')
    parser.add_argument('--network', action='store_true', help='enable QEMU user networking for the nested guest')
    args = parser.parse_args()
    ports = (args.http_port, args.websocket_port, 5900 + args.vnc_display)
    if not 64 <= args.memory <= maximum_memory or any(not 1024 <= p <= 65535 for p in ports) or len(set(ports)) != 3:
        parser.error(f'memory must be 64..{maximum_memory} MiB and all three listener ports must be distinct unprivileged ports')
    image = args.iso.resolve(strict=True)
    data = args.novnc.resolve(strict=True)
    if not (data / 'coast.html').is_file() or not (data / 'core/rfb.js').is_file():
        parser.error('Install coast-vm and novnc first')
    work = Path(tempfile.mkdtemp(prefix='coast-vm-', dir='/tmp'))
    log = (work / 'http.log').open('w')

    class Handler(SimpleHTTPRequestHandler):
        def do_GET(self):
            if urlsplit(self.path).path != '/coast-status':
                return super().do_GET()
            serial = b''
            try:
                with (work / 'serial.log').open('rb') as stream:
                    stream.seek(0, 2)
                    stream.seek(max(0, stream.tell() - 65536))
                    serial = stream.read(65536)
            except FileNotFoundError:
                pass
            data = json.dumps(dict(running=process is not None and process.poll() is None,
                booted=b'System ready:' in serial,
                desktop=serial.rfind(b'GUI_READY:') > serial.rfind(b'gui: returned to terminal.'))).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, fmt, *values):
            log.write(fmt % values + '\n')
            log.flush()

    server = ThreadingHTTPServer(('127.0.0.1', args.http_port), partial(Handler, directory=str(data)))
    command = [args.qemu, '-machine', 'q35', '-accel', 'tcg,thread=single', '-cpu', 'max',
        '-m', str(args.memory), '-smp', str(args.cpus), '-display', 'none', '-monitor', 'none',
        '-serial', 'stdio' if args.serial else 'file:' + str(work / 'serial.log'),
        '-nic', 'user,model=e1000e' if args.network else 'none', '-no-reboot',
        '-boot', 'd', '-cdrom', str(image),
        '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0',
        '-device', 'usb-mouse,bus=xhci.0',
        '-vnc', f'127.0.0.1:{args.vnc_display},websocket=127.0.0.1:{args.websocket_port}']
    if args.firmware:
        command += ['-L', str(args.firmware.resolve(strict=True))]
    process = thread = None
    def stop(_signal, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        # Coast cannot spawn a process from an ordinary multithreaded Python
        # VM. Start QEMU before the HTTP server creates any worker threads.
        process = subprocess.Popen(command, stdin=None if args.serial else subprocess.DEVNULL)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        print(f'Open in Chromium: http://127.0.0.1:{args.http_port}/coast.html?port={args.websocket_port}', flush=True)
        print('VM logs: ' + str(work), flush=True)
        return process.wait()
    except KeyboardInterrupt:
        return 130
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=20)
        if thread:
            server.shutdown()
            thread.join(5)
        server.server_close()
        log.close()


if __name__ == '__main__':
    raise SystemExit(main())
