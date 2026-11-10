#!/usr/bin/env python3
"""Exercise the read-only publication boundary and real dumb-HTTP Git clones."""
import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import subprocess
import threading
from serve_repository import RepositoryServer, load_repository


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repository', type=Path, required=True)
    p.add_argument('--output-root', type=Path, required=True)
    a = p.parse_args()
    out = a.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    manifest = json.loads(a.repository.read_text())
    report = dict(state='failed', checks=[], manifest_sha256=hashlib.sha256(a.repository.read_bytes()).hexdigest())
    server = RepositoryServer(('127.0.0.1', 0), a.repository)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    port = server.server_port

    def request(path, method='GET', headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        try:
            connection.request(method, path, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def check(name, condition):
        assert condition, name
        report['checks'].append(name)
        print('PASS', name, flush=True)

    try:
        status, headers, body = request('/health')
        check('health exposes Coast identity and every published package entry',
              status == 200 and json.loads(body)['name'] == 'Coast Packages' and
              json.loads(body)['packages'] == len(manifest['packages'])+len(manifest['recipes']))
        status, headers, body = request('/')
        check('catalog has local assets and a restrictive content policy', status == 200 and b'Coast Packages' in body and "default-src 'none'" in headers['Content-Security-Policy'])
        data = json.loads(request('/api/v1/search?q=g%2B%2B')[2])
        check('literal G++ search maps to the compiler package', [p['Name'] for p in data['results']] == ['gcc'])
        data = json.loads(request('/api/v1/search?q=g')[2])
        check('one-letter search includes GCC and Git', {'gcc', 'git'} <= {p['Name'] for p in data['results']})
        check('one-letter search puts GCC first and omits descriptive Qt matches',
              data['results'][0]['Name'] == 'gcc' and 'qt6-demo' not in {p['Name'] for p in data['results']})
        check('regex metacharacters remain literal', json.loads(request('/api/v1/search?q=.%2A')[2])['resultcount'] == 0)
        check('source filter returns exactly three recipes', json.loads(request('/api/v1/search?kind=source')[2])['resultcount'] == 3)
        check('API does not disclose host paths or private key directories', str(Path.home()).encode() not in request('/api/v1/search')[2])
        for path in ['/../etc/passwd', '/%2e%2e/etc/passwd', '/repo/', '/keys/private-keys-v1.d/', '/BUILD.json', '/.git/config']:
            check('unpublished path rejected: ' + path, request(path)[0] in (400, 404))
        check('untrusted Host header is rejected', request('/health', headers={'Host': 'evil.invalid'})[0] == 403)
        check('cross-site fetch is rejected', request('/health', headers={'Sec-Fetch-Site': 'cross-site'})[0] == 403)
        for method in ['POST', 'PUT', 'DELETE', 'PATCH']:
            check('writes rejected: ' + method, request('/recipes/index.json', method)[0] == 405)
        route = '/recipes/index.json'
        status, headers, body = request(route)
        check('signed catalog bytes match the build receipt', status == 200 and hashlib.sha256(body).hexdigest() == manifest['host_files'][route]['sha256'])
        check('HEAD reports the same length without a body', request(route, 'HEAD')[1]['Content-Length'] == str(len(body)) and request(route, 'HEAD')[2] == b'')
        check('byte ranges preserve exact bytes', request(route, headers={'Range': 'bytes=3-17'})[2] == body[3:18])
        check('invalid range is rejected', request(route, headers={'Range': 'bytes=99999999-'})[0] == 416)
        check('conditional request returns 304', request(route, headers={'If-None-Match': headers['ETag']})[0] == 304)
        recipe = next(p for p in manifest['recipes'] if p['Name'] == 'hello-coast')
        clone = out / 'clone'
        env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null', GIT_TERMINAL_PROMPT='0')
        with (out / 'git.log').open('wb') as log:
            subprocess.run(['git', '-c', 'core.hooksPath=/dev/null', '-c', 'http.proxy=', 'clone', '--no-local',
                            f'http://127.0.0.1:{port}' + recipe['URLPath'], str(clone)], env=env,
                           stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)
        head = subprocess.check_output(['git', '-C', str(clone), 'rev-parse', 'HEAD'], env=env).decode().strip()
        check('real Git clones the static HTTP origin at the signed commit', head == recipe['Commit'])
        # Mutations use a disposable publication fixture, never the real repository.
        fixture = out / 'mutation'
        fixture.mkdir()
        asset = fixture / 'index.json'
        asset.write_bytes(body)
        small = {**manifest, 'host_files': {route: dict(path=str(asset), sha256=hashlib.sha256(body).hexdigest())}}
        receipt = fixture / 'BUILD.json'
        receipt.write_text(json.dumps(small))
        second = RepositoryServer(('127.0.0.1', 0), receipt)
        second_thread = threading.Thread(target=second.serve_forever, daemon=True)
        second_thread.start()
        try:
            asset.write_bytes(body + b'x')
            connection = http.client.HTTPConnection('127.0.0.1', second.server_port, timeout=10)
            connection.request('GET', route)
            response = connection.getresponse()
            check('an asset changed after startup is refused', response.status == 409)
            response.read()
            connection.close()
        finally:
            second.shutdown()
            second.server_close()
            second_thread.join(5)
        try:
            load_repository(receipt)
        except ValueError:
            check('modified bytes fail startup hash verification', True)
        else:
            raise AssertionError('accepted a modified asset')
        small['host_files'][route]['path'] = str(a.repository.resolve())
        receipt.write_text(json.dumps(small))
        try:
            load_repository(receipt)
        except ValueError:
            check('a forged publication path cannot escape the output', True)
        else:
            raise AssertionError('accepted an escaped publication')
        report['state'] = 'passed'
    finally:
        server.shutdown()
        server.server_close()
        thread.join(5)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
