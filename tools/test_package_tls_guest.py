#!/usr/bin/env python3
"""Verify native Coast package/Git TLS using a localhost-only repository fixture."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import socket
import ssl
import subprocess
import threading
import traceback
from serve_repository import RepositoryServer
import test_runtime as runtime

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso',type=Path,required=True)
p.add_argument('--repository',type=Path,required=True)
p.add_argument('--output-root',type=Path,required=True)
a=p.parse_args();out=a.output_root.resolve();out.mkdir(parents=True,exist_ok=False);runtime.WORK=out
vm=server=thread=None
report=dict(state='failed',checks=[],listeners='127.0.0.1 only',host_trust_store_modified=False,
            target_host_executed=False,extra_disks=False,second_physical_computer_tested=False)
def check(name,command,expected=None,user=True,timeout=150):
    text=vm.shell_command(command,as_user=user,expect=expected,timeout=timeout)
    (out/(f'{len(report["checks"]):02d}-output.txt')).write_text(text)
    report['checks'].append(name);print('PASS '+name,flush=True)
try:
    cert,key=out/'certificate.pem',out/'private-key.pem'
    with (out/'openssl.log').open('w') as log:
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-noenc','-sha256','-days','2',
            '-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1,IP:10.0.2.2',
            '-keyout',str(key),'-out',str(cert)],stdout=log,stderr=subprocess.STDOUT,check=True)
    key.chmod(0o600);tls=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);tls.minimum_version=ssl.TLSVersion.TLSv1_2;tls.load_cert_chain(cert,key)
    with socket.socket() as probe:probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    origin='https://10.0.2.2:'+str(port)
    server=RepositoryServer(('127.0.0.1',port),a.repository,public_url=origin,tls_context=tls)
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    vm=runtime.VM(4,storage=False,network=True,memory='16G',accel='kvm',iso=a.iso.resolve(),label='package-tls')
    check('guest network reaches the persistent localhost service',"curl -q --noproxy '*' --retry 12 --retry-all-errors --retry-delay 1 --max-time 30 -fsS http://10.0.2.2:8765/health",'Coast Packages')
    check('native client rejects an untrusted HTTPS certificate','coast server '+origin+' --check; test $? -ne 0','SSL')
    encoded=base64.b64encode(cert.read_bytes()).decode()
    for offset in range(0,len(encoded),300):vm.command(('write' if offset==0 else 'append')+' /tmp/package-tls-ca.b64 '+encoded[offset:offset+300])
    check('only the disposable VM trusts the QA certificate','base64 -d /tmp/package-tls-ca.b64 >> /etc/ssl/cert.pem',user=False)
    check('native curl rejects a certificate hostname mismatch',"curl -q --noproxy '*' --resolve wrong.invalid:"+str(port)+':10.0.2.2 https://wrong.invalid:'+str(port)+'/health -o /tmp/wrong-host; test $? -eq 60','certificate')
    vm.command('true')
    check('authenticated server selection verifies TLS and the existing signing pin','coast server '+origin,'Package downloads and source builds now use')
    check('sandboxed libalpm downloads signed metadata over verified TLS','pacman -Syy --noconfirm','Synchronizing',False)
    check('clear the calculator and cache before a real TLS package transfer','pacman -R --noconfirm bc && rm -f /var/cache/pacman/pkg/bc-*','removing bc',False)
    vm.command('true')
    check('native pacman verifies and installs the real HTTPS archive','coast install bc --noconfirm','installing bc')
    check('TLS-installed files match their recorded hashes','pacman -Qkk bc','0 altered files')
    check('Git HTTPS clones the signed source commit','coast clone bc','Recipe checkout:')
    check('makepkg fetches sources through verified HTTPS and checks their digest','cd /home/user/Builds/bc && makepkg --verifysource','Passed')
    vm.command('check',expect='USERLAND CHECK PASS',timeout=90)
    report['checks'].append('guest remains healthy after TLS transactions');report['state']='passed'
except BaseException as error:
    report['error']=repr(error);(out/'exception.txt').write_text(traceback.format_exc());raise
finally:
    if vm:vm.close()
    if server:server.shutdown();server.server_close()
    if thread:thread.join(5)
    report['owned_vm_stopped']=bool(vm and vm.process.poll() is not None)
    with a.iso.open('rb') as f:report['iso_sha256']=hashlib.file_digest(f,'sha256').hexdigest()
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
