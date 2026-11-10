#!/usr/bin/env python3
"""Verify a restored Coast community instance behind a loopback TLS proxy."""
import argparse
import http.client
import http.server
import json
from pathlib import Path
import re
import ssl
import subprocess
import threading
import time
import urllib.error
import urllib.request
from test_community_server import Browser,NoRedirect


class Proxy(http.server.BaseHTTPRequestHandler):
    backend_port=8771
    def log_message(self,*args):pass
    def forward(self):
        length=int(self.headers.get('Content-Length','0'))
        if length>1024*1024:self.send_error(413);return
        body=self.rfile.read(length) if length else None
        headers={key:value for key,value in self.headers.items() if key.lower() not in ('connection','transfer-encoding')}
        connection=http.client.HTTPConnection('127.0.0.1',self.backend_port,timeout=20)
        try:
            connection.request(self.command,self.path,body,headers)
            response=connection.getresponse();data=response.read();self.send_response(response.status)
            for key,value in response.getheaders():
                if key.lower() not in ('connection','transfer-encoding','content-length','server','date'):self.send_header(key,value)
            self.send_header('Content-Length',str(len(data)));self.end_headers()
            if self.command!='HEAD':self.wfile.write(data)
        finally:connection.close()
    do_GET=forward;do_POST=forward;do_HEAD=forward


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bundle',type=Path,required=True)
    p.add_argument('--config',type=Path,required=True)
    p.add_argument('--state',type=Path,required=True)
    p.add_argument('--output-root',type=Path,required=True)
    a=p.parse_args();out=a.output_root.resolve();out.mkdir(mode=0o700,parents=True,exist_ok=False)
    report=dict(state='failed',checks=[],listeners='127.0.0.1 only',host_trust_store_modified=False,external_email_sent=False)
    original=a.config.read_bytes();config=json.loads(original);Proxy.backend_port=config['web_port']
    assert config['bind']=='127.0.0.1' and config['name'].startswith('coast-community-restore-')
    manage=['python3','-B',str(a.bundle/'manage.py'),'--config',str(a.config)]
    proxy=thread=None
    def service(action):subprocess.run([*manage,action],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    def check(name,condition):
        assert condition,name
        report['checks'].append(name);print('PASS '+name,flush=True)
    try:
        cert,key=out/'certificate.pem',out/'private-key.pem'
        with (out/'openssl.log').open('w') as log:
            subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-noenc','-sha256','-days','2',
                '-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1',
                '-keyout',str(key),'-out',str(cert)],stdout=log,stderr=subprocess.STDOUT,check=True)
        key.chmod(0o600)
        proxy=http.server.ThreadingHTTPServer(('127.0.0.1',0),Proxy)
        origin='https://127.0.0.1:'+str(proxy.server_port)
        tls=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);tls.minimum_version=ssl.TLSVersion.TLSv1_2;tls.load_cert_chain(cert,key)
        proxy.socket=tls.wrap_socket(proxy.socket,server_side=True)
        config['public_origin']=origin;a.config.write_text(json.dumps(config));service('start')
        for _ in range(45):
            try:
                with urllib.request.urlopen('http://127.0.0.1:'+str(Proxy.backend_port)+'/rpc/v5/info?arg[]=gcc',timeout=2):break
            except Exception:time.sleep(1)
        thread=threading.Thread(target=proxy.serve_forever,daemon=True);thread.start()
        context=ssl.create_default_context(cafile=cert);browser=Browser(origin)
        browser.opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),
            urllib.request.HTTPCookieProcessor(browser.jar),urllib.request.HTTPSHandler(context=context),NoRedirect())
        code,_,body=browser.request('/')
        check('community renders through a certificate-verified TLS reverse proxy',code==200 and b'Coast' in body)
        credentials=(a.state/'ADMIN-CREDENTIALS.txt').read_text()
        browser.login('coastadmin',re.search(r'^Password: (.+)$',credentials,re.M)[1])
        check('HTTPS administrator login sets a secure, HttpOnly cookie',any(c.name=='AURSID' and c.secure and 'HttpOnly' in c._rest for c in browser.jar))
        code,_,body=browser.request('/')
        check('authenticated HTTPS session reaches the actual dashboard',code==200 and b'Dashboard' in body)
        code,_,_=browser.request('/logout',dict(next='/'),headers={'Origin':'https://evil.invalid'})
        check('TLS proxy preserves cross-origin operation rejection',code==403)
        plain=Browser('http://127.0.0.1:'+str(Proxy.backend_port))
        code,_,_=plain.request('/login',dict(user='invalid',passwd='invalid',next='/'))
        check('HTTPS configuration refuses browser operations through its plaintext backend',code==403)
        try:
            with urllib.request.urlopen(origin+'/',timeout=5):pass
        except urllib.error.URLError:check('certificate is rejected without the explicitly trusted QA CA',True)
        else:raise AssertionError('unexpected host trust')
        report['state']='passed'
    finally:
        if thread:proxy.shutdown();thread.join(5)
        if proxy:proxy.server_close()
        service('stop');a.config.write_bytes(original)
        subprocess.run(['podman','rm',config['name']],check=True,stdout=subprocess.DEVNULL)
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
