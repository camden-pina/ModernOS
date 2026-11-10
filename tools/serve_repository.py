#!/usr/bin/env python3
"""Read-only Coast package repository for QEMU guests and networked computers.

Serves only the producer's verified public-artifact allowlist. It has no
filesystem browsing, uploads, Git execution, host package installation or key
management endpoint. Private signing keys stay outside the publication tree.
"""
import argparse
import hashlib
import importlib.util
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import ipaddress
import os
from pathlib import Path, PurePosixPath
import re
import ssl
import sys
import threading
from urllib.parse import parse_qs, unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]

_search_path = Path(__file__).resolve().with_name('search.py')
if not _search_path.is_file():
    _search_path = ROOT / 'userland/packages/search.py'
_search_spec = importlib.util.spec_from_file_location('coast_package_search', _search_path)
_search_module = importlib.util.module_from_spec(_search_spec)
_search_spec.loader.exec_module(_search_module)
search = _search_module.search


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def load_repository(manifest_path):
    path = Path(manifest_path).resolve(strict=True)
    data = json.loads(path.read_text())
    if data.get('target_host_executed') is not False or data.get('signing_secret_included') is not False:
        raise ValueError('A confined build with no published signing secrets is required')
    if not data.get('host_files'):
        raise ValueError('Rebuild with host publication support before starting this server')
    assets = {}
    for url, record in data['host_files'].items():
        if not url.startswith(('/repo/', '/recipes/', '/distfiles/', '/keys/')) or '..' in PurePosixPath(url).parts or unquote(url) != url:
            raise ValueError('Invalid public repository path: ' + url)
        asset = (path.parent / record['path']).resolve(strict=True)
        if not asset.is_relative_to(path.parent) or not asset.is_file():
            raise ValueError('Public asset escapes the repository output: ' + url)
        if sha(asset) != record['sha256']:
            raise ValueError('Public asset changed after signing/building: ' + url)
        stat = asset.stat()
        assets[url] = dict(path=asset, sha256=record['sha256'],
                           identity=(stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
    packages = []
    for name, record in data['packages'].items():
        packages.append(dict(Name=name, Version=record['version'], Description=record['description'],
                             Repository='coast', Kind='binary', Provides=record['provides'],
                             License=record['license'],
                             Commands=sorted({PurePosixPath(n).name for n in [*record['files'], *record['aliases']]
                                              if str(PurePosixPath(n).parent) in ('bin', 'usr/bin')})))
    for record in data['recipes']:
        packages.append({key: record[key] for key in ('Name', 'Version', 'Description', 'License', 'Commit')} |
                        dict(Repository='recipes', Kind='source', Provides=[], Commands=[]))
    return dict(manifest=str(path), manifest_sha256=sha(path), assets=assets,
                fingerprint=data['public_signing_fingerprint'], packages=packages)


class RepositoryServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 32

    def __init__(self, address, repository, web_root=None, log=None,
                 public_url=None, allow_clients=None, tls_context=None):
        if not ipaddress.ip_address(address[0]).is_loopback and not public_url:
            raise ValueError('A network listener requires an explicit --public-url')
        if public_url:
            endpoint = urlsplit(public_url)
            if (endpoint.scheme not in ('http', 'https') or not endpoint.hostname or
                endpoint.username or endpoint.password or endpoint.query or endpoint.fragment or
                endpoint.path not in ('', '/') or re.search(r'[\s\\%]', public_url)):
                raise ValueError('public_url must be a plain HTTP(S) origin')
            if (endpoint.scheme == 'https') != bool(tls_context):
                raise ValueError('The public URL scheme must match the listener TLS configuration')
            _ = endpoint.port
        self.public_url = public_url.rstrip('/') if public_url else None
        self.allowed_clients = [ipaddress.ip_network(value, strict=True) for value in (allow_clients or [])]
        self.tls_context = tls_context
        self.repository = load_repository(repository)
        self.log = log
        self.log_lock = threading.Lock()
        self.slots = threading.BoundedSemaphore(16)
        bundled_web = Path(__file__).resolve().parent / 'web'
        default_web = bundled_web if bundled_web.is_dir() else ROOT / 'userland/packages/web'
        self.web = {route: (Path(web_root or default_web) / name).read_bytes()
                    for route, name in [('/', 'index.html'), ('/app.js', 'app.js'), ('/app.css', 'app.css')]}
        super().__init__(address, Handler)
        self.allowed_hosts = {f'{name}:{self.server_port}' for name in ('127.0.0.1', 'localhost', '10.0.2.2')}
        if self.public_url:
            self.allowed_hosts.add(urlsplit(self.public_url).netloc.lower())

    def get_request(self):
        request, address = super().get_request()
        if self.tls_context:
            try:
                # The handshake runs on the bounded worker, never the accept loop.
                request = self.tls_context.wrap_socket(request, server_side=True, do_handshake_on_connect=False)
            except BaseException:
                request.close()
                raise
        return request, address

    def verify_request(self, request, address):
        return not self.allowed_clients or any(ipaddress.ip_address(address[0]) in network
                                               for network in self.allowed_clients)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            try:
                request.settimeout(1)
                request.sendall(b'HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n')
            finally:
                self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        except ssl.SSLError:
            self.shutdown_request(request)
        finally:
            self.slots.release()

    def handle_error(self, request, address):
        if not isinstance(sys.exc_info()[1], ssl.SSLError):
            super().handle_error(request, address)


class Handler(BaseHTTPRequestHandler):
    server_version = 'Coast-Packages/1'
    sys_version = ''
    protocol_version = 'HTTP/1.0'

    def setup(self):
        self.request.settimeout(15)
        super().setup()

    def log_message(self, fmt, *args):
        if self.server.log:
            with self.server.log_lock:
                self.server.log.write(json.dumps(dict(client=self.client_address[0],
                                                      request=self.requestline, message=fmt % args)) + '\n')
                self.server.log.flush()

    def end_headers(self):
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Referrer-Policy', 'no-referrer')
        self.send_header('Content-Security-Policy', "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'")
        super().end_headers()

    def body(self, status, data, content_type='application/json; charset=utf-8'):
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(data)

    def error(self, status, message):
        self.body(status, json.dumps(dict(error=message)).encode() + b'\n')

    def do_GET(self):
        try:
            self.get()
        except (ValueError, UnicodeError):
            self.error(400, 'Invalid request')
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            self.close_connection = True

    do_HEAD = do_GET

    def get(self):
        # Keep DNS-rebinding protection when a specific LAN hostname is enabled.
        hosts = self.headers.get_all('Host', [])
        if len(hosts) != 1 or hosts[0].lower() not in self.server.allowed_hosts or self.headers.get('Sec-Fetch-Site') == 'cross-site':
            self.error(403, 'Use a configured repository address')
            return
        parsed = urlsplit(self.path)
        route = unquote(parsed.path, errors='strict')
        if parsed.scheme or parsed.netloc or len(route) > 4096 or '\0' in route or '..' in PurePosixPath(route).parts:
            self.error(400, 'Invalid request path')
            return
        if route in self.server.web:
            content = 'text/html; charset=utf-8' if route == '/' else 'text/javascript; charset=utf-8' if route.endswith('.js') else 'text/css; charset=utf-8'
            self.body(200, self.server.web[route], content)
            return
        repository = self.server.repository
        if route == '/health':
            self.body(200, json.dumps(dict(name='Coast Packages', status='ready',
                                           packages=len(repository['packages']),
                                           fingerprint=repository['fingerprint'])).encode() + b'\n')
            return
        if route == '/api/v1/search':
            query = parse_qs(parsed.query, max_num_fields=8)
            kind = query.get('kind', ['all'])[0]
            if kind not in ('all', 'binary', 'source'):
                self.error(400, 'Unknown package type')
                return
            matches = search((package for package in repository['packages']
                              if kind == 'all' or package['Kind'] == kind), query.get('q', ['']))
            self.body(200, json.dumps(dict(name='Coast Packages', fingerprint=repository['fingerprint'],
                                           resultcount=len(matches), results=matches)).encode() + b'\n')
            return
        asset = repository['assets'].get(route)
        if not asset:
            self.error(404, 'No public repository artifact at this address')
            return
        try:
            descriptor = os.open(asset['path'], os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_CLOEXEC', 0))
        except OSError:
            self.error(409, 'Repository artifact is unavailable; restart with a complete build')
            return
        with os.fdopen(descriptor, 'rb') as source:
            stat = os.fstat(source.fileno())
            identity = (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)
            if identity != asset['identity']:
                self.error(409, 'Repository artifact changed; rebuild and restart the server')
                return
            etag = '"' + asset['sha256'] + '"'
            if self.headers.get('If-None-Match') == etag and not self.headers.get('Range'):
                self.send_response(304)
                self.send_header('ETag', etag)
                self.end_headers()
                return
            start, end, status = 0, stat.st_size - 1, 200
            requested = self.headers.get('Range')
            if requested:
                match = re.fullmatch(r'bytes=(\d*)-(\d*)', requested)
                if not match or not any(match.groups()) or not stat.st_size:
                    self.error(416, 'Unsatisfiable byte range')
                    return
                if match[1]:
                    start = int(match[1])
                    end = min(int(match[2]), end) if match[2] else end
                else:
                    start = max(0, stat.st_size - int(match[2]))
                if start > end or start >= stat.st_size:
                    self.error(416, 'Unsatisfiable byte range')
                    return
                status = 206
            self.send_response(status)
            self.send_header('Content-Type', 'application/json' if route.endswith('.json') else 'application/octet-stream')
            self.send_header('Content-Length', str(end - start + 1))
            self.send_header('ETag', etag)
            self.send_header('Accept-Ranges', 'bytes')
            self.send_header('Cache-Control', 'no-cache')
            if status == 206:
                self.send_header('Content-Range', f'bytes {start}-{end}/{stat.st_size}')
            self.end_headers()
            if self.command == 'HEAD':
                return
            source.seek(start)
            remaining = end - start + 1
            while remaining:
                block = source.read(min(remaining, 128 * 1024))
                if not block:
                    self.close_connection = True
                    break
                self.wfile.write(block)
                remaining -= len(block)

    def do_POST(self):
        self.error(405, 'This repository is read-only')

    do_PUT = do_DELETE = do_PATCH = do_POST


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, required=True, help='verified BUILD.json from build_local_repository.py')
    parser.add_argument('--config', type=Path, help='server JSON configuration, suitable for a service')
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--request-log', type=Path)
    parser.add_argument('--public-url', help='address used by physical clients, e.g. https://packages.example:8765')
    parser.add_argument('--allow-client', action='append', default=[], help='optional allowed client CIDR; repeat for multiple networks')
    parser.add_argument('--tls-cert', type=Path, help='PEM server certificate/chain')
    parser.add_argument('--tls-key', type=Path, help='PEM private TLS key; keep outside the public snapshot')
    args = parser.parse_args()
    if args.config:
        values = json.loads(args.config.read_text())
        if not isinstance(values, dict) or set(values) - {'bind', 'port', 'public_url', 'allow_clients', 'tls_cert', 'tls_key'}:
            parser.error('Unsupported server configuration fields')
        for key, value in values.items():
            setattr(args, 'allow_client' if key == 'allow_clients' else key, value)
    try:
        if ipaddress.ip_address(args.bind).version != 4:
            parser.error('This listener currently supports IPv4 bind addresses')
    except ValueError:
        parser.error('--bind must be an explicit IPv4 address')
    if type(args.port) is not int or not 0 <= args.port <= 65535:
        parser.error('invalid port')
    if not isinstance(args.allow_client, list) or any(not isinstance(value, str) for value in args.allow_client):
        parser.error('allow_clients must be a list of CIDR strings')
    context = None
    if bool(args.tls_cert) != bool(args.tls_key):
        parser.error('--tls-cert and --tls-key are required together')
    if args.tls_cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.tls_cert, args.tls_key)
    log = args.request_log.open('a') if args.request_log else None
    try:
        server = RepositoryServer((args.bind, args.port), args.repository, log=log,
                                  public_url=args.public_url, allow_clients=args.allow_client, tls_context=context)
    except ValueError as error:
        parser.error(str(error))
    origin = args.public_url or f'{"https" if context else "http"}://127.0.0.1:{server.server_port}'
    print(f'Coast Packages ready: {origin}', flush=True)
    if args.public_url:
        print(f'Coast client: coast server {origin}', flush=True)
    else:
        print(f'Guest repository: http://10.0.2.2:{server.server_port}/repo/x86_64', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if log:
            log.close()


if __name__ == '__main__':
    main()
