#!/usr/bin/env python3
"""Check real GUI navigation provenance reaches WebCore's Fetch Metadata."""
import argparse
from http.server import BaseHTTPRequestHandler
import json
from pathlib import Path
import shlex
from urllib.parse import urlsplit

from test_runtime import VM, assert_success, WORK
from test_desktop import DesktopTest
from web_fixture import WebFixture, run


def exercise(iso, cpus, accel, label):
    fixture = WebFixture(webcore=True)
    vm = None
    observations = []
    foreign_host = 'metadata.example.test'

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *args): pass

        def handle(self):
            try:
                super().handle()
            except (ConnectionResetError, BrokenPipeError):
                pass

        def do_GET(self):
            path = urlsplit(self.path).path
            item = {'host': self.headers.get('Host', ''), 'path': self.path,
                    'site': self.headers.get('Sec-Fetch-Site', ''),
                    'mode': self.headers.get('Sec-Fetch-Mode', ''),
                    'dest': self.headers.get('Sec-Fetch-Dest', ''),
                    'user': self.headers.get('Sec-Fetch-User', ''),
                    'ua': self.headers.get('User-Agent', '')}
            observations.append(item)
            script = ''
            title = 'Metadata ready ' + path
            if path == '/main':
                title = 'Metadata loading'
                script = '''
const child = new Promise(resolve => addEventListener('message',e=>{
 if(e.origin===FOREIGN && e.data==='metadata child')resolve();}));
Promise.all([child,fetch('/fetch'),fetch(FOREIGN+'/foreign-fetch')]).then(()=>{
 document.title='Metadata ready '+location.search;
}).catch(e=>{console.error(e);document.title='Metadata failed';});
'''.replace('FOREIGN', json.dumps(foreign_origin))
                body = ('<!doctype html><meta charset=utf-8><title>' + title +
                        '</title><body style="margin:0;background:#11aa33"><iframe style="position:absolute;left:20px;top:60px;width:200px;height:120px;border:0" src="' +
                        foreign_origin + '/frame"></iframe><script>' + script + '</script>').encode()
            else:
                if path == '/frame':
                    script = "parent.postMessage('metadata child','*');"
                elif path == '/same-script-start':
                    title = 'Metadata script loading'
                    script = "setTimeout(()=>location.href='/same-script-target',100);"
                elif path == '/cross-script-start':
                    title = 'Metadata script loading'
                    script = 'setTimeout(()=>location.href=' + json.dumps(foreign_origin + '/cross-script-target') + ',100);'
                elif path == '/script-reload-start':
                    title = 'Metadata reload loading'
                    script = "sessionStorage.removeItem('metadataReload');setTimeout(()=>location.href='/script-reload-target',100);"
                elif path == '/script-reload-target':
                    title = 'Metadata reload loading'
                    script = "if(!sessionStorage.getItem('metadataReload')){sessionStorage.setItem('metadataReload','1');setTimeout(()=>location.reload(),100);}else document.title='Metadata script reload ready';"
                body = ('<!doctype html><meta charset=utf-8><title>' + title +
                        '</title><body style="margin:0;background:#11aa33"><script>' + script + '</script>').encode()

            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(body)

    try:
        run('openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes',
            '-keyout', fixture.directory / 'metadata.key', '-out', fixture.directory / 'metadata.csr',
            '-subj', '/CN=ModernOS metadata fixture')
        extensions = fixture.directory / 'metadata.extensions'
        extensions.write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\n'
                              'extendedKeyUsage=serverAuth\nsubjectAltName=IP:10.0.2.2,DNS:' + foreign_host + '\n')
        run('openssl', 'x509', '-req', '-in', fixture.directory / 'metadata.csr', '-CA', fixture.directory / 'ca.pem',
            '-CAkey', fixture.directory / 'ca.key', '-CAcreateserial', '-out', fixture.directory / 'metadata.pem',
            '-days', '2', '-extfile', extensions)
        main_port = fixture.listen(Handler, 'metadata')
        child_port = fixture.listen(Handler, 'metadata')
        main_origin = f'https://10.0.2.2:{main_port}'
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

        def navigate(path, title):
            d.key('l', 'ctrl');
            vm.send(main_origin + path + '\n')
            d.expect('BROWSER_TITLE', title)

        for step in ('initial', 'same-ui'):
            navigate('/main?' + step, 'Metadata ready ?' + step)
        navigate('/same-script-start', 'Metadata ready /same-script-target')
        navigate('/script-reload-start', 'Metadata script reload ready')
        d.key('f5');
        d.expect('BROWSER_TITLE', 'Metadata script reload ready')
        navigate('/cross-script-start', 'Metadata ready /cross-script-target')
        d.key('f5');
        d.expect('BROWSER_TITLE', 'Metadata ready /cross-script-target')
        pixels = d.screenshot(label + '-final')
        left = (d.width - 840) // 2 + (window - 1) % 5 * 16
        top = (d.height - 42 - 560) // 2 + (window - 1) % 5 * 16 + 40
        at = ((top + 220) * d.width + left + 300) * 3
        assert pixels[at:at + 3] == bytes((17, 170, 51)), 'Metadata document did not render'
        d.key('f4', 'alt');
        d.expect('CLOSE');
        d.exit()

        def entries(path): return [item for item in observations if item['path'] == path]

        failures = []

        def check(path, sites, mode='navigate', dest='document'):
            found = entries(path)
            actual = [(item['site'], item['mode'], item['dest']) for item in found]
            expected = [(site, mode, dest) for site in sites]
            if actual != expected:
                failures.append({'path': path, 'actual': actual, 'expected': expected})

        check('/main?initial', ['none'])
        check('/main?same-ui', ['none'])
        check('/frame', ['cross-site', 'cross-site'], dest='iframe')
        check('/fetch', ['same-origin', 'same-origin'], mode='cors', dest='empty')
        check('/foreign-fetch', ['cross-site', 'cross-site'], mode='cors', dest='empty')
        check('/same-script-target', ['same-origin'])
        check('/script-reload-target', ['same-origin', 'same-origin', 'none'])
        check('/cross-script-target', ['cross-site', 'none'])
        uas = {item['ua'] for item in observations}
        if len(uas) != 1 or not next(iter(uas)):
            failures.append({'user_agent_count': len(uas)})
        log = Path(vm.log.name).read_text(errors='replace')
        for marker in ('PANIC:', 'GLib-CRITICAL', 'BROWSER_FORCED_CLOSE', 'Uncaught error in JS:'):
            if marker in log:
                failures.append({'unexpected_log_marker': marker})
        report = {'iso': str(iso), 'cpus': cpus, 'accel': accel,
                  'requests': observations, 'failures': failures, 'passed': not failures}
        with (WORK / (label + '-result.json')).open('x') as output:
            output.write(json.dumps(report, indent=2) + '\n')

        assert not failures, json.dumps(failures, indent=2)

        print(f'PASS native Fetch Metadata: actual HTTPS headers for fresh/same-origin GUI navigation, '
              f'foreign iframe/fetch, same/cross-site scripts, script/toolbar reload and stable UA '
              f'({cpus} CPUs, {accel})', flush=True)
    finally:
        if vm:
            vm.close()
        fixture.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True)
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--label', default='native-fetch-metadata')
    args = parser.parse_args();
    exercise(args.iso, args.cpus, args.accel, args.label)
