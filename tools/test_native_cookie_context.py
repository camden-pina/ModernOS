#!/usr/bin/env python3
"""Check DOM and HTTP cookies use the real isolated frame's site context."""
import argparse
from http.server import BaseHTTPRequestHandler
import json
from pathlib import Path
import shlex
from urllib.parse import urlsplit, parse_qs
from test_runtime import VM, assert_success
from test_desktop import DesktopTest
from web_fixture import WebFixture, run


def exercise(iso, cpus, accel, label):
    fixture = WebFixture(webcore=True)
    vm = None
    observations = []
    foreign_host = 'cookie.example.test'
    child_script = '''
function names(value){return value.split(';').map(x=>x.trim().split('=')[0]).filter(Boolean).sort();}
const errors=[];function check(value,why){if(!value)errors.push(why);}
(async()=>{
 const prefix=PREFIX,allowed=ALLOWED;
 const before=names(document.cookie);
 check(JSON.stringify(before)===JSON.stringify(allowed),'DOM initial '+JSON.stringify(before));
 document.cookie=prefix+'_dom=written; Path=/; SameSite=None; Secure';
 const response=await fetch('/echo?mode='+MODE).then(r=>r.json());
 const http=names(response.cookie),expected=allowed.concat(prefix+'_hidden_none');
 if(MODE==='same')expected.push(prefix+'_hidden_lax',prefix+'_dom');
 expected.sort();
 check(JSON.stringify(http)===JSON.stringify(expected),'HTTP cookies '+JSON.stringify(http));
 const after=names(document.cookie),finalExpected=allowed.slice();
 if(MODE==='same')finalExpected.push(prefix+'_dom',prefix+'_net');
 finalExpected.sort();
 check(JSON.stringify(after)===JSON.stringify(finalExpected),'DOM after '+JSON.stringify(after));
 if(errors.length){parent.postMessage({error:errors.join('; '),before,http,after},'*');return;}
 document.body.style.background='#11aa33';
 parent.postMessage({ready:true,before,http,after},'*');
})().catch(e=>{console.error(e);parent.postMessage({error:String(e)},'*');});
'''

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        def log_message(self, *args): pass

        def handle(self):
            try:
                super().handle()
            except (ConnectionResetError, BrokenPipeError):
                pass

        def do_GET(self):
            parts = urlsplit(self.path)
            mode = parse_qs(parts.query).get('mode', [''])[0]
            prefix = 'foreign' if self.headers.get('Host', '').startswith(foreign_host) else 'same'
            headers = []
            content_type = 'text/html; charset=utf-8'

            if parts.path == '/seed':
                for suffix, attributes in (
                    ('lax', 'SameSite=Lax'), ('strict', 'SameSite=Strict'),
                    ('none', 'SameSite=None'), ('hidden_lax', 'SameSite=Lax; HttpOnly'),
                        ('hidden_none', 'SameSite=None; HttpOnly')):
                    headers.append(
                        ('Set-Cookie', f'{prefix}_{suffix}=seed; Path=/; Secure; {attributes}'))
                body = f'<title>Cookie seed ready {prefix}</title>'.encode()
            elif parts.path == '/echo':
                cookie = self.headers.get('Cookie', '')
                observations.append((mode, cookie))
                headers.append(
                    ('Set-Cookie', f'{prefix}_net=network; Path=/; SameSite=None; Secure'))
                body = json.dumps({'cookie': cookie}).encode()
                content_type = 'application/json'
            elif parts.path == '/child':
                allowed = ([prefix + '_lax', prefix + '_strict', prefix + '_none']
                           if mode == 'same' else [prefix + '_none'])
                script = child_script.replace('PREFIX', json.dumps(prefix)).replace(
                    'ALLOWED', json.dumps(sorted(allowed))).replace('MODE', json.dumps(mode))
                body = ('<!doctype html><meta charset=utf-8><body style="margin:0;background:white"><script>' + script + '</script>').encode()
            else:
                child = same_origin if mode == 'same' else foreign_origin
                body = ('''<!doctype html><meta charset=utf-8><title>Cookie context loading</title>
<body style="margin:0"><iframe id=f style="position:absolute;left:20px;top:60px;width:300px;height:160px;border:0" src="CHILD/child?mode=MODE"></iframe>
<script>onmessage=e=>{if(e.source!==f.contentWindow||e.origin!==ORIGIN)return;
 console.log('COOKIE_CONTEXT '+JSON.stringify(e.data));
 document.title=e.data.ready?'Cookie context ready MODE':'Cookie context failed';};</script>''').replace(
                    'CHILD', child).replace('MODE', mode).replace('ORIGIN', json.dumps(child)).encode()

            self.send_response(200)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            for key, value in headers:
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)

    try:
        run('openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes',
            '-keyout', fixture.directory / 'cookie.key', '-out', fixture.directory / 'cookie.csr',
            '-subj', '/CN=ModernOS cookie fixture')
        extensions = fixture.directory / 'cookie.extensions'
        extensions.write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\n'
                              'extendedKeyUsage=serverAuth\nsubjectAltName=IP:10.0.2.2,DNS:' + foreign_host + '\n')
        run('openssl', 'x509', '-req', '-in', fixture.directory / 'cookie.csr', '-CA', fixture.directory / 'ca.pem',
            '-CAkey', fixture.directory / 'ca.key', '-CAcreateserial', '-out', fixture.directory / 'cookie.pem',
            '-days', '2', '-extfile', extensions)
        main_port = fixture.listen(Handler, 'cookie')
        child_port = fixture.listen(Handler, 'cookie')
        main_origin = f'https://10.0.2.2:{main_port}'
        same_origin = f'https://10.0.2.2:{child_port}'
        foreign_origin = f'https://{foreign_host}:{child_port}'
        vm = VM(cpus, storage=False, iso=iso, label=label, accel=accel)
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            'i=0; while test ! -s /etc/resolv.conf && test "$i" -lt 30; do '
            'sleep 1; i=$((i+1)); done; test -s /etc/resolv.conf'), timeout=45))
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            f'curl -fsS --max-time 30 {fixture.url("ca.pem")} >> /etc/ssl/cert.pem')))
        assert_success(vm.command('build-sh -c ' + shlex.quote(
            f"printf '10.0.2.2 {foreign_host}\\n' >> /etc/hosts")))
        vm.send('gui --trace --stats\n');
        vm.until(b'GUI_READY', 40)
        d = DesktopTest(vm);
        d.screenshot(label + '-start');

        window = d.launch('Browser')

        for origin, prefix in ((foreign_origin, 'foreign'), (main_origin, 'same')):
            d.key('l', 'ctrl');
            vm.send(origin + '/seed\n')
            d.expect('BROWSER_TITLE', 'Cookie seed ready ' + prefix)

        for mode in ('same', 'cross'):
            d.key('l', 'ctrl');
            vm.send(main_origin + '/main?mode=' + mode + '\n')
            try:
                d.expect('BROWSER_TITLE', 'Cookie context ready ' + mode)
            except AssertionError:
                d.screenshot(label + '-failed');
                raise

            pixels = d.screenshot(label + '-' + mode)
            left = (d.width - 840) // 2 + (window - 1) % 5 * 16
            top = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16 + 40
            at = ((top + 150) * d.width + left + 200) * 3
            assert pixels[at:at + 3] == bytes((17, 170, 51)
                                              ), 'Cookie-context document was not rendered'

        assert {mode for mode, _ in observations} == {'same', 'cross'}, observations
        d.key('f4', 'alt');
        d.expect('CLOSE');
        d.exit()
        log = Path(vm.log.name).read_text(errors='replace')
        for marker in ('PANIC:', 'GLib-CRITICAL', 'BROWSER_FORCED_CLOSE', 'Uncaught error in JS:'):
            assert marker not in log, marker

        print(f'PASS native cookie contexts: real same-site/cross-site isolated DOM and HTTP cookies, '
              f'SameSite, existing third-party policy and HttpOnly ({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='native-cookie-context')
    args = parser.parse_args();
    exercise(args.iso, args.cpus, args.accel, args.label)
