#!/usr/bin/env python3
"""Measure real guest navigations and exercise clipboard through USB input."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import socket
import threading
import time
import traceback
from urllib.parse import parse_qs, urlsplit
import test_runtime as runtime
import test_desktop as desktop

WEB_TEXT = 'Web-α-€'
NATIVE_TEXT = 'Native-α-€'

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iso', type=Path, required=True)
    p.add_argument('--output-root', type=Path, required=True)
    p.add_argument('--live', action='store_true')
    p.add_argument('--clipboard', action='store_true')
    p.add_argument('--cpus', type=int, default=2)
    p.add_argument('--transfer-only', action='store_true')
    p.add_argument('--rx-delay-us', type=int, default=0)
    p.add_argument('--network-checks', action='store_true')
    p.add_argument('--network-backend', choices=['user', 'passt'], default='user')
    p.add_argument('--google-only', action='store_true')
    p.add_argument('--profile', action='store_true')
    args = p.parse_args()
    out = args.output_root.resolve(); out.mkdir(parents=True, exist_ok=False)
    (out / 'HARNESS.py').write_bytes(Path(__file__).read_bytes())
    runtime.WORK = desktop.WORK = out
    requests, samples = [], []
    bulk = (b'/* Coast bulk asset */\n' * 400000)[:8*1024*1024]
    assert len(bulk) == 8*1024*1024

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        def setup(self):
            super().setup(); self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        def log_message(self, *_args): pass
        def do_GET(self):
            requests.append(dict(path=self.path, at=time.monotonic()))
            if self.path == '/bulk.css':
                data = bulk; content = 'text/css'
            elif re.match(r'/asset[0-9]+\.css', self.path):
                data = b'/* coast asset */' * 4096; content = 'text/css'
            else:
                assets = ''.join('<link rel=stylesheet href="/asset%d.css">' % n for n in range(24)) if '/assets' in self.path else ''
                if self.path.startswith('/bulk'): assets = '<link rel=stylesheet href=/bulk.css>'
                script = 'let s=0;for(let i=0;i<10000000;i++)s=(s+i)|0;' if '/script' in self.path else ''
                data = ('''<!doctype html><meta charset=utf-8>''' + assets + '''
<style>body{background:#1964ae;color:white;font:24px sans-serif}</style>
<h1>Coast benchmark</h1><form action=/submitted><input name=source value="Web-α-€"><input name=target><button>Submit</button></form>
<script>''' + script + '''document.title='Benchmark ready '+location.pathname;</script>''').encode()
                content = 'text/html; charset=utf-8'
            self.send_response(200)
            self.send_header('Content-Type', content)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers(); self.wfile.write(data)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = 'http://10.0.2.2:' + str(server.server_port)
    vm = d = None
    report = dict(state='failed', iso=str(args.iso.resolve()), cpus=args.cpus, rx_delay_us=args.rx_delay_us, network_backend=args.network_backend, samples=samples, requests=requests)

    def raw():
        while vm.selector.select(.002):
            data = os.read(vm.process.stdout.fileno(), 65536)
            if not data: break
            vm.pending.extend(data); vm.log.write(data); vm.log.flush()
        return Path(vm.log.name).read_text(errors='replace')

    def wait(pattern, start=0, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = raw()[start:]
            assert 'PANIC:' not in text, text[-2000:]
            match = re.search(pattern, text)
            if match: return match
            time.sleep(.005)
        raise AssertionError('Missing ' + pattern + '\n' + raw()[-4000:])

    def type_text(text):
        keys = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', '_': 'minus', ':': 'semicolon', '?': 'slash', '=': 'equal', '&': '7'}
        for char in text:
            raw(); d.key(keys.get(char, char.lower()), *(['shift'] if char.isupper() or char in '_:?&' else []))

    def navigate(url, marker, label):
        d.key('l', 'ctrl')
        # Let the guest process Ctrl release before the first URL character.
        time.sleep(.25)
        type_text(url)
        def cpu_time():
            fields = Path(f'/proc/{vm.process.pid}/stat').read_text().split(') ', 1)[1].split()
            return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')
        start = len(raw()); began = time.monotonic(); cpu_before = cpu_time(); d.key('ret')
        wait(marker, start)
        elapsed = time.monotonic() - began
        sample = dict(label=label, url=url, seconds=elapsed, cpu_seconds=cpu_time()-cpu_before,
                      first_request_seconds=next((r['at'] - began for r in requests if r['at'] >= began and not re.match(r'/asset[0-9]+\.css', r['path'])), None))
        samples.append(sample); print(json.dumps(sample), flush=True)
        d.screenshot(label)
        return start

    try:
        backend = 'user,id=perfnet' if args.network_backend == 'user' else runtime.PASST_OPTIONS + ',id=perfnet'
        network_args = ['-netdev', backend, '-device', 'e1000e,netdev=perfnet',
                        '-object', 'filter-dump,id=nettrace,netdev=perfnet,file=' + str(out / 'network.pcap')]
        if args.rx_delay_us:
            network_args += ['-object', f'filter-buffer,id=delay,netdev=perfnet,queue=rx,interval={args.rx_delay_us}']
        vm = runtime.VM(args.cpus, storage=False, network=False, memory='16G', accel='kvm', iso=args.iso.resolve(), label='chromium-benchmark', extra_args=network_args)
        vm.command('mkdir -p /home/user/.config')
        vm.shell_command("printf '" + NATIVE_TEXT + "' | clip -c")
        vm.shell_command("printf 'scale=3\\nterminal_shell=1\\n' > /home/user/.config/desktop.ini")
        vm.send("build-sh -c 'COAST_BROWSER_QA=1 " + ('COAST_BROWSER_PROFILE=1 ' if args.profile else '') + "exec /bin/gui --trace --stats'\n")
        vm.until(rb'GUI_READY:', 60); d = desktop.DesktopTest(vm)
        began = time.monotonic(); d.launch('Chromium Browser')
        wait(r'COAST_BROWSER .*"event":"page"[^\n]*Welcome to Coast', timeout=90)
        report['launch_seconds'] = time.monotonic() - began
        print('Launch seconds:', report['launch_seconds'], flush=True)
        for route in ([] if args.google_only else ['bulk1','bulk2'] if args.transfer_only else ['static1', 'static2', 'assets1', 'assets2', 'script1', 'script2']):
            navigate(origin + '/' + route, r'COAST_BROWSER .*"event":"loaded"[^\n]*Benchmark ready /' + route, route)
        if args.live or args.google_only:
            for label, url, title in [('example', 'https://example.com/', 'Example Domain'), ('google', 'https://www.google.com/', 'Google'), ('osdev', 'https://osdev.org/', 'OSDev')]:
                if args.google_only and label != 'google': continue
                try:
                    marker = r'COAST_BROWSER .*"event":"loaded"[^\n]*' + (r'"url":"https://osdev.org/' if label == 'osdev' else title)
                    start = navigate(url, marker, label)
                    if label == 'osdev': report['osdev_verification_required'] = 'Just a moment' in raw()[start:]
                except AssertionError as error:
                    report.setdefault('live_errors', {})[label] = str(error)[0:200]
                    d.screenshot(label + '-incomplete')
                    print('Live page incomplete:', label, flush=True)
        if args.clipboard:
            navigate(origin + '/clipboard', r'COAST_BROWSER .*"event":"loaded"[^\n]*Benchmark ready /clipboard', 'clipboard')
            start = len(requests)
            d.key('tab'); d.key('tab'); d.key('v', 'ctrl'); time.sleep(.3); d.key('ret')
            wait(r'COAST_BROWSER .*"event":"navigation"[^\n]*/submitted\?', timeout=10)
            transferred = next((r['path'] for r in requests[start:] if r['path'].startswith('/submitted')), '')
            report['native_to_browser'] = parse_qs(urlsplit(transferred).query).get('target') == [NATIVE_TEXT]
            print('Native to browser:', report['native_to_browser'], transferred, flush=True)
            navigate(origin + '/clipboard', r'COAST_BROWSER .*"event":"loaded"[^\n]*Benchmark ready /clipboard', 'clipboard-copy')
            d.key('tab'); d.key('a', 'ctrl'); d.key('c', 'ctrl'); time.sleep(.3)
            d.key('tab'); d.key('v', 'ctrl'); time.sleep(.3); d.key('ret')
            wait(r'COAST_BROWSER .*"event":"navigation"[^\n]*/submitted\?', timeout=10)
            report['clipboard_request'] = next((r['path'] for r in reversed(requests) if r['path'].startswith('/submitted')), None)
            report['clipboard_passed'] = parse_qs(urlsplit(report['clipboard_request']).query) == dict(source=[WEB_TEXT], target=[WEB_TEXT])
            print('Clipboard:', report['clipboard_request'], flush=True)
            d.screenshot('clipboard-result')
            navigate(origin + '/cut', r'COAST_BROWSER .*"event":"loaded"[^\n]*Benchmark ready /cut', 'clipboard-cut')
            start = len(raw()); request_start = len(requests)
            d.key('tab'); d.key('a', 'ctrl'); d.key('x', 'ctrl'); d.key('tab'); d.key('insert', 'shift'); d.key('ret')
            wait(r'COAST_BROWSER .*"event":"navigation"[^\n]*/submitted\?', start, timeout=10)
            cut = next((r['path'] for r in requests[request_start:] if r['path'].startswith('/submitted')), '')
            report['cut_and_shift_insert'] = parse_qs(urlsplit(cut).query, keep_blank_values=True) == dict(source=[''], target=[WEB_TEXT])
            # Cross the native compositor boundary, then verify actual guest
            # file bytes after closing the GUI. A same-client paste is not
            # sufficient to prove that the operating system clipboard works.
            navigate(origin + '/clipboard', r'COAST_BROWSER .*"event":"loaded"[^\n]*Benchmark ready /clipboard', 'clipboard-native')
            d.key('tab'); d.key('a', 'ctrl'); d.key('c', 'ctrl'); time.sleep(.3)
            d.launch('Text Editor'); d.key('v', 'ctrl')
            from test_clipboard_desktop import save_as
            save_as(d, '/tmp/browser-copied.txt')
            d.key('f4', 'alt')
            d.key('l', 'ctrl'); d.key('c', 'ctrl'); time.sleep(.3)
            d.launch('Text Editor'); d.key('v', 'ctrl')
            save_as(d, '/tmp/address-copied.txt')
            d.key('f4', 'alt')
            d.launch('Text Editor'); type_text(origin + '/address-pasted')
            save_as(d, '/tmp/native-address.txt')
            d.key('a', 'ctrl'); d.key('c', 'ctrl'); time.sleep(.3); d.key('f4', 'alt')
            start = len(raw()); d.key('l', 'ctrl'); d.key('v', 'ctrl'); d.key('ret')
            wait(r'COAST_BROWSER .*"event":"navigation"[^\n]*' + re.escape(origin + '/address-pasted'), start, timeout=10)
            report['native_to_address'] = True
        d.key('f4', 'alt'); wait(r'C1_RUNTIME_WAIT status=0 exit=0 signal=0 timeout=0', timeout=30)
        d.exit()
        if args.network_checks:
            vm.command('/bin/chromium-network-check --large-read ' + str(server.server_port), 'BROWSER_LARGE_READ_PASS', timeout=90)
            vm.shell_command('sha256sum /tmp/chromium-large-read.bin', hashlib.sha256(bulk).hexdigest())
            vm.command('/bin/chromium-network-check --tcp-pool-check', 'BROWSER_TCP_POOL_PASS', timeout=60)
            report['large_read_hash_and_pool_refund'] = True
        if args.clipboard:
            result = vm.shell_command('cat /tmp/browser-copied.txt')
            report['browser_to_native'] = result.strip('\n') == WEB_TEXT
            result = vm.shell_command('cat /tmp/address-copied.txt')
            report['address_to_native'] = result.strip('\n') == origin + '/clipboard'
            print('Cross-app clipboard:', {k:v for k,v in report.items() if 'native' in k}, flush=True)
            assert all(report.get(key) for key in ['native_to_browser', 'browser_to_native', 'address_to_native', 'native_to_address', 'clipboard_passed', 'cut_and_shift_insert']), 'Clipboard transfer failed'
            vm.command('clipboard-check', 'CLIPBOARD CHECK PASS', timeout=60)
            report['native_clipboard_checks'] = True
        if args.live or args.google_only: assert not report.get('live_errors'), report['live_errors']
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = repr(error); (out / 'exception.txt').write_text(traceback.format_exc())
        if d:
            try: d.screenshot('failure')
            except BaseException: pass
        raise
    finally:
        if vm: vm.close()
        server.shutdown(); server.server_close()
        log = out / f'uefi-{args.cpus}cpu-chromium-benchmark.log'
        if log.is_file():
            observations = []
            for line in log.read_text(errors='replace').splitlines():
                if line.startswith('COAST_BROWSER '):
                    try:
                        record = json.loads(line[len('COAST_BROWSER '):])
                        if record.get('event') == 'timing': observations.append(record)
                    except ValueError: pass
            report['guest_timings'] = observations
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__': main()
