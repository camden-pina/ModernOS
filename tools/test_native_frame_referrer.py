#!/usr/bin/env python3
"""Check WebCore referrer policy survives an isolated-frame navigation."""
import argparse
from http.server import BaseHTTPRequestHandler
import json
from pathlib import Path
import shlex
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def exercise(iso, cpus, accel, label):
    fixture = WebFixture(webcore=True);
    vm = None;
    requests = []

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        def log_message(self, *args): pass

        def handle(self):
            try:
                super().handle()
            except (ConnectionResetError, BrokenPipeError):
                pass

        def do_GET(self):
            referrer = self.headers.get('Referer', '');
            requests.append((self.path, referrer))
            body = ('''<!doctype html><meta charset=utf-8><body style="margin:0;background:#11aa33"><script>
parent.postMessage({referrer:document.referrer,http:HTTP_REFERRER},'*');
</script>'''.replace('HTTP_REFERRER', json.dumps(referrer))).encode()
            self.send_response(200);
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)));
            self.end_headers();
            self.wfile.write(body)

    try:
        port = fixture.listen(Handler, 'good');
        child = f'https://10.0.2.2:{port}'
        vm = VM(cpus, storage=False, iso=iso, label=label, accel=accel)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem')))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(label + '-start');

        window = d.launch('Browser')

        for policy in ('strict-origin-when-cross-origin', 'no-referrer', 'unsafe-url'):
            url = fixture.url('frame-referrer?policy=' + policy, tls=True)
            expected = '' if policy == 'no-referrer' else url if policy == 'unsafe-url' else fixture.url(
                '', tls=True)
            main = '''<!doctype html><meta charset=utf-8><title>Referrer loading</title><body style="margin:0">
<iframe id=f referrerpolicy="POLICY" style="position:absolute;left:20px;top:60px;width:200px;height:120px;border:0" src="CHILD"></iframe><script>
onmessage=e=>{if(e.source!==f.contentWindow||e.origin!==ORIGIN)return;
 if(e.data.referrer!==EXPECTED||e.data.http!==EXPECTED){console.error('REFERRER_MISMATCH '+JSON.stringify(e.data));document.title='Referrer failed';}
 else document.title='Referrer ready POLICY';};
</script>'''.replace('POLICY', policy).replace('CHILD', child + '/frame?policy=' + policy).replace('ORIGIN', repr(child)).replace('EXPECTED', repr(expected))
            fixture.assets['/frame-referrer'] = main.encode(), 'text/html; charset=utf-8'
            d.key('l', 'ctrl');
            vm.send(url + '\n')

            try:
                d.expect('BROWSER_TITLE', 'Referrer ready ' + policy)
            except AssertionError:
                d.screenshot(label + '-failed');
                raise

            assert ('/frame?policy=' + policy, expected) in requests, requests

        d.key('f4', 'alt');
        d.expect('CLOSE');
        d.exit()
        log = Path(vm.log.name).read_text(errors='replace')
        assert 'GLib-CRITICAL' not in log and 'BROWSER_FORCED_CLOSE' not in log

        print(f'PASS isolated referrers: actual DOM/HTTP referrers, strict origin serialization, '
              f'no-referrer and explicitly requested unsafe-url ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True);
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='native-frame-referrer')
    args = parser.parse_args();
    exercise(args.iso, args.cpus, args.accel, args.label)
