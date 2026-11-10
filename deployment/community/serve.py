#!/usr/bin/python
"""Supervise upstream aurweb, its Git interfaces and private database in a container."""
import configparser
import json
import os
from pathlib import Path
import pwd
import secrets
import signal
import subprocess
import sys
import time
import re
from urllib.parse import urlsplit

os.umask(0o077)
DATA = Path('/data')
RUN = Path('/run/coast')
children = []
stopping = False


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def as_user(user, args):
    return ['runuser', '-u', user, '--', *args]


def start(name, args):
    process = subprocess.Popen(args, start_new_session=True)
    children.append((name, process))
    return process


def stop(signum=None, frame=None):
    global stopping
    stopping = True


def wait_socket(path, process):
    for _ in range(200):
        if process.poll() is not None:
            raise RuntimeError('Service exited before creating '+str(path))
        if path.exists():
            return
        time.sleep(.1)
    raise RuntimeError('Timed out waiting for '+str(path))


def owned_dir(path, user, mode=0o700):
    account = pwd.getpwnam(user)
    path.mkdir(parents=True, exist_ok=True)
    path.chmod(mode)
    os.chown(path, account.pw_uid, account.pw_gid)


def configure():
    public_origin=os.environ.get('COAST_PUBLIC_ORIGIN','http://127.0.0.1:8770').rstrip('/')
    endpoint=urlsplit(public_origin)
    if (endpoint.scheme not in ('http','https') or endpoint.username or endpoint.password or
        endpoint.path or endpoint.query or endpoint.fragment or not endpoint.hostname or
        not re.fullmatch('[A-Za-z0-9.-]+',endpoint.hostname)):
        raise ValueError('invalid community public origin')
    ssh_port=int(os.environ.get('COAST_SSH_PORT','2222'))
    if not 1024<=ssh_port<=65535:raise ValueError('invalid public SSH port')
    DATA.mkdir(exist_ok=True)
    DATA.chmod(0o711)
    RUN.mkdir(exist_ok=True)
    RUN.chmod(0o755)
    for name, owner, mode in [('db', 'mysql', 0o700), ('git', 'aur', 0o700),
                              ('outbox', 'aur', 0o700), ('archives', 'aur', 0o700),
                              ('ssh', 'root', 0o700)]:
        owned_dir(DATA/name, owner, mode)
    owned_dir(RUN/'mysql', 'mysql', 0o755)
    owned_dir(RUN/'sockets', 'aur', 0o755)
    for name in ['body', 'proxy', 'fastcgi', 'uwsgi', 'scgi', 'cache']:
        owned_dir(RUN/name, 'aur', 0o700)
    (Path('/run/sshd')).mkdir(exist_ok=True)
    secret_file = DATA/'secrets.json'
    if not secret_file.exists():
        with secret_file.open('x') as stream:
            json.dump(dict(database=secrets.token_hex(32), session=secrets.token_hex(48),
                           admin=secrets.token_urlsafe(30)), stream)
        secret_file.chmod(0o600)
    values = json.loads(secret_file.read_text())
    config = configparser.RawConfigParser()
    config.read('/opt/aurweb/conf/config.defaults')
    settings = {
        'database': dict(backend='mysql', name='coast_community', user='aur',
                         password=values['database'], socket='/run/coast/mysql/mysqld.sock'),
        'options': dict(aurwebdir='/opt/aurweb', aur_location=public_origin,
                        git_clone_uri_anon=public_origin+'/%s.git',
                        git_clone_uri_priv='ssh://aur@'+endpoint.hostname+':'+str(ssh_port)+'/%s.git',
                        disable_http_login='1' if endpoint.scheme=='https' else '0', enable_maintenance='0',
                        localedir='/usr/share/locale', salt_rounds='12', traceback='0',
                        cache='redis', redis_address='redis://127.0.0.1:6379',
                        render_comment_cmd='/usr/bin/aurweb-rendercomment',
                        aur_request_ml='requests@coast.invalid'),
        'notifications': dict(sendmail='/opt/coast/local-mail.py', sender='notify@coast.invalid',
                              reply_to='noreply@coast.invalid', **{'error-project': '', 'error-token': ''}),
        'fastapi': dict(session_secret=values['session']),
        'auth': {'git-serve-cmd': '/opt/coast/git-serve', 'ssh-options': 'restrict'},
        'serve': {'repo-path': '/data/git/aur.git', 'git-update-cmd': '/opt/coast/git-update',
                  'ssh-cmdline': 'ssh -p 2222 aur@127.0.0.1'},
        'mkpkglists': {'archivedir': '/data/archives', 'packagesfile': '/data/archives/packages.gz',
                      'packagesmetafile': '/data/archives/packages-meta-v1.json.gz',
                      'packagesmetaextfile': '/data/archives/packages-meta-ext-v1.json.gz',
                      'pkgbasefile': '/data/archives/pkgbase.gz', 'userfile': '/data/archives/users.gz'},
        'tracing': dict(otlp_endpoint=''),
    }
    # Config uses hyphens for two upstream option names.
    settings['options']['enable-maintenance'] = settings['options'].pop('enable_maintenance')
    settings['options']['render-comment-cmd'] = settings['options'].pop('render_comment_cmd')
    for section, options in settings.items():
        if not config.has_section(section):
            config.add_section(section)
        for key, value in options.items():
            config.set(section, key, value)
    config_file = RUN/'config'
    with config_file.open('w') as stream:
        config.write(stream)
    aur = pwd.getpwnam('aur')
    os.chown(config_file, aur.pw_uid, aur.pw_gid)
    config_file.chmod(0o600)
    nginx=(Path('/opt/coast/nginx.conf')).read_text()
    allowed=' '.join(sorted({'127.0.0.1','localhost','10.0.2.2',endpoint.hostname}))
    nginx=nginx.replace('default 0; 127.0.0.1 1; localhost 1; 10.0.2.2 1;',
                        'default 0; '+' '.join(host+' 1;' for host in allowed.split()))
    nginx=nginx.replace('server_name 127.0.0.1 localhost 10.0.2.2;', 'server_name '+allowed+';')
    if endpoint.scheme=='https':nginx=nginx.replace('X-Forwarded-Proto $scheme','X-Forwarded-Proto https')
    (RUN/'nginx.conf').write_text(nginx)
    (RUN/'nginx.conf').chmod(0o644)
    (RUN/'cgitrc').write_text('virtual-root=/cgit\nroot-title=Brine Community Packages\n'
                            'snapshots=tar.gz\ncss=/css/cgit.css\nclone-prefix=http://127.0.0.1:8770\n'
                            'enable-git-config=1\nrepo.url=aur.git\nrepo.name=Brine recipe history\n'
                            'repo.path=/data/git/aur.git\nrepo.snapshots=tar.gz\n')
    (RUN/'cgitrc').chmod(0o644)
    return values


def initialize_database(values):
    if not (DATA/'db/mysql').exists():
        run(as_user('mysql', ['mariadb-install-db', '--datadir=/data/db',
                            '--auth-root-authentication-method=socket', '--skip-test-db']))
    database = start('database', as_user('mysql', ['mariadbd', '--datadir=/data/db',
                     '--socket=/run/coast/mysql/mysqld.sock', '--pid-file=/run/coast/mysql/mysqld.pid',
                     '--skip-networking', '--skip-name-resolve', '--innodb-buffer-pool-size=64M']))
    wait_socket(RUN/'mysql/mysqld.sock', database)
    sql = ("CREATE DATABASE IF NOT EXISTS coast_community CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
           "CREATE USER IF NOT EXISTS 'aur'@'localhost' IDENTIFIED BY '"+values['database']+"';"
           "GRANT ALL ON coast_community.* TO 'aur'@'localhost';\n")
    run(['mariadb', '-u', 'root', '--socket=/run/coast/mysql/mysqld.sock'], input=sql, text=True)
    query = run(['mariadb', '-u', 'root', '--socket=/run/coast/mysql/mysqld.sock', '-N', '-e',
                 "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='coast_community' AND table_name='Users'"],
                stdout=subprocess.PIPE, text=True)
    if query.stdout.strip() == '0':
        run(as_user('aur', ['python', '-m', 'aurweb.initdb']))
    else:
        run(as_user('aur', ['alembic', 'upgrade', 'head']))
    bootstrap = '''import json,sys,time
from aurweb import db
from aurweb.models import User,AccountType
values=json.load(sys.stdin)
if not db.query(User,User.Username=='coastadmin').first():
 with db.begin():
  db.create(User,Username='coastadmin',Email='admin@coast.invalid',Passwd=values['admin'],
   AccountType=db.query(AccountType,AccountType.ID==4).one(),EmailVerified=True)
'''
    run(as_user('aur', ['python', '-c', bootstrap]), input=json.dumps(values), text=True)
    credentials = DATA/'ADMIN-CREDENTIALS.txt'
    if not credentials.exists():
        with credentials.open('x') as stream:
            stream.write('Brine community local administrator\nUsername: coastadmin\nPassword: '+values['admin']+'\n')
        credentials.chmod(0o600)


def initialize_git():
    repository = DATA/'git/aur.git'
    if not (repository/'config').exists():
        run(as_user('aur', ['git', 'init', '--bare', '--initial-branch=master', str(repository)]))
        for key, value in [('transfer.hideRefs', '^refs/'), ('transfer.hideRefs', '!refs/'),
                           ('transfer.hideRefs', '!HEAD'), ('receive.autogc', 'false'),
                           ('gc.pruneExpire', '3.months.ago')]:
            run(as_user('aur', ['git', '-C', str(repository), 'config', '--add', key, value]))
        (repository/'hooks/update').symlink_to('/opt/coast/git-update')
    key = DATA/'ssh/ssh_host_ed25519_key'
    if not key.exists():
        run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(key)])
    fingerprint = run(['ssh-keygen', '-lf', str(key)+'.pub'], stdout=subprocess.PIPE,
                      text=True).stdout.split()[1]
    config = configparser.RawConfigParser()
    config.read(RUN/'config')
    config.remove_section('fingerprints')
    config.add_section('fingerprints')
    config.set('fingerprints', 'Ed25519', fingerprint)
    with (RUN/'config').open('w') as stream:
        config.write(stream)


def main():
    for sig in [signal.SIGTERM, signal.SIGINT]:
        signal.signal(sig, stop)
    try:
        values = configure()
        initialize_database(values)
        initialize_git()
        start('cache', as_user('aur', ['valkey-server', '--bind', '127.0.0.1', '--port', '6379',
                    '--save', '', '--appendonly', 'no', '--dir', '/run/coast/cache']))
        run(as_user('aur',['aurweb-mkpkglists','--extended']),timeout=90)
        start('git-http', as_user('aur', ['uwsgi', '--socket', '/run/coast/sockets/git-http.sock',
              '--chmod-socket=666', '--plugins', 'cgi', '--cgi', '/usr/lib/git-core/git-http-backend',
              '--master', '--die-on-term']))
        start('cgit', as_user('aur', ['uwsgi', '--socket', '/run/coast/sockets/cgit.sock',
              '--chmod-socket=666', '--plugins', 'cgi', '--cgi', '/usr/lib/cgit-aurweb/cgit.cgi',
              '--master', '--die-on-term']))
        start('web', as_user('aur', ['uvicorn', 'brine_app:app', '--app-dir', '/opt/coast', '--host', '127.0.0.1',
              '--port', '8001', '--workers', '2', '--forwarded-allow-ips', '127.0.0.1', '--no-access-log']))
        start('ssh', ['/usr/bin/sshd', '-D', '-e', '-f', '/opt/coast/sshd_config'])
        start('frontend', ['nginx', '-c', '/run/coast/nginx.conf', '-g', 'daemon off;'])
        next_maintenance = time.monotonic()+30
        while not stopping:
            for name, process in children:
                if process.poll() is not None:
                    raise RuntimeError(name+' exited with status '+str(process.returncode))
            if time.monotonic() >= next_maintenance:
                for command in ['aurweb-mkpkglists', 'aurweb-popupdate', 'aurweb-pkgmaint',
                                'aurweb-usermaint', 'aurweb-requestmaint']:
                    run(as_user('aur', [command,*(['--extended'] if command=='aurweb-mkpkglists' else [])]), timeout=90)
                next_maintenance = time.monotonic()+300
            time.sleep(.25)
    finally:
        for _, process in reversed(children):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
        deadline = time.monotonic()+15
        for _, process in reversed(children):
            try:
                process.wait(max(.1, deadline-time.monotonic()))
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()


if __name__ == '__main__':
    main()
