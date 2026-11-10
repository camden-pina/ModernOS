#!/usr/bin/env python3
"""Disposable loopback HTTP/TLS/Git fixtures for real guest networking tests.

No external repositories, accounts, or services are modified. QEMU's user
network maps its host address 10.0.2.2 to these loopback-only host listeners.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
import os
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
from urllib.parse import urlsplit, unquote

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'build/web-fixture'


def run(*args, **kwargs):
    return subprocess.check_output([str(arg) for arg in args], stderr=subprocess.STDOUT,
                                   timeout=60, **kwargs)


class WebFixture:
    def __init__(self):
        WORK.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix='fixture-', dir=WORK))
        self.payload = bytes((i * 31 + 7) % 256 for i in range(190123))
        self.sha256 = hashlib.sha256(self.payload).hexdigest()
        self.servers = []
        self.threads = []
        self.requests = []
        self.requests_by_port = []
        self.assets = {}
        self.ranges = []
        self.lock = threading.Lock()
        self.log = (self.directory / 'requests.log').open('w')
        self.repo = self.directory / 'repos/main.git'
        self.repo.parent.mkdir(exist_ok=True)
        run('git', 'init', '--bare', '--initial-branch=main', self.repo)
        run('git', '-C', self.repo, 'config', 'http.receivepack', 'true')
        self.commit('first fixture revision')
        self.git_backend = Path(run('git', '--exec-path').decode().strip()) / 'git-http-backend'
        self.certificates()
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, format, *args):
                with fixture.lock:
                    fixture.log.write(format % args + '\n');
                    fixture.log.flush()

            def body(self):
                if self.headers.get('Transfer-Encoding', '').lower() == 'chunked':
                    result = bytearray()
                    while True:
                        count = int(self.rfile.readline(128).split(b';', 1)[0].strip(), 16)
                        if not count:
                            while self.rfile.readline(4096).strip():
                                pass
                            return bytes(result)

                        if count > 32 * 1024 * 1024 - len(result):
                            raise ValueError('fixture request too large')
                        result.extend(self.rfile.read(count))

                        if self.rfile.read(2) != b'\r\n':
                            raise ValueError('invalid chunk terminator')

                count = int(self.headers.get('Content-Length', '0'))
                if not 0 <= count <= 32 * 1024 * 1024:
                    raise ValueError('fixture request too large')
                return self.rfile.read(count)

            def respond(self, status, body, content_type='text/plain', headers=()):
                self.send_response(status)
                self.send_header('Content-Type', content_type)
                self.send_header('Content-Length', str(len(body)))
                for key, value in headers:
                    self.send_header(key, value)
                self.end_headers()
                if self.command != 'HEAD':
                    self.wfile.write(body)

            def do_HEAD(self):
                self.do_GET()

            def do_GET(self):
                parsed = urlsplit(self.path)
                with fixture.lock:
                    fixture.requests.append((self.command, self.path))
                    fixture.requests_by_port.append(
                        (self.server.server_port, self.command, self.path))

                if parsed.path in fixture.assets:
                    data, content_type = fixture.assets[parsed.path]
                    selected = self.headers.get('Range')
                    if selected:
                        import re

                        match = re.fullmatch(r'bytes=(\d+)-(\d*)', selected)
                        if not match:
                            return self.respond(416, b'')
                        first = int(match[1]);
                        last = min(int(match[2]) if match[2] else len(data) - 1, len(data) - 1)
                        if first > last:
                            return self.respond(416, b'', headers=[('Content-Range', f'bytes */{len(data)}')])
                        with fixture.lock:
                            fixture.ranges.append(
                                (self.server.server_port, parsed.path, first, last))
                        return self.respond(206, data[first:last + 1], content_type,
                                            [('Accept-Ranges', 'bytes'), ('Content-Range', f'bytes {first}-{last}/{len(data)}')])
                    return self.respond(200, data, content_type, [('Accept-Ranges', 'bytes')])

                if parsed.path.startswith('/repos/'):
                    return self.git(parsed)

                if parsed.path == '/ca.pem':
                    return self.respond(200, (fixture.directory / 'ca.pem').read_bytes())
                if parsed.path in ('/large.bin', '/download.bin'):
                    return self.respond(200, fixture.payload, 'application/octet-stream',
                                        [('Content-Disposition', 'attachment; filename="download.bin"')])

                if parsed.path == '/chunked':
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/octet-stream')
                    self.send_header('Transfer-Encoding', 'chunked')
                    self.end_headers()
                    for offset in range(0, len(fixture.payload), 1403):
                        chunk = fixture.payload[offset:offset + 1403]
                        self.wfile.write(f'{len(chunk):x}\r\n'.encode() + chunk + b'\r\n')
                    self.wfile.write(b'0\r\n\r\n');
                    self.wfile.flush()
                    return

                if parsed.path == '/redirect':
                    return self.respond(302, b'', headers=[('Location', '/large.bin')])
                if parsed.path == '/fail':
                    return self.respond(503, b'Intentional fixture failure\n')

                if parsed.path == '/second.html':
                    body = b'<!doctype html><title>Coast second page</title><h1>Second real document</h1>'
                elif parsed.path == '/script.html':
                    body = (b'<!doctype html><html><head><script>'
                            b'var words=["Coast","JavaScript","page"];'
                            b'if (globalThis !== window || self !== window) throw new Error("Incorrect global aliases");'
                            b'var start=performance.now(), origin=performance.timeOrigin;'
                            b'performance.timeOrigin=0;'
                            b'if (performance.timeOrigin!==origin || Math.abs(Date.now()-origin-start)>1000) throw new Error("Incorrect document clock");'
                            b'document.write("<title>"+words.join(" ")+"</title>");'
                            b'</script></head><body><h1>Embedded JavaScript</h1></body></html>')
                elif parsed.path in ('/', '/page.html', '/form'):
                    body = (b'<!doctype html><html><head><title>Coast HTTP page</title>'
                            b'<link rel="stylesheet" href="/style.css"></head><body>'
                            b'<h1>Real HTTP, HTML and CSS</h1><p id="intro">Served from the host fixture.</p>'
                            b'<p><a href="/second.html">Second page</a></p>'
                            b'<p><a href="/download.bin">Download binary</a></p>'
                            b'<form action="/form" method="get"><input name="query" value="native">'
                            b'<input type="submit" value="Submit"></form>'
                            b'<div style="height:1000px">Scrollable content</div></body></html>')
                elif parsed.path == '/style.css':
                    return self.respond(200, b'body { background:#dcebe6; color:#2d3d44; font-family:sans-serif; margin:24px } h1 { color:#347966 }', 'text/css')
                else:
                    return self.respond(404, b'Not found\n')
                return self.respond(200, body, 'text/html; charset=utf-8')

            def do_POST(self):
                parsed = urlsplit(self.path)
                with fixture.lock:
                    fixture.requests.append((self.command, self.path))
                    fixture.requests_by_port.append(
                        (self.server.server_port, self.command, self.path))

                if parsed.path.startswith('/repos/'):
                    return self.git(parsed)

                return self.respond(404, b'Not found\n')

            def git(self, parsed):
                path = unquote(parsed.path[len('/repos'):])
                if '..' in path.split('/') or '\0' in path:
                    return self.respond(400, b'Invalid fixture path\n')
                env = os.environ | {'GIT_PROJECT_ROOT': str(fixture.repo.parent),
                                    'GIT_HTTP_EXPORT_ALL': '1', 'REQUEST_METHOD': self.command,
                                    'PATH_INFO': path, 'QUERY_STRING': parsed.query,
                                    'REMOTE_ADDR': '127.0.0.1',
                                    'CONTENT_TYPE': self.headers.get('Content-Type', '')}
                body = self.body()
                env['CONTENT_LENGTH'] = str(len(body))
                for key, value in self.headers.items():
                    env['HTTP_' + key.upper().replace('-', '_')] = value
                output = run(fixture.git_backend, input=body, env=env)
                head, separator, data = output.partition(b'\r\n\r\n')
                assert separator, output[:300]
                status, headers = 200, []
                for line in head.decode().split('\r\n'):
                    key, value = line.split(':', 1)
                    if key.lower() == 'status':
                        status = int(value.strip().split()[0])
                    elif key.lower() != 'content-length':
                        headers.append((key, value.strip()))

                self.send_response(status)
                for key, value in headers:
                    self.send_header(key, value)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers();
                self.wfile.write(data)

        try:
            self.http = self.listen(Handler)
            self.https = self.listen(Handler, 'good')
            self.wrong_host = self.listen(Handler, 'wrong')
            self.untrusted = self.listen(Handler, 'untrusted')
        except BaseException:
            self.close();
            raise

    def certificates(self):
        run('openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
            '-keyout', self.directory / 'ca.key', '-out', self.directory / 'ca.pem', '-subj', '/CN=Coast fixture CA',
            '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign')

        for name, san in [('good', 'IP:10.0.2.2,DNS:localhost'), ('wrong', 'IP:192.0.2.1')]:
            run('openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes',
                '-keyout', self.directory / (name + '.key'), '-out', self.directory / (name + '.csr'), '-subj', '/CN=Coast fixture')
            extensions = self.directory / (name + '.extensions')
            extensions.write_text(
                'basicConstraints=critical,CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=' + san + '\n')
            run('openssl', 'x509', '-req', '-in', self.directory / (name + '.csr'), '-CA', self.directory / 'ca.pem',
                '-CAkey', self.directory / 'ca.key', '-CAcreateserial', '-out', self.directory / (name + '.pem'),
                '-days', '2', '-extfile', extensions)

        run('openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
            '-keyout', self.directory / 'untrusted.key', '-out', self.directory / 'untrusted.pem',
            '-subj', '/CN=Untrusted Coast fixture', '-addext', 'subjectAltName=IP:10.0.2.2')

    def listen(self, handler, certificate=None):
        server = ThreadingHTTPServer(('127.0.0.1', 0), handler)
        server.daemon_threads = True
        if certificate:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(self.directory / (certificate + '.pem'),
                                    self.directory / (certificate + '.key'))
            server.socket = context.wrap_socket(server.socket, server_side=True)

        thread = threading.Thread(target=server.serve_forever, daemon=True)
        self.servers.append(server);
        self.threads.append(thread);
        thread.start()
        return server.server_address[1]

    def commit(self, text):
        tree = self.directory / 'seed'
        if not (tree / '.git').is_dir():
            tree.mkdir(exist_ok=True)
            run('git', 'init', '--initial-branch=main', tree)
        run('git', '-C', tree, 'config', 'user.name', 'Coast fixture')
        run('git', '-C', tree, 'config', 'user.email', 'fixture@example.invalid')
        (tree / 'document.txt').write_text(text + '\n')
        run('git', '-C', tree, 'add', 'document.txt')
        run('git', '-C', tree, '-c', 'commit.gpgsign=false', 'commit', '--allow-empty', '-m', text)
        run('git', '-C', tree, 'push', str(self.repo), 'HEAD:main')
        self.head = run('git', '-C', tree, 'rev-parse', 'HEAD').decode().strip()

    def url(self, path='', tls=False):
        return f'{"https" if tls else "http"}://10.0.2.2:{self.https if tls else self.http}/{path.lstrip("/")}'

    def close(self):
        for server in self.servers:
            server.shutdown();
            server.server_close()
        for thread in self.threads:
            thread.join(timeout=2)
        self.log.close()
