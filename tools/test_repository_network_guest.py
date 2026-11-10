#!/usr/bin/env python3
"""Download signed packages, Git and sources through a TCP/TLS fixture."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import ssl
import threading
import traceback

from serve_repository import RepositoryServer
import test_runtime as runtime


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iso', type=Path, required=True)
    p.add_argument('--repository', type=Path, required=True)
    p.add_argument('--output-root', type=Path, required=True)
    p.add_argument('--server-address', required=True)
    p.add_argument('--isolated-network', action='store_true')
    p.add_argument('--tls-cert', type=Path, required=True)
    p.add_argument('--tls-key', type=Path, required=True)
    a = p.parse_args()
    out = a.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime.WORK = out
    report = dict(state='failed', checks=[], iso=str(a.iso.resolve()), server_address=a.server_address,
                  transport='guest NIC + QEMU user network + IPv4 TCP/TLS fixture',
                  isolated_network=a.isolated_network,
                  two_physical_computers_tested=False, target_host_executed=False)
    (out/'HARNESS.py').write_bytes(Path(__file__).read_bytes())
    servers, logs = [], []
    vm = None

    def start(tls=False, repository=None):
        context = None
        if tls:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(a.tls_cert, a.tls_key)
        for _ in range(10):
            with socket.socket() as probe:
                probe.bind((a.server_address,0))
                port = probe.getsockname()[1]
            url = ('https' if tls else 'http')+'://'+a.server_address+':'+str(port)
            log = (out/(('tls' if tls else 'http')+'-'+str(port)+'.jsonl')).open('w')
            try:
                server = RepositoryServer((a.server_address,port), repository or a.repository,
                    public_url=url, tls_context=context, log=log)
                break
            except OSError:
                log.close()
        else:
            raise AssertionError('No unused test listener port')
        thread = threading.Thread(target=server.serve_forever,daemon=True)
        thread.start()
        servers.append((server,thread))
        logs.append(log)
        return url

    def check(name, command, expected=None, user=True, timeout=90, native=False):
        output = vm.command(command, expect=expected, timeout=timeout) if native else \
                 vm.shell_command(command, expect=expected, as_user=user, timeout=timeout)
        (out/(f'{len(report["checks"]):02d}-output.txt')).write_text(output)
        report['checks'].append(name)
        print('PASS '+name,flush=True)
        return output

    def settings():
        return vm.shell_command('sha256sum /etc/coast/packages.json /etc/pacman.conf /etc/makepkg.conf',as_user=True)

    try:
        http = start()
        https = start(tls=True)
        report.update(http=http,https=https)
        vm = runtime.VM(4, storage=False, network=True, memory='16G', accel='kvm',
                        iso=a.iso.resolve(), label='network-repo')
        check('DHCP and resolver configuration becomes available',
              'for i in 1 2 3 4 5 6 7 8 9 10 11 12; do test -s /etc/resolv.conf && break; sleep 1; done; test -s /etc/resolv.conf')
        check('new Coast package helper is installed', 'pacman -Q coast-packages', 'coast-packages 1.3-1')
        original = settings()
        check('LAN endpoint verifies the signed database and source catalog before elevation',
              'coast server '+http+' --check', 'Verified package database and source catalog')
        assert settings()==original
        check('unprivileged callers cannot invoke the root settings operation',
              'python3 -I /usr/lib/coast/packages/client.py _set-server '+http+'; test $? -ne 0', 'requires authenticated sudo')
        vm.command('true')
        check('authenticated mirror switch updates package and source settings', 'coast server '+http,
              'Package downloads and source builds now use '+http)
        check('pacman and makepkg use the same LAN server',
              "grep -F '"+http+"/repo/$arch' /etc/pacman.conf; grep -F '"+http+"' /etc/makepkg.conf", http)
        check('repository settings remain root-owned and not user-writable',
              'printf x >> /etc/coast/packages.json; test $? -ne 0', 'Permission denied')
        check('normal users refresh the signed catalog from the LAN', 'coast refresh', '3 recipes')
        check('pacman downloads and verifies the LAN repository database', 'pacman -Sy --noconfirm', 'Synchronizing',False)
        check('remove calculator and cached archives so reinstall needs network bytes',
              'pacman -R --noconfirm bc && rm -f /var/cache/pacman/pkg/bc-*', 'removing bc',False)
        vm.command('true')
        check('install a signed binary over ordinary LAN HTTP', 'coast install bc --noconfirm','installing bc')
        check('downloaded calculator computes the expected result', "printf '6*7\\n' | bc -q", '42')

        selected = settings()
        check('untrusted HTTPS certificate fails without changing the mirror',
              'coast server '+https+' --check; test $? -ne 0','SSL', timeout=120)
        assert settings()==selected
        # The public QA certificate is trusted only inside this owned guest.
        encoded = __import__('base64').b64encode(a.tls_cert.read_bytes()).decode()
        for offset in range(0,len(encoded),300):
            vm.command(('write' if offset==0 else 'append')+' /tmp/repo-test-ca.b64 '+encoded[offset:offset+300])
        check('install only the disposable fixture CA in the disposable guest',
              'base64 -d /tmp/repo-test-ca.b64 >> /etc/ssl/cert.pem', user=False)
        port = https.rsplit(':',1)[1]
        check('native TLS rejects a certificate hostname mismatch',
              "curl -q --noproxy '*' --resolve wrong.invalid:"+port+':'+a.server_address+
              ' https://wrong.invalid:'+port+'/health -o /tmp/wrong-host; test $? -eq 60', 'certificate')
        vm.command('true')
        check('select HTTPS after validating its certificate and pinned signatures',
              'coast server '+https,'Package downloads and source builds now use '+https)
        check('refresh both signed metadata sets over TLS',
              'coast refresh && sudo pacman -Sy --noconfirm','Synchronizing')
        check('force a new TLS package transfer',
              'pacman -R --noconfirm bc && rm -f /var/cache/pacman/pkg/bc-*','removing bc',False)
        vm.command('true')
        check('install signed calculator archive over certificate-verified TLS',
              'coast install bc --noconfirm','installing bc')
        check('installed TLS download matches package file metadata','pacman -Qkk bc','0 altered files')
        check('clone signed-commit recipe through actual Git HTTPS', 'coast clone bc','Recipe checkout',timeout=120)
        check('makepkg downloads the source archive from the selected TLS server and verifies its checksum',
              'cd /home/user/Builds/bc && makepkg --verifysource', 'Passed',timeout=120)
        check('native GCC builds the real source recipe from downloaded bytes',
              'cd /home/user/Builds/bc && makepkg --noconfirm', 'Finished making',timeout=360)
        vm.command('true')
        check('authenticated installation of the reviewed source-built package',
              'coast install --source bc --reviewed --noconfirm','upgrading bc',timeout=180)
        check('source-built calculator runs without elevation',
              "printf '2^100\\n' | bc -q",'1267650600228229401496703205376')
        check('ordinary users cannot write the root-owned repository settings',
              'test "$(id -u)" = 1000; test ! -w /etc/coast/packages.json')
        check('guest remains healthy after network transactions', 'check', 'USERLAND CHECK PASS', False, native=True)
        report['state']='passed'
    except BaseException as error:
        report['error']=repr(error)
        (out/'exception.txt').write_text(traceback.format_exc())
        raise
    finally:
        if vm:
            vm.close()
            report['owned_vm_stopped']=vm.process.poll() is not None
        for server,thread in servers:
            server.shutdown()
            server.server_close()
            thread.join(5)
        for log in logs:
            log.close()
        with a.iso.open('rb') as stream:
            report['iso_sha256']=hashlib.file_digest(stream,'sha256').hexdigest()
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':
    main()
