#!/usr/bin/env python3
"""Verify portable publication, TLS and package-server configuration boundaries."""
import argparse
import http.client
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import threading

from export_repository import export
from serve_repository import RepositoryServer, load_repository

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('coast_client', ROOT/'userland/packages/client.py')
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repository', type=Path, required=True)
    p.add_argument('--output-root', type=Path, required=True)
    p.add_argument('--lan-address', default='127.0.0.1')
    a = p.parse_args()
    out = a.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = dict(state='failed', checks=[])

    def check(name, condition):
        assert condition, name
        report['checks'].append(name)
        print('PASS '+name, flush=True)

    def rejected(name, function):
        try:
            function()
        except (ValueError, OSError, client.Error):
            check(name, True)
        else:
            raise AssertionError(name)

    servers = []

    def start(**kwargs):
        server = RepositoryServer(('127.0.0.1', 0), snapshot/'repository.json', **kwargs)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        servers.append((server, thread))
        return server

    def request(server, host=None, tls=None, path='/health'):
        cls = http.client.HTTPSConnection if tls else http.client.HTTPConnection
        kw = dict(context=tls) if tls else {}
        connection = cls('127.0.0.1', server.server_port, timeout=5, **kw)
        try:
            connection.request('GET', path, headers={'Host':host} if host else {})
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    try:
        exported = out/'exported'
        export(a.repository, exported)
        snapshot = out/'relocated'/'snapshot'
        snapshot.parent.mkdir()
        exported.rename(snapshot)
        verified = load_repository(snapshot/'repository.json')
        check('public snapshot loads after relocation without its build workspace', len(verified['assets']) > 100)
        text = (snapshot/'repository.json').read_text()
        check('snapshot excludes absolute host paths and signing secrets', str(ROOT) not in text and 'GNUPGHOME' not in text and
              not any('private-keys-v1.d' in str(path) for path in snapshot.rglob('*')))
        # Execute the exported standalone Python server, never a guest program.
        with (out/'standalone.log').open('w+') as log:
            process = subprocess.Popen(['python3','-I','-B',str(snapshot/'serve_repository.py'),
                '--repository',str(snapshot/'repository.json'),'--port','0'], cwd=out,
                stdout=subprocess.PIPE, stderr=log, text=True)
            try:
                address = process.stdout.readline().strip().split()[-1]
                parsed = __import__('urllib.parse', fromlist=['urlsplit']).urlsplit(address)
                connection = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=5)
                connection.request('GET','/')
                response = connection.getresponse()
                check('standalone exported server serves its bundled catalog page', response.status==200 and b'Coast Packages' in response.read())
                connection.close()
            finally:
                process.terminate()
                process.wait(timeout=10)
                process.stdout.close()
        rejected('network listeners require an explicit public origin', lambda: RepositoryServer(('0.0.0.0',0), snapshot/'repository.json'))
        server = start(public_url='http://packages.coast.test')
        check('configured network hostname is accepted', request(server, 'packages.coast.test')[0]==200)
        check('unconfigured network hostname is rejected', request(server, 'other.coast.test')[0]==403)
        connection = http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=5)
        connection.putrequest('GET','/health',skip_host=True)
        connection.putheader('Host','packages.coast.test')
        connection.putheader('Host','evil.invalid')
        connection.endheaders()
        response = connection.getresponse()
        check('duplicate Host headers cannot bypass the allowlist', response.status==403)
        response.read()
        connection.close()
        restricted = start(allow_clients=['127.0.0.2/32'])
        rejected('client subnet restrictions reject another address', lambda: request(restricted))
        rejected('invalid client CIDRs fail before listening', lambda: start(allow_clients=['127.0.0.1/24']))

        cert, key = out/'server-cert.pem', out/'server-key.pem'
        with (out/'openssl.log').open('wb') as log:
            subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-noenc','-sha256','-days','2',
                '-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1,IP:10.0.2.2,IP:'+a.lan_address,
                '-keyout',str(key),'-out',str(cert)],stdout=log,stderr=log,check=True)
        key.chmod(0o600)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(cert,key)
        tls = start(public_url='https://localhost', tls_context=context)
        trusted = ssl.create_default_context(cafile=cert)
        check('HTTPS verifies a trusted certificate and serves signed catalog bytes', request(tls,'localhost',trusted,'/recipes/index.json')[0]==200)
        rejected('untrusted TLS certificates are rejected', lambda: request(tls,'localhost',ssl.create_default_context()))

        def wrong_name():
            with socket.create_connection(('127.0.0.1',tls.server_port),timeout=5) as connection:
                with trusted.wrap_socket(connection,server_hostname='wrong.invalid'):
                    pass
        rejected('TLS certificate hostname mismatch is rejected', wrong_name)
        rejected('an HTTPS URL cannot label a plaintext listener', lambda: start(public_url='https://localhost'))

        for value in ['http://user:pass@host', 'http://host/path', 'http://host/?q=1', 'file:///tmp',
                      'http://host:0', 'http://host:65536', 'http://host;id', 'http://$(id)', 'http://host\nX=Y']:
            rejected('unsafe client origin rejected: '+repr(value), lambda value=value: client.server_origin(value))
        check('valid DNS and IPv4 origins normalize', client.server_origin('https://Packages.Example:8765/')=='https://packages.example:8765' and
              client.server_origin('http://192.168.1.50:8765')=='http://192.168.1.50:8765')
        fixture = out/'settings'
        (fixture/'etc/coast').mkdir(parents=True)
        settings = fixture/'etc/coast/packages.json'
        pacman = fixture/'etc/pacman.conf'
        makepkg = fixture/'etc/makepkg.conf'
        settings.write_text(json.dumps(dict(version=1,url='http://old:8765',signing_fingerprint='A'*40)))
        pacman.write_text('[options]\nSigLevel = Required\n[coast]\nServer = http://old:8765/repo/$arch\n\n[unrelated]\nServer = https://unchanged.example/repo\n')
        makepkg.write_text("CARCH=x86_64\nexport COAST_REPOSITORY_URL='http://old:8765'\n")
        client.apply_server('https://new.example:8765', fixture, os.getuid())
        check('all three client settings switch while the trusted key and signature policy stay intact',
              json.loads(settings.read_text())['url']=='https://new.example:8765' and
              json.loads(settings.read_text())['signing_fingerprint']=='A'*40 and
              'Server = https://new.example:8765/repo/$arch' in pacman.read_text() and
              'SigLevel = Required' in pacman.read_text() and
              'Server = https://unchanged.example/repo' in pacman.read_text() and
              "export COAST_REPOSITORY_URL='https://new.example:8765'" in makepkg.read_text())
        before = [path.read_bytes() for path in [settings,pacman,makepkg]]
        replace = client.os.replace
        failed = False

        def fail_once(source,destination):
            nonlocal failed
            if destination==pacman and not failed:
                failed=True
                raise OSError('injected write failure')
            return replace(source,destination)
        client.os.replace = fail_once
        try:
            rejected('a failed settings update reports the error', lambda: client.apply_server('http://third:8765',fixture,os.getuid()))
        finally:
            client.os.replace = replace
        check('ordinary update failure restores every original settings byte and removes temporary files',
              [path.read_bytes() for path in [settings,pacman,makepkg]]==before and not list(fixture.rglob('.coast-server-*')) and
              not (fixture/'etc/coast/.server-update.lock').exists())
        pacman.unlink()
        pacman.symlink_to(makepkg)
        rejected('root settings helper refuses symlink destinations', lambda: client.apply_server('http://third:8765',fixture,os.getuid()))
        report['state']='passed'
        report['tls_cert']=str(cert)
        report['tls_key']=str(key)
        report['portable_snapshot']=str(snapshot)
    finally:
        for server,thread in servers:
            server.shutdown()
            server.server_close()
            thread.join(5)
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':
    main()
