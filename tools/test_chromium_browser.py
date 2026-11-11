#!/usr/bin/env python3
"""Exercise the real Chromium browser in an owned Coast QEMU guest via USB."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import ssl
import subprocess
import threading
import time
import traceback
from source_tree import digest
import test_runtime as runtime
import test_desktop as desktop


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iso', type=Path, required=True)
    p.add_argument('--output-root', type=Path, required=True)
    p.add_argument('--accel', choices=['kvm', 'tcg'], default='kvm')
    p.add_argument('--cpus', type=int, default=2)
    p.add_argument('--timeout-scale', type=float, default=1,
                   help='scale UI observation deadlines on a busy build host (1..4)')
    p.add_argument('--live', action='store_true')
    p.add_argument('--network-backend', choices=['user', 'passt'], default='passt')
    args = p.parse_args()
    if not 1 <= args.timeout_scale <= 4:
        p.error('--timeout-scale must be between 1 and 4')
    out = args.output_root.resolve(); out.mkdir(parents=True, exist_ok=False)
    (out / 'HARNESS.py').write_bytes(Path(__file__).read_bytes())
    runtime.WORK = desktop.WORK = out
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args): pass
        def do_GET(self):
            requests.append(dict(path=self.path, headers=dict(self.headers)))
            if self.path == '/download':
                data = b'COAST_CHROMIUM_DOWNLOAD_OK\n'; content = 'application/octet-stream'
            elif self.path == '/data':
                data = b'network works'; content = 'text/plain'
            else:
                data = ('''<!doctype html><meta charset=utf-8><title>Fixture loading</title>
<style>body{margin:0;background:#1964ae;color:white;font:24px sans-serif}main{padding:32px}a{color:white}canvas{background:#e54343}article{height:1800px}</style>
<main><h1>Coast Chromium fixture</h1><p id=proof>Loading…</p><a href=/second>Second page</a> · <a href=/download>Download</a><form action=/submitted><p><input id=edit name=message aria-label=Editor value="editable text"></p></form><canvas id=canvas width=160 height=80></canvas><article>Scroll the real page</article></main>
<script>document.cookie='coastBrowser=ok;SameSite=Lax';localStorage.setItem('coastBrowser','ok');
const c=document.getElementById('canvas').getContext('2d');c.fillStyle='#e54343';c.fillRect(0,0,160,80);
fetch('/data').then(r=>r.text()).then(text=>{document.getElementById('proof').textContent=text+' / storage '+localStorage.getItem('coastBrowser')+' / node '+typeof process+' / require '+typeof require;document.title=location.pathname==='/second'?'Fixture second':'Fixture ready';});</script>''').encode(); content = 'text/html; charset=utf-8'
            self.send_response(200); self.send_header('Content-Type', content); self.send_header('Content-Length', str(len(data)))
            if self.path == '/download': self.send_header('Content-Disposition', 'attachment; filename="proof.txt"')
            self.end_headers(); self.wfile.write(data)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    key = out / 'untrusted.key'
    try:
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
                        '-keyout', str(key), '-out', str(out / 'untrusted.pem'),
                        '-subj', '/CN=Untrusted Coast fixture', '-addext', 'subjectAltName=IP:10.0.2.2'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        tls = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(out / 'untrusted.pem', key)
    finally:
        key.unlink(missing_ok=True)
    tls.socket = context.wrap_socket(tls.socket, server_side=True)
    threading.Thread(target=tls.serve_forever, daemon=True).start()
    origin = 'http://10.0.2.2:' + str(server.server_port)
    vm = d = None
    report = dict(state='failed', iso=str(args.iso.resolve()), checks=[], requests=requests,
                  timeout_scale=args.timeout_scale, iso_sha256=digest(args.iso),
                  harness_sha256=digest(Path(__file__)))

    def raw():
        while vm.selector.select(.005):
            data = os.read(vm.process.stdout.fileno(), 65536)
            if not data: break
            vm.pending.extend(data); vm.log.write(data); vm.log.flush()
        return Path(vm.log.name).read_text(errors='replace')

    def wait(pattern, start=0, timeout=45):
        deadline = time.monotonic() + timeout * args.timeout_scale
        while time.monotonic() < deadline:
            text = raw()[start:]
            assert 'PANIC:' not in text, text[-3000:]
            match = re.search(pattern, text)
            if match: return match
            time.sleep(.025)
        raise AssertionError('Missing ' + pattern + '\n' + raw()[-5000:])

    def passed(description):
        report['checks'].append(description); print('PASS ' + description, flush=True)

    def type_text(text):
        keys = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', '_': 'minus', ':': 'semicolon', '?': 'slash', '=': 'equal', '&': '7', '#': '3', '+': 'equal'}
        for char in text:
            raw()
            shift = char.isupper() or char in '_:?&#+ '
            if char == ' ': shift = False
            d.timed_key(keys.get(char, char.lower()), *(['shift'] if shift else []))

    def navigate(url):
        start = len(raw())
        d.timed_key('l', 'ctrl')
        wait(r'COAST_BROWSER .*"event":"address-ready","focused":true', start)
        type_text(url)
        d.timed_key('ret')
        return start

    try:
        vm = runtime.VM(args.cpus, storage=False, network=True, memory='16G', accel=args.accel, iso=args.iso.resolve(), label='chromium-browser', network_backend=args.network_backend)
        vm.command('mkdir -p /home/user/.config')
        vm.shell_command("printf 'scale=3\\nterminal_shell=1\\n' > /home/user/.config/desktop.ini")
        vm.send("build-sh -c 'COAST_BROWSER_QA=1 exec /bin/gui --trace --stats'\n")
        vm.until(rb'GUI_READY:', 60); d = desktop.DesktopTest(vm); d.screenshot('00-desktop')
        window = d.launch('Chromium Browser'); wait(r'COAST_BROWSER .*"event":"ready"', timeout=60)
        wait(r'COAST_BROWSER .*"event":"page"[^\n]*Welcome to Coast', timeout=30)
        time.sleep(2); home = d.screenshot('01-home')
        assert home.count(bytes.fromhex('f8fafc')) > 10000, 'Actual home page pixels missing'
        wait('BROWSER_NETWORK_CHECK_PASS')
        passed('Native desktop launches sandboxed Chromium and paints its home page')
        start = navigate(origin + '/')
        wait(r'COAST_BROWSER .*"event":"page"[^\n]*network works / storage ok / node undefined / require undefined', start)
        time.sleep(1); pixels = d.screenshot('02-http')
        assert pixels.count(bytes.fromhex('1964ae')) > 10000, 'Actual page background pixels missing'
        assert pixels.count(bytes.fromhex('e54343')) > 1000, 'Actual canvas pixels missing'
        passed('HTTP, JavaScript fetch, cookies, storage, canvas pixels and Node isolation work in the guest')
        report['geometry'] = d.geometry(window)
        g = report['geometry']; d.click(g['x'] + g['width'] - 100, g['y'] + g['height'] - 100)
        d.key('end', 'ctrl'); time.sleep(.3); scrolled = d.screenshot('02a-scrolled')
        assert scrolled.count(bytes.fromhex('e54343')) < 100, 'Page canvas did not scroll out of view'
        d.key('home', 'ctrl'); time.sleep(.2)
        for _ in range(3): d.key('tab')
        d.key('a', 'ctrl'); type_text('coast-form'); start = len(raw()); d.key('ret')
        wait(r'COAST_BROWSER .*"event":"navigation"[^\n]*' + re.escape(origin + '/submitted?message=coast-form'), start)
        passed('Real page scrolling, editable input and a submitted HTML form work through USB input')
        start = navigate(origin + '/second')
        wait(r'COAST_BROWSER .*"event":"page"[^\n]*"title":"Fixture second"', start)
        start = len(raw()); d.key('left', 'alt')
        wait(r'COAST_BROWSER .*"event":"navigation"[^\n]*' + re.escape(origin + '/submitted?message=coast-form') + r'"', start)
        start = len(raw()); d.key('right', 'alt')
        wait(r'COAST_BROWSER .*"event":"navigation"[^\n]*' + re.escape(origin + '/second') + r'"', start)
        passed('Back and forward use Chromium navigation history through real USB shortcuts')
        start = len(raw()); d.key('t', 'ctrl')
        wait(r'COAST_BROWSER .*"event":"page","id":2[^\n]*Welcome to Coast', start)
        start = navigate(origin + '/tab-two')
        wait(r'COAST_BROWSER .*"event":"page","id":2[^\n]*network works / storage ok', start)
        d.screenshot('03-tabs'); d.key('w', 'ctrl'); time.sleep(.3)
        passed('A second isolated tab navigates and closes, restoring the previous tab')
        for tab_id in range(3, 6):
            start = len(raw()); d.key('t', 'ctrl')
            wait(r'COAST_BROWSER .*"event":"page","id":' + str(tab_id) + r'[^\n]*Welcome to Coast', start)
        start = len(raw()); d.key('t', 'ctrl'); wait(r'COAST_BROWSER .*"event":"tab-limit","max":4', start)
        d.screenshot('03a-four-tabs')
        for _ in range(3): d.key('w', 'ctrl')
        wait(r'COAST_BROWSER .*"event":"tabs","count":1', start)
        passed('Four tabs render within the finite process pool; a fifth is refused and all extra tabs close')
        layouts = re.findall(r'DESKTOP GAME_VIEW window=' + str(window) + r' x=-?\d+ y=-?\d+ width=(\d+) height=(\d+)', raw())
        assert layouts, 'Missing the actual pre-fullscreen client layout'
        restored_width, restored_height = layouts[-1]
        start = len(raw()); d.key('f11'); wait(r'DESKTOP FULLSCREEN window=' + str(window), start)
        wait(r'DESKTOP NATIVE_LAYOUT window=' + str(window) + r' width=2560 height=1600', start)
        time.sleep(.3); full = d.screenshot('03b-fullscreen')
        assert full.count(bytes.fromhex('1964ae')) > 10000, 'Fullscreen page pixels missing'
        start = len(raw()); d.key('f11'); wait(r'DESKTOP UNFULLSCREEN window=' + str(window), start)
        # The shell acknowledges unfullscreen before the Wayland resize and
        # renderer frame finish. Wait for that work before focusing the URL.
        wait(r'DESKTOP NATIVE_LAYOUT window=' + str(window) + r' width=' + restored_width + r' height=' + restored_height, start)
        restored = d.screenshot('03c-unfullscreen')
        assert restored.count(bytes.fromhex('1964ae')) > 10000, 'Restored page pixels missing'
        passed('Native fullscreen reconfigures Chromium and preserves actual page pixels')
        start = navigate('https://10.0.2.2:' + str(tls.server_port) + '/')
        wait(r'COAST_BROWSER .*"event":"load-error"[^\n]*ERR_CERT_AUTHORITY_INVALID', start)
        d.screenshot('04-untrusted-certificate')
        passed('An untrusted HTTPS certificate is rejected without a bypass')
        if args.live:
            start = navigate('https://example.com/')
            wait(r'COAST_BROWSER .*"event":"page"[^\n]*Example Domain', start, timeout=60)
            time.sleep(1); d.screenshot('05-public-https'); passed('Public certificate-verified HTTPS page renders in Chromium')
        start = navigate(origin + '/download')
        wait(r'COAST_BROWSER .*"event":"download"[^\n]*"state":"completed"', start)
        passed('A real HTTP attachment completes in the guest Downloads directory')
        d.key('f4', 'alt')
        wait(r'C1_RUNTIME_WAIT status=0 exit=0 signal=0 timeout=0', timeout=30)
        wait(r'C1_RUNTIME_DESCENDANTS_REAPED helpers=\d+ complete=1', timeout=30)
        passed('Closing the native window exits Chromium and reaps its helpers')
        d.exit()
        result = vm.command('cat /home/user/Downloads/proof.txt')
        assert 'COAST_CHROMIUM_DOWNLOAD_OK' in result, result
        passed('The downloaded file contains the exact server bytes after the browser exits')
        assert any('/data' == request['path'] for request in requests)
        assert any('coastBrowser=ok' in request['headers'].get('Cookie', '') for request in requests)
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error); (out / 'exception.txt').write_text(traceback.format_exc())
        if d:
            try: d.screenshot('failure')
            except BaseException: pass
            try: d.exit()
            except BaseException: pass
        raise
    finally:
        if vm: vm.close()
        server.shutdown(); server.server_close()
        tls.shutdown(); tls.server_close()
        report['owned_vm_stopped'] = bool(vm and vm.process.poll() is not None)
        report['image_unchanged'] = digest(args.iso) == report['iso_sha256']
        if not report['image_unchanged']:
            report['state'] = 'failed'
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        if not report['image_unchanged']:
            raise ValueError('The browser-tested image changed')


if __name__ == '__main__': main()
