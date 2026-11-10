#!/usr/bin/env python3
"""Exercise real aurweb accounts, SSH submissions, Git reads and community actions."""
import argparse
import email
from email import policy
import hashlib
import http.cookiejar
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


class Form(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.values = {}
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if tag == 'input' and values.get('name'):
            self.values[values['name']] = values.get('value', '')


class Browser:
    def __init__(self, base):
        self.base = base
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
                      urllib.request.HTTPCookieProcessor(self.jar), NoRedirect())

    def request(self, path, fields=None, headers=None, origin=True):
        data = urllib.parse.urlencode(fields).encode() if fields is not None else None
        values = {'Referer': self.base+('/login' if path.startswith('/login') else '/')}
        if origin:
            values['Origin'] = self.base
        values.update(headers or {})
        request = urllib.request.Request(self.base+path, data=data, headers=values)
        try:
            response = self.opener.open(request, timeout=30)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.code, {name.title(): value for name,value in response.headers.items()}, response.read()

    def login(self, username, password):
        code, _, _ = self.request('/login', dict(user=username, passwd=password, next='/'))
        assert code == 303 and any(cookie.name == 'AURSID' for cookie in self.jar), 'Login failed'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--server', default='http://127.0.0.1:8770')
    p.add_argument('--container', default='coast-community-r1')
    p.add_argument('--data', type=Path, required=True)
    p.add_argument('--output-root', type=Path, required=True)
    args = p.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False, mode=0o700)
    report = dict(state='failed', checks=[], server=args.server,
                  upstream='aurweb ed420d423d20a69ed5b9bd77aea6b81538d16248',
                  external_email_sent=False)
    suffix = secrets.token_hex(3)
    package = 'coast-community-check-'+suffix
    publisher, observer = 'ports'+suffix, 'reader'+suffix
    passwords = {publisher: secrets.token_urlsafe(30), observer: secrets.token_urlsafe(30)}
    with (out/'credentials.json').open('x') as stream:
        json.dump(passwords, stream)
    (out/'credentials.json').chmod(0o600)
    report.update(package=package, publisher=publisher, observer=observer)

    def check(name, condition):
        assert condition, name
        report['checks'].append(name)
        print('PASS '+name, flush=True)

    def container_python(code, values=None, user='aur'):
        result = subprocess.run(['podman', 'exec', '-i', '--user', user, args.container,
                                 'python', '-c', code], input=json.dumps(values or {}), text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        return result.stdout

    def messages(address):
        code = '''import email,json,sys
from email import policy
from pathlib import Path
address=json.load(sys.stdin)['address']
result=[]
for path in sorted(Path('/data/outbox').glob('*.eml')):
 msg=email.message_from_bytes(path.read_bytes(),policy=policy.default)
 if address not in str(msg['To']):continue
 parts=[p.get_content() for p in msg.walk() if p.get_content_type()=='text/plain']
 result.append(dict(subject=str(msg['Subject']),body='\\n'.join(parts)))
print(json.dumps(result))'''
        return json.loads(container_python(code, dict(address=address)))

    def signup(username, public_key=''):
        browser = Browser(args.server)
        code, _, body = browser.request('/register')
        assert code == 200
        salt = Form(body.decode()).values['captcha_salt']
        token = hashlib.md5(salt.encode()).hexdigest()[:3]
        version = subprocess.check_output(['pacman', '-V'], env=dict(os.environ, LC_ALL='C'))
        answer = hashlib.md5(re.sub(rb'[0-9]+', token.encode(), version)).hexdigest()[:6]
        address = username+'@example.net'
        code, _, body = browser.request('/register', dict(U=username, E=address, H='on',
                    L='en', TZ='UTC', PK=public_key, captcha_salt=salt, captcha=answer))
        if code != 200:
            # Preserve only human-readable errors, never form values/tokens.
            errors = re.findall(rb'<li>([^<>]+)</li>', body)
            raise AssertionError('Registration failed: '+str(code)+' '+str(errors[:5]))
        collected = []
        for _ in range(40):
            collected = messages(address)
            if len(collected) >= 2:
                break
            time.sleep(.1)
        text = '\n'.join(item['body'] for item in collected)
        reset = re.search(r'/passreset/?\?resetkey=([A-Za-z0-9_-]+)', text)
        verify = re.search(r'/account/verify/([A-Za-z0-9_-]+)', text)
        assert reset and verify, 'Welcome/verification mail was not delivered to the private local outbox'
        code, _, _ = browser.request('/account/verify/'+verify[1])
        assert code == 303, 'Email verification failed'
        fields = dict(user=username, resetkey=reset[1], password=passwords[username], confirm=passwords[username])
        code, _, _ = browser.request('/passreset', fields)
        assert code == 303, 'Initial password setup failed'
        code, _, _ = browser.request('/passreset', fields)
        check('password-reset link is single use for '+username, code == 404)
        browser.login(username, passwords[username])
        check('real registration, private email delivery, verification and login for '+username, True)
        return browser

    def info():
        code, _, body = Browser(args.server).request('/rpc/v5/info?'+urllib.parse.urlencode({'arg[]': package}))
        assert code == 200
        return json.loads(body)['results'][0]

    def git(command, cwd=None, allowed=(0,), environment=None):
        result = subprocess.run(['git', '-c', 'core.hooksPath=/dev/null', *command], cwd=cwd,
                    env=environment or git_env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode not in allowed:
            raise AssertionError('Git operation failed: '+result.stdout[-2500:])
        return result

    try:
        public = Browser(args.server)
        code, _, body = public.request('/')
        check('upstream community homepage is available', code == 200 and b'Packages' in body)
        code,_,body=public.request('/static/opensearch.xml')
        check('browser search targets this Coast server instead of Arch',code==200 and args.server.encode() in body and b'aur.archlinux.org' not in body)
        code,_,body=public.request('/rss/')
        check('community RSS identifies Coast and uses the active server',code==200 and b'Coast Community Newest Packages' in body and args.server.encode() in body)
        code, _, _ = public.request('/health', headers={'Host': 'evil.invalid'})
        check('unconfigured browser Host values are rejected', code == 403)
        code, _, _ = public.request('/login', dict(user='invalid', passwd='invalid', next='/'),
                                     headers={'Origin': 'http://evil.invalid'})
        check('cross-origin web operations are rejected before authentication', code == 403)
        key = out/'publisher-key'
        subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(key)], check=True)
        reader_key=out/'reader-key'
        subprocess.run(['ssh-keygen','-q','-t','ed25519','-N','','-f',str(reader_key)],check=True)
        author = signup(publisher, key.with_suffix('.pub').read_text().strip())
        reader = signup(observer,reader_key.with_suffix('.pub').read_text().strip())
        host_key = subprocess.check_output(['podman', 'exec', args.container, 'cat',
                              '/data/ssh/ssh_host_ed25519_key.pub'], text=True).strip().split()
        known = out/'known_hosts'
        known.write_text('[127.0.0.1]:2222 '+' '.join(host_key[:2])+'\n')
        ssh = ['ssh', '-i', str(key), '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes',
               '-o', 'StrictHostKeyChecking=yes', '-o', 'UserKnownHostsFile='+str(known), '-p', '2222']
        git_env = dict(os.environ, GIT_SSH_COMMAND=shlex.join(ssh), GIT_TERMINAL_PROMPT='0',
                       GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null')
        reader_ssh=[reader_key if part==key else str(reader_key) if part==str(key) else part for part in ssh]
        reader_env=dict(git_env,GIT_SSH_COMMAND=shlex.join(list(map(str,reader_ssh))))
        checkout = out/'recipe'
        checkout.mkdir()
        source = '#include <stdio.h>\n#include <unistd.h>\nint main(void){printf("COAST_COMMUNITY_HELLO uid=%u\\n",getuid());return getuid()==0;}\n'
        (checkout/'hello.c').write_text(source)
        digest = hashlib.sha256(source.encode()).hexdigest()
        (checkout/'PKGBUILD').write_text("pkgname="+package+"\npkgver=1.0\npkgrel=1\npkgdesc='Native Coast community submission example'\narch=('x86_64')\nlicense=('0BSD')\ndepends=('musl')\nmakedepends=('gcc')\nsource=('hello.c')\nsha256sums=('"+digest+"')\nbuild() { cc \"$srcdir/hello.c\" -o hello-community; }\ncheck() { ./hello-community; }\npackage() { install -Dm755 hello-community \"$pkgdir/usr/bin/hello-community\"; }\n")
        (checkout/'.SRCINFO').write_text('pkgbase = '+package+'\n\tpkgdesc = Native Coast community submission example\n\tpkgver = 1.0\n\tpkgrel = 1\n\tarch = x86_64\n\tlicense = 0BSD\n\tdepends = musl\n\tmakedepends = gcc\n\tsource = hello.c\n\tsha256sums = '+digest+'\n\npkgname = '+package+'\n')
        git(['init', '--initial-branch=master'], checkout)
        git(['add', '--', 'PKGBUILD', '.SRCINFO', 'hello.c'], checkout)
        git(['-c', 'user.name=Coast community QA', '-c', 'user.email=qa@coast.invalid', 'commit', '-m', 'Reviewed community build example'], checkout)
        remote = 'ssh://aur@127.0.0.1:2222/'+package+'.git'
        result = git(['push', remote, 'master'], checkout)
        (out/'git-push.txt').write_text(result.stdout)
        check('SSH public-key authentication submits a real Git recipe through the aurweb hook',
              info()['Maintainer'] == publisher and info()['Version'] == '1.0-1')
        clone = out/'http-clone'
        git(['-c', 'http.proxy=', 'clone', args.server+'/'+package+'.git', str(clone)])
        check('anonymous smart HTTP Git clone preserves the submitted commit',
              git(['rev-parse', 'HEAD'], clone).stdout == git(['rev-parse', 'HEAD'], checkout).stdout)
        code, _, body = public.request(info()['URLPath'])
        check('RPC source snapshot is downloadable', code == 200 and body.startswith(b'\x1f\x8b'))
        forbidden = subprocess.run([*ssh, 'aur@127.0.0.1', 'sh -c id'], text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        check('authenticated SSH cannot obtain a shell', forbidden.returncode != 0)
        before = info()['NumVotes']
        reader.request('/pkgbase/'+package+'/vote', {})
        check('authenticated voting updates the real package record', info()['NumVotes'] == before+1)
        reader.request('/pkgbase/'+package+'/vote', {})
        check('duplicate votes do not increment totals', info()['NumVotes'] == before+1)
        reader.request('/pkgbase/'+package+'/unvote', {})
        check('unvote restores the original total', info()['NumVotes'] == before)
        code, _, _ = reader.request('/pkgbase/'+package+'/adopt', {})
        check('a reader cannot take ownership of a maintained package', info()['Maintainer'] == publisher)
        reader.request('/pkgbase/'+package+'/delete', dict(confirm='on'))
        check('unprivileged users cannot delete a package', info()['Maintainer'] == publisher)
        code, headers, _ = reader.request('/pkgbase/'+package+'/comments',
                      dict(comment='COAST_COMMENT **reviewed** <script>COAST_XSS</script> [unsafe](javascript:alert(1))'))
        check('authenticated comments persist', code == 303 and '#comment-' in headers.get('Location', ''))
        comment = headers['Location'].rsplit('#comment-', 1)[1]
        _, _, body = public.request('/packages/'+package)
        check('comment rendering removes executable HTML and JavaScript links',
              b'COAST_COMMENT' in body and b'<script>COAST_XSS' not in body and b'href="javascript:' not in body)
        code, _, _ = author.request('/pkgbase/'+package+'/comments/'+comment, dict(comment='UNAUTHORIZED_EDIT'))
        _, _, body = public.request('/packages/'+package)
        check('another ordinary account cannot edit a reader comment', b'UNAUTHORIZED_EDIT' not in body)
        reader.request('/pkgbase/'+package+'/flag', dict(comments='Coast QA out-of-date report'))
        check('out-of-date flags persist', info()['OutOfDate'] is not None)
        author.request('/pkgbase/'+package+'/unflag', {})
        check('maintainer can clear the out-of-date flag', info()['OutOfDate'] is None)
        admin_text = (args.data/'ADMIN-CREDENTIALS.txt').read_text()
        admin = Browser(args.server)
        admin.login('coastadmin', re.search(r'^Password: (.+)$', admin_text, re.M)[1])
        admin.request('/pkgbase/'+package+'/comments/'+comment+'/delete', {})
        _, _, body = public.request('/packages/'+package)
        check('moderator can remove abusive comments', b'COAST_COMMENT' not in body)
        admin.request('/pkgbase/'+package+'/comments/'+comment+'/undelete', {})
        _, _, body = public.request('/packages/'+package)
        check('moderator can restore a removed comment', b'COAST_COMMENT' in body)
        code, _, _ = reader.request('/pkgbase/'+package+'/request',
                              dict(type='deletion', comments='Disposable QA request; test moderation queue.'))
        _, _, body = admin.request('/requests')
        check('community requests reach the moderator queue', code == 303 and package.encode() in body)
        for path in [checkout/'PKGBUILD',checkout/'.SRCINFO']:
            path.write_text(path.read_text().replace('pkgrel=1','pkgrel=2').replace('pkgrel = 1','pkgrel = 2'))
        git(['add','--','PKGBUILD','.SRCINFO'],checkout)
        git(['-c','user.name=Coast community QA','-c','user.email=qa@coast.invalid','commit','-m','Test maintainer ownership'],checkout)
        rejected=git(['push',remote,'master'],checkout,allowed=(1,128),environment=reader_env)
        check('another authenticated SSH account cannot update a maintained package',rejected.returncode!=0 and info()['Version']=='1.0-1')
        author.request('/pkgbase/'+package+'/comaintainers',dict(users=observer))
        _,_,body=author.request('/pkgbase/'+package+'/comaintainers')
        check('maintainer can add a co-maintainer',observer.encode() in body)
        git(['push',remote,'master'],checkout,environment=reader_env)
        check('co-maintainer SSH key can submit an updated recipe',info()['Version']=='1.0-2')
        author.request('/pkgbase/'+package+'/disown',dict(confirm='on'))
        check('disown promotes the next co-maintainer',info()['Maintainer']==observer)
        reader.request('/pkgbase/'+package+'/disown',dict(confirm='on'))
        check('last maintainer can leave an orphaned package',info()['Maintainer'] is None)
        author.request('/pkgbase/'+package+'/adopt',{})
        _,_,body=admin.request('/requests')
        rows=re.findall(rb'<tr[^>]*>(.*?)</tr>',body,re.S)
        adoption=[row for row in rows if package.encode() in row and b'via=' in row]
        check('ordinary orphan adoption enters the upstream approval queue',info()['Maintainer'] is None and bool(adoption))
        via=re.search(rb'via=(\d+)',adoption[0])[1].decode()
        admin.request('/pkgbase/'+package+'/adopt',dict(via=via))
        check('moderator approval grants the orphan to its applicant',info()['Maintainer']==publisher)
        old_cookie = next(cookie.value for cookie in reader.jar if cookie.name == 'AURSID')
        reader.request('/logout', dict(next='/'))
        forged = Browser(args.server)
        code, _, _ = forged.request('/account/'+observer+'/edit', headers={'Cookie': 'AURSID='+old_cookie})
        check('logout invalidates the server session', code == 303)
        report['state'] = 'passed'
    except BaseException as error:
        report['error'] = re.sub(r'[a-fA-F0-9]{32,}', '[redacted]', str(error))
        raise
    finally:
        (out/'report.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
