"""Search local packages and Git recipes; upstream makepkg/pacman build/install."""
import errno
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import unicodedata
from urllib.parse import urlsplit

CATALOG = Path('/usr/share/coast/packages/catalog.json')
CONFIG = Path('/etc/coast/packages.json')
NAME = re.compile(r'[a-zA-Z0-9][a-zA-Z0-9@._+-]{0,63}\Z')

_search_spec = importlib.util.spec_from_file_location('coast_package_search', Path(__file__).with_name('search.py'))
_search_module = importlib.util.module_from_spec(_search_spec)
_search_spec.loader.exec_module(_search_module)
search = _search_module.search
_community_spec=importlib.util.spec_from_file_location('coast_package_community',Path(__file__).with_name('community.py'))
_community_module=importlib.util.module_from_spec(_community_spec)
_community_spec.loader.exec_module(_community_module)


class Error(Exception):
    pass


def server_origin(value):
    if not isinstance(value, str) or re.search(r'[^A-Za-z0-9:/.-]', value):
        raise Error('server must be a plain HTTP(S) origin without credentials, a path or shell characters')
    endpoint = urlsplit(value)
    host = endpoint.hostname or ''
    if (endpoint.scheme not in ('http', 'https') or endpoint.username or endpoint.password or
        endpoint.query or endpoint.fragment or endpoint.path not in ('', '/') or
        not host or any(not re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?', label)
                        for label in host.split('.')) or len(host) > 253):
        raise Error('server must be a plain IPv4 or DNS HTTP(S) origin')
    port = endpoint.port
    if port is not None and not 1 <= port <= 65535:
        raise Error('invalid repository port')
    return endpoint.scheme + '://' + host + (':' + str(port) if port is not None else '')


def run(command, cwd=None, capture=False, env=None):
    result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE if capture else None, env=env)
    if result.returncode:
        raise Error((result.stderr or '').strip() or f'{command[0]} exited {result.returncode}')
    return result.stdout.strip() if capture else None


def config(required=True):
    data = json.loads(CONFIG.read_text())
    if data.get('version') == 1 and data.get('url') is None and data.get('signing_fingerprint') is None:
        if required:
            raise Error('no repository configured; use coast configure URL --key FILE --fingerprint FINGERPRINT')
        return data
    endpoint = urlsplit(data['url'])
    if data.get('version') != 1 or endpoint.scheme not in ('file', 'http', 'https') or endpoint.username or endpoint.password or endpoint.query or endpoint.fragment:
        raise Error('invalid Coast Packages server configuration')
    if endpoint.scheme != 'file' and (not endpoint.hostname or endpoint.path not in ('', '/')):
        raise Error('repository URL must be a server origin')
    if not re.fullmatch('[A-F0-9]{40}', data['signing_fingerprint']):
        raise Error('invalid repository signing fingerprint')
    return data


def community_fetch(relative,maximum):
    endpoint=config().get('community_url')
    if not endpoint:raise Error('no community server configured; use coast community URL')
    with tempfile.TemporaryDirectory(prefix='coast-community-') as temporary:
        path=Path(temporary)/'response'
        download(relative,path,maximum,endpoint,retries=False)
        return path.read_bytes()


def community_info(names):
    return _community_module.info(community_fetch,names)


def community_search(queries,required=False):
    if not config().get('community_url'):
        if required:raise Error('no community server configured; use coast community URL')
        return []
    try:return _community_module.search(community_fetch,queries)
    except (Error,OSError,ValueError,KeyError) as error:
        if required:raise Error(str(error)) from error
        print('coast: community search unavailable: '+str(error),file=sys.stderr)
        return []


def verify_signature(path, signature):
    if signature.stat().st_size > 16384:
        raise Error('signature exceeds its size limit')
    status = run(['/usr/bin/gpg', '--no-options', '--batch', '--no-autostart', '--no-auto-key-retrieve',
                  '--homedir', '/etc/pacman.d/gnupg', '--status-fd', '1', '--verify', str(signature), str(path)], capture=True)
    fingerprints = [line.split()[2] for line in status.splitlines() if line.startswith('[GNUPG:] VALIDSIG ')]
    if fingerprints != [config()['signing_fingerprint']]:
        raise Error('artifact was not signed by the configured repository key')


def validate_catalog(path, signature):
    if path.stat().st_size > 4 * 1024 * 1024:
        raise Error('source catalog exceeds its size limit')
    verify_signature(path, signature)
    data = json.loads(path.read_text())
    if data.get('version') != 1 or not isinstance(data.get('results'), list):
        raise Error('invalid Coast Packages source catalog')
    for package in data['results']:
        if not NAME.fullmatch(package['Name']) or not NAME.fullmatch(package['PackageBase']):
            raise Error('invalid package name in source catalog')
        if not re.fullmatch('[a-f0-9]{40}', package['Commit']):
            raise Error('invalid recipe Git commit')
        if package['URLPath'] != '/recipes/' + package['PackageBase'] + '.git':
            raise Error('recipe URL does not match its package name')
        if not isinstance(package.get('Files'), dict) or 'PKGBUILD' not in package['Files']:
            raise Error('recipe lacks verified file metadata')
        for name, digest in package['Files'].items():
            if '/' in name or name in ('.', '..') or not re.fullmatch('[a-f0-9]{64}', digest):
                raise Error('invalid recipe file metadata')
    if len({p['Name'] for p in data['results']}) != len(data['results']):
        raise Error('duplicate package in source catalog')
    return {package['Name']: package for package in data['results']}


def catalog():
    current = home() / '.cache/coast/packages/current' if 1000 <= os.getuid() < 1032 else None
    path = current / 'index.json' if current and current.exists() else CATALOG
    return validate_catalog(path, path.with_name(path.name + '.sig'))


def download(relative, destination, maximum=4 * 1024 * 1024, server=None,retries=True):
    endpoint = server_origin(server) if server else config()['url'].rstrip('/')
    protocols = '=file' if endpoint.startswith('file:') else '=http,https'
    run(['/usr/bin/curl', '-q', '--noproxy', '*', '--proto', protocols, '--fail', '--silent', '--show-error',
         '--connect-timeout', '5' if retries else '2',
         *(['--retry','8','--retry-all-errors','--retry-delay','1'] if retries else []),
         '--max-time', '60' if retries else '8', '--max-filesize', str(maximum),
         '--output', str(destination), endpoint + relative])
    if destination.stat().st_size > maximum:
        raise Error('repository response exceeds its size limit')


def check_server(url):
    url = server_origin(url)
    with tempfile.TemporaryDirectory(prefix='coast-server-check-') as temporary:
        directory = Path(temporary)
        health = directory/'health.json'
        download('/health', health, 16384, url)
        identity = json.loads(health.read_text())
        if identity.get('name') != 'Coast Packages' or identity.get('fingerprint') != config()['signing_fingerprint']:
            raise Error('server identity does not match the already trusted repository key')
        for route, maximum in [('recipes/index.json', 4*1024*1024), ('repo/x86_64/coast.db', 32*1024*1024)]:
            target, signature = directory/Path(route).name, directory/(Path(route).name+'.sig')
            download('/'+route, target, maximum, url)
            download('/'+route+'.sig', signature, 16384, url)
            if route.startswith('recipes/'):
                validate_catalog(target, signature)
            else:
                verify_signature(target, signature)
    print('Verified package database and source catalog: ' + url)
    return url


def check_community(url):
    url=server_origin(url)
    with tempfile.TemporaryDirectory(prefix='coast-community-check-') as temporary:
        target=Path(temporary)/'info.json'
        download('/rpc/v5/info?arg[]=coast-health-check',target,16384,url,retries=False)
        data=json.loads(target.read_text())
        if data.get('version')!=5 or data.get('type')!='multiinfo' or not isinstance(data.get('results'),list):
            raise Error('server does not provide the AUR v5 community API')
    print('Community API available: '+url)
    return url


def apply_community(url,root=Path('/'),owner=0):
    if root==Path('/') and owner!=0:raise Error('system settings require root ownership')
    url=server_origin(url) if url!='off' else None
    path=root/'etc/coast/packages.json'
    for parent in [root/'etc',path.parent]:
        info=parent.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid!=owner or info.st_mode&0o022:
            raise Error('unsafe repository configuration directory')
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW)
    with os.fdopen(fd) as stream:
        info=os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid!=owner or info.st_mode&0o022 or info.st_size>1024*1024:
            raise Error('unsafe repository configuration file')
        data=json.load(stream)
    if data.get('version')!=1 or not re.fullmatch('[A-F0-9]{40}',data.get('signing_fingerprint','')):
        raise Error('invalid existing repository trust configuration')
    data['community_url']=url
    fd,name=tempfile.mkstemp(prefix='.coast-community-',dir=path.parent)
    try:
        with os.fdopen(fd,'w') as stream:json.dump(data,stream,indent=2);stream.write('\n')
        os.chmod(name,0o644);os.replace(name,path)
    finally:Path(name).unlink(missing_ok=True)


def server_config_texts(url, current, pacman, makepkg):
    url = server_origin(url)
    data = json.loads(current)
    if data.get('version') != 1 or not re.fullmatch('[A-F0-9]{40}', data.get('signing_fingerprint', '')):
        raise Error('invalid existing repository trust configuration')
    data['url'] = url
    section = re.search(r'(?ms)^\[coast\][ \t]*\n(?P<body>.*?)(?=^\[|\Z)', pacman)
    if not section:
        raise Error('missing coast repository section in pacman.conf')
    body, count = re.subn(r'(?m)^Server[ \t]*=.*$', 'Server = '+url+'/repo/$arch', section['body'])
    if count != 1:
        raise Error('expected exactly one coast package server')
    pacman = pacman[:section.start('body')]+body+pacman[section.end('body'):]
    pacman = pacman.replace('--proto =file', '--proto =http,https')
    makepkg, count = re.subn(r'(?m)^export COAST_REPOSITORY_URL=.*$', 'export COAST_REPOSITORY_URL='+repr(url), makepkg)
    if count > 1:
        raise Error('ambiguous source repository configuration')
    if not count:
        makepkg = makepkg.rstrip()+'\nexport COAST_REPOSITORY_URL='+repr(url)+'\n'
    return [json.dumps(data, indent=2)+'\n', pacman, makepkg]


def apply_server(url, root=Path('/'), owner=0):
    """Stage all settings; replace individual files atomically, with error rollback.

    The CLI always uses / and root ownership. The root argument exists only for
    filesystem tests; no user-supplied destination or trust key is accepted.
    """
    if root == Path('/') and owner != 0:
        raise Error('system settings require root ownership')
    paths = [root/'etc/coast/packages.json', root/'etc/pacman.conf', root/'etc/makepkg.conf']
    original = []
    for path in paths:
        for parent in [path.parent, root/'etc']:
            info = parent.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != owner or info.st_mode & 0o022:
                raise Error('unsafe repository configuration directory')
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor, 'r') as source:
            info = os.fstat(source.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_uid != owner or info.st_mode & 0o022 or info.st_size > 1024*1024:
                raise Error('unsafe repository configuration file')
            original.append(source.read())
    replacement = server_config_texts(url, *original)
    lock = root/'etc/coast/.server-update.lock'
    descriptor = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    os.close(descriptor)
    staged, replaced = [], []

    def stage(path, text):
        fd, name = tempfile.mkstemp(prefix='.coast-server-', dir=path.parent)
        try:
            with os.fdopen(fd, 'w') as target:
                target.write(text)
            os.chmod(name, 0o644)
            return Path(name)
        except BaseException:
            os.unlink(name)
            raise

    try:
        for path, text in zip(paths, replacement):
            staged.append(stage(path, text))
        for index, (path, temporary) in enumerate(zip(paths, staged)):
            os.replace(temporary, path)
            replaced.append(index)
    except BaseException:
        for index in reversed(replaced):
            os.replace(stage(paths[index], original[index]), paths[index])
        raise
    finally:
        for temporary in staged:
            temporary.unlink(missing_ok=True)
        lock.unlink()


def refresh():
    unprivileged()
    cache = home() / '.cache/coast/packages'
    cache.mkdir(mode=0o700, parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.refresh-', dir=cache) as temporary:
        transaction = Path(temporary)
        directory = transaction / 'catalog'
        directory.mkdir(mode=0o700)
        index, signature = directory / 'index.json', directory / 'index.json.sig'
        download('/recipes/index.json', index)
        download('/recipes/index.json.sig', signature, 16384)
        packages = validate_catalog(index, signature)
        digest = hashlib.sha256(index.read_bytes() + signature.read_bytes()).hexdigest()
        generation = cache / digest
        try:
            os.rename(directory, generation)
        except OSError as error:
            if error.errno not in (errno.EEXIST, errno.ENOTEMPTY):
                raise
            validate_catalog(generation / 'index.json', generation / 'index.json.sig')
        link = transaction / 'current'
        link.symlink_to(generation.name)
        os.replace(link, cache / 'current')
    print(f'Updated signed source catalog: {len(packages)} recipes.')


def repository_packages():
    # Let libalpm load and verify both signed databases. Do not maintain a
    # second, potentially stale package/command index in the helper.
    env = dict(os.environ, LC_ALL='C', LANG='C', COLUMNS='100000')
    metadata = run(['/usr/bin/pacman', '-Si', '--color', 'never'], capture=True, env=env)
    packages, fields, label = [], {}, None
    for line in [*metadata.splitlines(), '']:
        if not line.strip():
            if fields:
                if not all(fields.get(key) for key in ('Repository', 'Name', 'Version', 'Description')):
                    raise Error('incomplete pacman repository metadata')
                if not NAME.fullmatch(fields['Name']):
                    raise Error('invalid package name in repository metadata')
                packages.append({key: fields[key] for key in ('Repository', 'Name', 'Version', 'Description')} |
                                {'Provides': [] if fields.get('Provides', 'None') == 'None' else fields['Provides'].split(),
                                 'Commands': []})
                fields, label = {}, None
        elif line[:1].isspace():
            if label:
                fields[label] += ' ' + line.strip()
        else:
            label, separator, value = line.partition(':')
            if not separator:
                raise Error('invalid pacman repository metadata')
            label = label.strip()
            fields[label] = value.strip()
    by_key = {(package['Repository'], package['Name'], package['Version']): package for package in packages}
    current_versions = {(package['Repository'], package['Name']): package['Version'] for package in packages}
    files = run(['/usr/bin/pacman', '-Fl', '--machinereadable', '--color', 'never'], capture=True, env=env)
    file_repositories = set()
    for line in files.splitlines():
        columns = line.split('\0')
        if len(columns) != 4:
            raise Error('invalid pacman file metadata; refresh with sudo pacman -Fy')
        repository, name, version, filename = columns
        file_repositories.add(repository)
        package = by_key.get((repository, name, version))
        if (repository, name) in current_versions and current_versions[repository, name] != version:
            raise Error('file database is out of date; refresh with sudo pacman -Fy')
        if package and not filename.endswith('/') and filename.count('/') in (1, 2):
            directory, _, command = filename.rpartition('/')
            if directory in ('bin', 'sbin', 'usr/bin', 'usr/sbin'):
                package['Commands'].append(command)
    # Upstream -Fl can exit successfully after an invalid/missing repository
    # yields a null package cache. Never turn that into incomplete search
    # results which appear to have verified command metadata.
    if {package['Repository'] for package in packages} - file_repositories:
        raise Error('invalid or corrupted file database; refresh with sudo pacman -Fy')
    installed = dict(line.split(' ', 1) for line in run(['/usr/bin/pacman', '-Q'], capture=True, env=env).splitlines())
    for package in packages:
        package['Commands'] = sorted(set(package['Commands']))
        package['InstalledVersion'] = installed.get(package['Name'])
    return packages


def provider(packages, name):
    exact = [package for package in packages if package['Name'] == name]
    return exact or [package for package in packages if name in
                     [value.split('=', 1)[0] for value in package.get('Provides', [])]]


def unprivileged():
    if os.getuid() == 0 or os.geteuid() == 0:
        raise Error('run coast as your normal user; sudo authenticates package installation separately')


def home():
    uid = os.getuid()
    if uid == 1000:
        return Path('/home/user')
    if 1000 < uid < 1032:
        return Path('/home/profile-' + str(uid - 1000))
    raise Error('no profile home for this account')


def git(package, arguments, cwd=None, capture=False):
    source=recipe_url(package)
    if not package.get('Community') and arguments[0] in ('clone', 'fetch'):
        with tempfile.TemporaryDirectory(prefix='coast-network-') as temporary:
            download(package['URLPath'] + '/HEAD', Path(temporary)/'HEAD', 16384)
    trust = ['-c', 'safe.directory=' + urlsplit(source).path] if source.startswith('file:') else []
    return run(['/usr/bin/git', *trust, '-c', 'core.hooksPath=/dev/null',
                '-c', 'http.proxy=', '-c', 'protocol.allow=never', '-c', 'protocol.http.allow=always',
                '-c', 'protocol.https.allow=always', '-c', 'protocol.file.allow='+('never' if package.get('Community') else 'always'),
                '-c','core.fsmonitor=false', *arguments],
               cwd=cwd, capture=capture, env=dict(os.environ,GIT_TERMINAL_PROMPT='0',GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL='/dev/null'))


def recipe_url(package):
    if package.get('Community'):
        return server_origin(config()['community_url'])+'/'+package['PackageBase']+'.git'
    return config()['url'].rstrip('/')+package['URLPath']


def review_files(directory):
    result=[directory/'PKGBUILD',directory/'.SRCINFO',*sorted(directory.glob('*.install'))]
    info=directory/'.SRCINFO'
    if not info.is_symlink() and info.is_file() and info.stat().st_size<=1024*1024:
        for value in re.findall(r'^\s*install\s*=\s*(.*?)\s*$',info.read_text(),re.M):
            path=Path(value)
            if path.is_absolute() or '..' in path.parts:raise Error('install script must be inside the recipe checkout')
            result.append(directory/path)
    return list(dict.fromkeys(result))


def validate_checkout(directory):
    for path in review_files(directory):
        if not path.resolve().is_relative_to(directory.resolve()):
            raise Error('recipe review file escapes the checkout: '+str(path))
        info=path.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_size>1024*1024 or info.st_uid!=os.getuid():
            raise Error('recipe review files must be bounded regular files owned by your account: '+str(path))


def printable_recipe(path):
    return ''.join(('\\u%04x'%ord(c)) if c not in '\n\t' and unicodedata.category(c) in ('Cc','Cf') else c
                   for c in path.read_text())


def checkout(package, destination=None):
    unprivileged()
    destination = Path(destination) if destination else home() / 'Builds' / package['PackageBase']
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise Error('destination exists: ' + str(destination))
    source=recipe_url(package)
    git(package, ['clone', '--no-local', '--', str(source), str(destination)])
    commit = git(package, ['rev-parse', 'HEAD'], cwd=destination, capture=True)
    if not package.get('Community') and commit != package['Commit']:
        raise Error('checkout does not match the catalog commit; inspect the repository')
    print('Recipe checkout: ' + str(destination))
    validate_checkout(destination)
    return destination


def show(package):
    if package.get('Community'):
        unprivileged()
        with tempfile.TemporaryDirectory(prefix='coast-review-') as temporary:
            directory=checkout(package,Path(temporary)/'recipe')
            print('Community commit: '+git(package,['rev-parse','HEAD'],cwd=directory,capture=True))
            for path in review_files(directory):print('\n'+path.name+':\n'+printable_recipe(path))
        return
    with tempfile.TemporaryDirectory(prefix='coast-review-') as temporary:
        for name in ['PKGBUILD', *sorted(n for n in package['Files'] if n.endswith('.install'))]:
            path = Path(temporary) / name
            download('/recipes/' + package['PackageBase'] + '/files/' + name, path)
            if hashlib.sha256(path.read_bytes()).hexdigest() != package['Files'][name]:
                raise Error('recipe file does not match the signed catalog: ' + name)
            print('\n' + name + ':\n' + printable_recipe(path))


def review(directory, reviewed):
    validate_checkout(directory)
    print('\nPKGBUILD: ' + str(directory / 'PKGBUILD'))
    for path in review_files(directory):
        print('\nReview file: '+str(path))
        print(printable_recipe(path))
    print('PKGBUILDs are executable shell code. Review sources, patches and install scripts before building.')
    if not reviewed:
        if not sys.stdin.isatty():
            raise Error('review this checkout first; pass --reviewed only after reviewing its files')
        if input('Build this reviewed recipe? [y/N] ').strip().lower() not in ('y', 'yes'):
            raise Error('build cancelled')


def main():
    if len(sys.argv) == 5 and sys.argv[1] == '_configure':
        if os.getuid() != 0 or os.geteuid() != 0:
            raise Error('changing system package settings requires authenticated sudo')
        spec = importlib.util.spec_from_file_location('coast_package_configure', Path(__file__).with_name('configure.py'))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.initialize(server_origin(sys.argv[2]), sys.argv[3], sys.argv[4], server_texts=server_config_texts)
        return
    if len(sys.argv) == 3 and sys.argv[1] in ('_set-server','_set-community'):
        if os.getuid() != 0 or os.geteuid() != 0:
            raise Error('changing system package settings requires authenticated sudo')
        (apply_server if sys.argv[1]=='_set-server' else apply_community)(sys.argv[2])
        return
    if os.uname().sysname != 'Coast':
        raise Error('this client runs inside Coast')
    parser = argparse.ArgumentParser(prog='coast', description='Coast Packages: signed packages and reviewed source recipes.')
    sub = parser.add_subparsers(dest='operation', required=True)
    query = sub.add_parser('search', help='find partial names, commands and descriptions in local packages and recipes')
    query.add_argument('query', nargs='*', help='literal, case-insensitive terms; all terms must match')
    scope = query.add_mutually_exclusive_group()
    scope.add_argument('--source', action='store_true', help='search source recipes only')
    scope.add_argument('--repo', action='store_true', help='search signed binary repositories only')
    scope.add_argument('--community',action='store_true',help='search the community server only')
    output = query.add_mutually_exclusive_group()
    output.add_argument('--json', action='store_true', help='emit structured search results')
    output.add_argument('-q', '--quiet', action='store_true', help='print matching package names only')
    info = sub.add_parser('info', help='show package metadata without executing its PKGBUILD')
    info.add_argument('name')
    info.add_argument('--source', action='store_true', help='show source-recipe metadata')
    info.add_argument('--community',action='store_true',help='look up this exact community recipe')
    show_parser = sub.add_parser('show', help='print the catalog PKGBUILD without executing it')
    show_parser.add_argument('name')
    show_parser.add_argument('--community',action='store_true')
    clone = sub.add_parser('clone', help='clone a local PKGBUILD Git repository')
    clone.add_argument('name')
    clone.add_argument('destination', nargs='?')
    clone.add_argument('--community',action='store_true')
    for operation in ['build', 'install', 'update']:
        command = sub.add_parser(operation, help='build a recipe, build/install it, or update its checkout')
        command.add_argument('name')
        command.add_argument('--community',action='store_true',help='select the reviewed community recipe')
        command.add_argument('--directory', type=Path)
        command.add_argument('--reviewed', action='store_true', help='the caller has reviewed this checkout')
        command.add_argument('--noconfirm', action='store_true', help='pass makepkg/pacman noninteractive confirmation')
        if operation == 'install':
            scope = command.add_mutually_exclusive_group()
            scope.add_argument('--repo', action='store_true', help='install the signed binary package through sudo/pacman')
            scope.add_argument('--source', action='store_true', help='build a reviewed source recipe instead of a binary package')
    sub.add_parser('refresh', help='download and verify the signed source catalog')
    sub.add_parser('repo', help='show the configured repository and signing key')
    configure = sub.add_parser('configure', help='initialize a fresh installation with an explicitly verified repository key')
    configure.add_argument('url')
    configure.add_argument('--key', type=Path, required=True, help='repository public key file obtained from its operator')
    configure.add_argument('--fingerprint', required=True, help='full signing fingerprint verified with the repository operator')
    server = sub.add_parser('server', help='verify or select the server for packages and source builds')
    server.add_argument('url', nargs='?')
    server.add_argument('--check', action='store_true', help='verify signatures and connectivity without changing settings')
    community=sub.add_parser('community',help='show, check, select or disable the community server')
    community.add_argument('url',nargs='?',help='HTTP(S) origin, or off')
    community.add_argument('--check',action='store_true')
    sub.add_parser('list', help='list installed packages')
    upgrade = sub.add_parser('upgrade', help='synchronize and upgrade signed binary packages through sudo/pacman')
    upgrade.add_argument('--noconfirm', action='store_true')
    remove = sub.add_parser('remove', help='remove exact installed package names through sudo/pacman')
    remove.add_argument('names', nargs='+')
    remove.add_argument('--unused', action='store_true', help='also remove dependencies no longer needed')
    remove.add_argument('--noconfirm', action='store_true')
    sub.add_parser('checkupdates', help='compare installed packages with local recipe versions')
    rpc = sub.add_parser('rpc', help='emit Coast source-catalog search/info JSON')
    rpc.add_argument('--type', choices=['search', 'info'], required=True)
    rpc.add_argument('--arg', required=True)
    args = parser.parse_args()
    if args.operation == 'configure':
        unprivileged()
        url = server_origin(args.url)
        fingerprint = args.fingerprint.upper()
        key = args.key.resolve(strict=True)
        spec = importlib.util.spec_from_file_location('coast_package_configure', Path(__file__).with_name('configure.py'))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.public_key(key, fingerprint)
        run(['/bin/sudo', '/usr/bin/python3', '-I', str(Path(__file__).resolve()), '_configure', url, str(key), fingerprint])
        print('Configured Coast Packages: ' + url + '\nSigning key: ' + fingerprint)
        return
    if getattr(args,'community',False) and (getattr(args,'repo',False) or getattr(args,'source',False)):
        parser.error('--community cannot be combined with --repo or --source')
    if args.operation=='community':
        if not args.url and not args.check:
            print('Community server: '+str(config().get('community_url') or 'disabled'));return
        unprivileged()
        url=args.url or config().get('community_url')
        if url!='off':url=check_community(url)
        if not args.check:
            run(['/bin/sudo','/usr/bin/python3','-I',str(Path(__file__).resolve()),'_set-community',url])
            print('Community server '+('disabled' if url=='off' else 'set to '+url))
        return
    if args.operation == 'server':
        if not args.url and not args.check:
            data = config(required=False)
            print('Coast Packages\nServer: '+str(data['url'] or 'not configured')+'\nSigning key: '+str(data['signing_fingerprint'] or 'not configured'))
            return
        unprivileged()
        url = check_server(args.url or config()['url'])
        if not args.check:
            run(['/bin/sudo', '/usr/bin/python3', '-I', str(Path(__file__).resolve()), '_set-server', url])
            print('Package downloads and source builds now use '+url)
        return
    if args.operation == 'refresh':
        refresh()
        return
    if args.operation == 'repo':
        data = config(required=False)
        print('Coast Packages\nServer: ' + str(data['url'] or 'not configured') + '\nSigning key: ' + str(data['signing_fingerprint'] or 'not configured'))
        return
    if args.operation == 'list':
        run(['/usr/bin/pacman', '-Q'])
        return
    if args.operation in ('remove', 'upgrade'):
        unprivileged()
        flags = ['--noconfirm'] if args.noconfirm else []
        if args.operation == 'remove':
            installed = {line.split(' ', 1)[0] for line in run(['/usr/bin/pacman', '-Q'], capture=True).splitlines()}
            if any(not NAME.fullmatch(name) or name not in installed for name in args.names):
                raise Error('remove requires exact installed package names; use coast list')
            run(['/bin/sudo', '/usr/bin/pacman', '-Rs' if args.unused else '-R', *flags, '--', *args.names])
        else:
            run(['/bin/sudo', '/usr/bin/pacman', '-Syu', *flags])
        return
    packages = catalog()
    if args.operation == 'rpc':
        matches = ([packages[args.arg]] if args.arg in packages else []) if args.type == 'info' else search(packages.values(), [args.arg])
        print(json.dumps({'version': 1, 'type': 'info' if args.type == 'info' else 'search',
                          'resultcount': len(matches), 'results': matches}, sort_keys=True))
        return
    if args.operation == 'search':
        candidates = [] if args.repo or args.community else [dict(package, Repository='recipes') for package in packages.values()]
        if not args.source and not args.community:
            candidates.extend(repository_packages())
        if not args.repo:candidates.extend(community_search(args.query,required=args.community))
        matches = search(candidates, args.query)
        if args.json:
            print(json.dumps({'resultcount': len(matches), 'results': matches}, sort_keys=True))
        elif args.quiet:
            print('\n'.join(dict.fromkeys(package['Name'] for package in matches)), end='\n' if matches else '')
        else:
            for package in matches:
                installed = package.get('InstalledVersion')
                status = (' [installed]' if installed == package['Version'] else f' [installed: {installed}]') if installed else ''
                print(f"{package['Repository']}/{package['Name']} {package['Version']}{status}\n    {package['Description']}")
                commands = package.get('Commands', [])
                terms = [term.casefold() for query in args.query for term in query.split()]
                found = [name for name in commands if any(term in name.casefold() for term in terms)]
                if found:
                    print('    Commands: ' + ', '.join(found))
            if not matches:
                print('No packages found' + (': ' + ' '.join(args.query) if args.query else '.'))
        return
    if args.operation == 'checkupdates':
        installed = dict(line.split(' ', 1) for line in run(['/usr/bin/pacman', '-Q'], capture=True).splitlines())
        for name, package in packages.items():
            if name in installed and int(run(['/usr/bin/vercmp', package['Version'], installed[name]], capture=True)) > 0:
                print(f"{name} {installed[name]} -> {package['Version']}")
        if config().get('community_url'):
            query=subprocess.run(['/usr/bin/pacman','-Qm'],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            if query.returncode not in (0,1) or query.stderr.strip():raise Error(query.stderr.strip() or 'cannot list foreign packages')
            foreign=query.stdout.splitlines()
            for name,package in community_info([line.split(' ',1)[0] for line in foreign]).items():
                if int(run(['/usr/bin/vercmp',package['Version'],installed[name]],capture=True))>0:
                    print(f"{name} {installed[name]} -> {package['Version']} [community]")
        return
    explicit_community=getattr(args,'community',False)
    binary = args.operation in ('install', 'info') and not args.source and not explicit_community
    if binary:
        # Installation is an exact package/provides lookup. Partial matching
        # is exclusively for search, never an automatic privileged selection.
        available = repository_packages() if NAME.fullmatch(args.name) else []
        providers = provider(available, args.name)
        if not providers and getattr(args, 'repo', False):
            raise Error('no exact local package: ' + args.name + '; use coast search to find its full name')
        if providers and args.operation == 'info':
            for package in providers:
                run(['/usr/bin/pacman', '-Si', '--', package['Repository'] + '/' + package['Name']])
        elif providers:
            unprivileged()
            run(['/bin/sudo', '/usr/bin/pacman', '-S', *(['--noconfirm'] if args.noconfirm else []), '--', args.name])
        if providers:
            return
    if explicit_community or args.name not in packages:
        found=community_info([args.name]) if config().get('community_url') else {}
        if args.name not in found:raise Error('no exact source recipe: '+args.name+'; use coast search to find its full name')
        package=found[args.name]
    else:package=packages[args.name]
    if args.operation == 'info':
        for label in ['Name', 'PackageBase', 'Version', 'Description', 'Maintainer', 'Depends', 'MakeDepends', 'License', 'Commit']:
            value = package.get(label, [])
            print(f"{label}: " + (', '.join(value) if isinstance(value, list) else str(value)))
        return
    if args.operation == 'show':
        show(package)
        return
    if args.operation == 'clone':
        checkout(package, args.destination)
        return
    unprivileged()
    directory = args.directory or home() / 'Builds' / package['PackageBase']
    if not directory.exists():
        checkout(package, directory)
    if directory.is_symlink() or directory.stat().st_uid != os.getuid():
        raise Error('recipe checkout must be a directory owned by your account')
    if args.operation == 'update':
        source=recipe_url(package)
        if git(package, ['remote', 'get-url', 'origin'], cwd=directory, capture=True) != source:
            raise Error('checkout origin differs from the local catalog')
        git(package, ['fetch', 'origin'], cwd=directory)
        target=git(package,['rev-parse','FETCH_HEAD'],cwd=directory,capture=True) if package.get('Community') else package['Commit']
        if not package.get('Community') and git(package, ['rev-parse', 'FETCH_HEAD'], cwd=directory, capture=True) != target:
            raise Error('remote commit differs from the signed catalog; run coast refresh before updating')
        git(package,['log','--oneline','HEAD..'+target],cwd=directory)
        git(package,['diff','--stat','HEAD',target],cwd=directory)
        git(package, ['merge', '--ff-only', target], cwd=directory)
        validate_checkout(directory)
        print('Review the new Git diff before the next build.')
        return
    review(directory, args.reviewed)
    command = ['/usr/bin/makepkg', '--syncdeps']
    if args.operation == 'install':
        command.append('--install')
    if args.noconfirm:
        command.append('--noconfirm')
    run(command, cwd=directory, env=dict(os.environ, HOME=str(home()), COAST_REPOSITORY_URL=config()['url']))


if __name__ == '__main__':
    try:
        main()
    except (Error, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print('coast: ' + str(error), file=sys.stderr)
        sys.exit(1)
