#!/usr/bin/env python3
"""Validate real HTTPS timings, compression, language headers and timing privacy."""
import argparse
import gzip
from http.server import BaseHTTPRequestHandler
import json
from pathlib import Path
import re
import shlex
import time
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture


def exercise(iso, cpus, accel, label):
    fixture = WebFixture(webcore=True)
    vm = None
    payload = b'0123456789abcdefghijklmnopqrstuvwxyz' * 1000
    compressed = gzip.compress(payload, mtime=0)
    private_cookie = 'native_transport_cookie_DO_NOT_LOG_' + ('a.b_C-123' * 48)
    page = '''<!doctype html><meta charset=utf-8><title>Transport loading</title><body style="margin:0;background:#ffffff"><script>
function check(value,why){if(!value)throw Error(why);}
(async()=>{
 const url=location.origin+'/slow';const started=performance.now();
 const text=await fetch(url).then(r=>r.text());
 check(text.length===DECODED,'gzip decoding');
 const elapsed=performance.now()-started;
 await fetch(CROSS+'/private',{mode:'no-cors'});
 await fetch(CROSS+'/public');
 await fetch('/redirect-private',{mode:'no-cors'});
 await fetch(CROSS+'/redirect-public',{mode:'no-cors'});
 await fetch(CROSS+'/redirect-approved',{mode:'no-cors'});
 const identity=await fetch('/identity').then(r=>r.json());
 const explicit=await fetch('/identity',{headers:{'Accept-Language':'fr'}}).then(r=>r.json());
 if(document.readyState!=='complete')await new Promise(r=>addEventListener('load',r,{once:true}));
 await new Promise(r=>setTimeout(r,50));
 const entry=performance.getEntriesByName(url).find(e=>e.entryType==='resource');
 console.log('TRANSPORT_TIMING '+JSON.stringify({elapsed,start:entry&&entry.requestStart,response:entry&&entry.responseStart,end:entry&&entry.responseEnd,encoded:entry&&entry.encodedBodySize,decoded:entry&&entry.decodedBodySize,protocol:entry&&entry.nextHopProtocol}));
 check(entry&&entry.requestStart>0&&entry.responseStart-entry.requestStart>=140&&entry.responseEnd>=entry.responseStart,'missing real request/response timings');
 check(elapsed>=140&&entry.duration>=140,'delayed HTTP timing');
 check(entry.nextHopProtocol==='http/1.1','actual HTTP protocol');
 check(entry.encodedBodySize===ENCODED&&entry.decodedBodySize===DECODED,'encoded/decoded transfer lengths');
 const hidden=performance.getEntriesByName(CROSS+'/private').find(e=>e.entryType==='resource');
 const exposed=performance.getEntriesByName(CROSS+'/public').find(e=>e.entryType==='resource');
 console.log('TRANSPORT_TAO '+JSON.stringify({hidden:hidden&&[hidden.requestStart,hidden.responseStart,hidden.nextHopProtocol,hidden.decodedBodySize],exposed:exposed&&[exposed.requestStart,exposed.responseStart,exposed.nextHopProtocol,exposed.decodedBodySize]}));
 check(hidden&&hidden.requestStart===0&&hidden.responseStart===0&&hidden.nextHopProtocol===''&&hidden.encodedBodySize===0&&hidden.decodedBodySize===0,'cross-origin timing data exposed without TAO');
 // WebCore intentionally keeps cross-origin byte counts private even with
 // TAO; the approved origin exposes timings and protocol, not those counts.
 check(exposed&&exposed.requestStart>0&&exposed.responseStart>=exposed.requestStart&&exposed.nextHopProtocol==='http/1.1'&&exposed.encodedBodySize===0&&exposed.decodedBodySize===0,'TAO-approved timing unavailable');
 for(const address of [location.origin+'/redirect-private',CROSS+'/redirect-public']){
  const redirect=performance.getEntriesByName(address).find(e=>e.entryType==='resource');
  check(redirect&&redirect.requestStart===0&&redirect.responseStart===0&&redirect.redirectStart===0&&redirect.nextHopProtocol==='','redirect timing exposed after failed TAO');
 }
 const approved=performance.getEntriesByName(CROSS+'/redirect-approved').find(e=>e.entryType==='resource');
 check(approved&&approved.requestStart>0&&approved.redirectStart>0&&approved.nextHopProtocol==='http/1.1','approved redirect timing missing');
 check(identity.ua===navigator.userAgent&&identity.language===navigator.language,'HTTP/JavaScript identity mismatch');
 check(identity.cookie==='trace_integrity='+COOKIE_VALUE,'complete HTTP cookie value changed');
 check(!document.cookie.includes('trace_integrity'),'HttpOnly transport cookie exposed');
 check(explicit.language==='fr','explicit language header replaced');
 const nav=performance.getEntriesByType('navigation')[0];
 check(nav&&nav.requestStart>0&&nav.responseStart>=nav.requestStart&&nav.nextHopProtocol==='http/1.1','navigation timing missing');
 let rejected=false;try{await fetch(UNTRUSTED);}catch(e){rejected=true;}
 check(rejected,'untrusted HTTPS certificate accepted');
 document.body.style.background='#11aa33';document.title='Transport ready';
})().catch(e=>{console.error(e);document.title='Transport failed';});
</script>'''

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *args):
            pass

        def handle(self):
            try:
                super().handle()
            except (ConnectionResetError, BrokenPipeError):
                pass

        def do_GET(self):
            headers = [];
            content_type = 'text/plain';
            body = b'public'

            if self.path.startswith('/redirect-'):
                self.send_response(302)
                destination = f'https://10.0.2.2:{cross}/private' if self.path == '/redirect-private' else f'https://10.0.2.2:{main}/public'
                self.send_header('Location', destination);
                self.send_header('Content-Length', '0')
                if self.path == '/redirect-approved':
                    self.send_header('Timing-Allow-Origin', '*')
                self.end_headers();
                return

            if self.path == '/main':
                body = page.encode();
                content_type = 'text/html; charset=utf-8'
                headers.append(
                    ('Set-Cookie', f'trace_integrity={private_cookie}; Path=/; Secure; HttpOnly; SameSite=Lax'))
            elif self.path == '/slow':
                time.sleep(.2);
                body = compressed;
                headers.append(('Content-Encoding', 'gzip'))
            elif self.path == '/public':
                headers.extend([('Access-Control-Allow-Origin', '*'), ('Timing-Allow-Origin', '*')])
            elif self.path == '/identity':
                body = json.dumps({'ua': self.headers.get('User-Agent', ''), 'language': self.headers.get('Accept-Language', ''),
                                   'cookie': self.headers.get('Cookie', '')}).encode()
                content_type = 'application/json'

            self.send_response(200);
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            for key, value in headers:
                self.send_header(key, value)
            self.end_headers();
            self.wfile.write(body)

    try:
        main = fixture.listen(Handler, 'good');
        cross = fixture.listen(Handler, 'good')
        page = page.replace('DECODED', str(len(payload))).replace('ENCODED', str(len(compressed))).replace(
            'CROSS', repr(f'https://10.0.2.2:{cross}')).replace('UNTRUSTED', repr(fixture.url('page.html', tls=True).replace(
                ':' + str(fixture.https) + '/', ':' + str(fixture.untrusted) + '/'))).replace('COOKIE_VALUE', repr(private_cookie))
        vm = VM(cpus, storage=False, iso=iso, label=label, accel=accel)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem')))
        vm.send('gui --trace --stats --browser-network-trace\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(label + '-start');

        window = d.launch('Browser')
        d.key('l', 'ctrl');
        vm.send(f'https://10.0.2.2:{main}/main\n')

        try:
            d.expect('BROWSER_TITLE', 'Transport ready')
        except AssertionError:
            d.screenshot(label + '-failed');
            raise

        pixels = d.screenshot(label + '-ready')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16;
        top = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16 + 40
        at = ((top + 80) * d.width + left + 80) * 3
        assert pixels[at:at + 3] == bytes((17, 170, 51)), 'Transport document was not rendered'
        d.key('f4', 'alt');
        d.expect('CLOSE');
        d.exit()
        log = Path(vm.log.name).read_text(errors='replace')
        integrity = [line for line in log.splitlines() if 'HTTP cookie integrity:' in line]
        assert integrity and all('matches_jar=1' in line for line in integrity), integrity
        assert any(
            'selected_count=1' in line for line in integrity), 'Stored cookie was not checked against the outgoing header'
        assert private_cookie not in log, 'Transport diagnostics printed a cookie value'
        assert re.search(
            r'HTTP response diagnostics: id=\d+ status=200 http=\[http/1.1\] tls=\[tls-1-3\] certificate_errors=0', log), 'Actual TLS/HTTP metadata missing'

        skew = [int(value) for value in re.findall(r'clock_skew_seconds=(-?\d+)', log)]
        assert skew and all(
            abs(value) <= 5 for value in skew), ('Guest/server clock mismatch', skew)
        for marker in ('PANIC:', 'GLib-CRITICAL', 'BROWSER_FORCED_CLOSE', 'Uncaught error in JS:'):
            assert marker not in log, marker

        print(f'PASS native transport: actual TLS/HTTP timings, gzip byte counts, '
              f'TAO privacy, locale/UA agreement, explicit language, private cookie integrity, clock/response diagnostics and untrusted-cert rejection ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True);
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='native-transport')
    args = parser.parse_args();
    exercise(args.iso, args.cpus, args.accel, args.label)
