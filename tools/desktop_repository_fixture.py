"""Disposable signed repository for fresh-image integration qualification."""
import base64
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

from source_tree import digest, EPOCH

ROOT = Path(__file__).resolve().parents[1]


class RepositoryFixture:
    def __init__(self, output):
        self.output = Path(output).resolve()
        self.output.mkdir(parents=True, exist_ok=False)
        self.public = self.output / 'public'
        self.public.mkdir()
        self.keys = self.output / 'private-signing'
        self.keys.mkdir(mode=0o700)
        self.gpg = ['gpg', '--no-options', '--batch', '--homedir', str(self.keys)]
        self.assets = {}

    def host(self, command, cwd=None, env=None):
        result = subprocess.run(list(map(str, command)), cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                capture_output=True, check=True)
        return result.stdout

    def close(self):
        try:
            subprocess.run(['gpgconf', '--homedir', str(self.keys), '--kill', 'gpg-agent'], check=True, capture_output=True)
        finally:
            shutil.rmtree(self.keys)

    def publish(self, path, route):
        path = Path(path).resolve(strict=True)
        self.assets[route] = dict(path=str(path.relative_to(self.public)), sha256=digest(path))

    def sign(self, path):
        self.host([*self.gpg, '--yes', '--pinentry-mode', 'loopback', '--passphrase', '', '--detach-sign', path])

    def tar(self, path, entries):
        with tarfile.open(path, 'w:gz', format=tarfile.USTAR_FORMAT) as archive:
            for name, data in sorted(entries.items()):
                member = tarfile.TarInfo(name)
                member.size, member.mode, member.mtime = len(data), 0o644, EPOCH
                archive.addfile(member, io.BytesIO(data))

    def build(self, nested_iso=None):
        self.host([*self.gpg, '--pinentry-mode', 'loopback', '--passphrase', '', '--quick-generate-key',
                   'Disposable Coast wider integration fixture', 'rsa2048', 'sign', '0'])
        listing = self.host([*self.gpg, '--with-colons', '--list-keys']).decode()
        self.fingerprint, = [line.split(':')[9] for line in listing.splitlines() if line.startswith('fpr:')]
        key = self.public / 'public.asc'
        key.write_bytes(self.host([*self.gpg, '--armor', '--export', self.fingerprint]))
        self.publish(key, '/keys/fixture.asc')
        binary_name, version = 'coast-wide-qa', '1.0-1'
        package = self.public / (binary_name + '-' + version + '-x86_64.pkg.tar.gz')
        filename, data = 'usr/share/coast/wider-integration-qa.txt', b'Coast wider signed transaction\n'
        info = (f'pkgname = {binary_name}\npkgbase = {binary_name}\npkgver = {version}\n'
                f'pkgdesc = Disposable wider integration fixture\nurl = https://example.invalid/\n'
                f'builddate = {EPOCH}\npackager = Coast qualification\nsize = {len(data)}\narch = x86_64\nlicense = 0BSD\ndepend = musl\n')
        mtree = (f'#mtree\n. type=dir uid=0 gid=0 mode=755 time={EPOCH}.0\n' +
                 f'./{filename} type=file uid=0 gid=0 mode=644 time={EPOCH}.0 size={len(data)} sha256digest={hashlib.sha256(data).hexdigest()}\n')
        self.tar(package, {'.PKGINFO': info.encode(), '.MTREE': gzip.compress(mtree.encode(), mtime=EPOCH), filename: data})
        self.sign(package)
        self.publish(package, '/repo/x86_64/' + package.name)
        self.publish(Path(str(package) + '.sig'), '/repo/x86_64/' + package.name + '.sig')
        fields = dict(FILENAME=[package.name], NAME=[binary_name], BASE=[binary_name], VERSION=[version],
            DESC=['Disposable wider integration fixture'], ARCH=['x86_64'], CSIZE=[str(package.stat().st_size)],
            ISIZE=[str(len(data))], SHA256SUM=[digest(package)], LICENSE=['0BSD'], BUILDDATE=[str(EPOCH)],
            PACKAGER=['Coast qualification'], DEPENDS=['musl'],
            PGPSIG=[base64.b64encode(Path(str(package) + '.sig').read_bytes()).decode()])
        desc = ''.join('%' + key + '%\n' + '\n'.join(value) + '\n\n' for key, value in fields.items()).encode()
        for suffix in ('db', 'files'):
            path = self.public / ('coast.' + suffix)
            entries = {binary_name + '-' + version + '/desc': desc}
            if suffix == 'files':
                entries[binary_name + '-' + version + '/files'] = ('%FILES%\n' + filename + '\n\n').encode()
            self.tar(path, entries)
            self.sign(path)
            self.publish(path, '/repo/x86_64/' + path.name)
            self.publish(Path(str(path) + '.sig'), '/repo/x86_64/' + path.name + '.sig')
        work = self.output / 'recipe-work'
        shutil.copytree(ROOT / 'userland/packages/recipes/hello-coast', work)
        (work / '.SRCINFO').write_text('pkgbase = hello-coast\n\tpkgdesc = Reviewed native hello package\n'
            '\tpkgver = 1.1\n\tpkgrel = 1\n\tarch = x86_64\n\tlicense = 0BSD\n'
            '\tdepends = musl\n\tmakedepends = gcc\n\tmakedepends = make\n'
            '\tsource = hello.c\n\tsha256sums = ' + digest(work / 'hello.c') + '\n\npkgname = hello-coast\n')
        environment = dict(PATH='/usr/bin:/bin', HOME=str(self.output), LANG='C.UTF-8',
            GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null',
            GIT_AUTHOR_NAME='Coast qualification', GIT_AUTHOR_EMAIL='qa@coast.invalid',
            GIT_COMMITTER_NAME='Coast qualification', GIT_COMMITTER_EMAIL='qa@coast.invalid',
            GIT_AUTHOR_DATE=f'@{EPOCH} +0000', GIT_COMMITTER_DATE=f'@{EPOCH} +0000')
        git = ['git', '-c', 'core.hooksPath=/dev/null']
        self.host([*git, 'init', '--object-format=sha1', '-b', 'main'], work, environment)
        self.host([*git, 'add', '.'], work, environment)
        self.host([*git, 'commit', '-m', 'Pinned qualification recipe'], work, environment)
        commit = self.host([*git, 'rev-parse', 'HEAD'], work, environment).decode().strip()
        bare = self.public / 'hello-coast.git'
        self.host([*git, 'clone', '--bare', '--no-local', work, bare], env=environment)
        self.host([*git, 'update-server-info'], bare, environment)
        for path in bare.rglob('*'):
            if path.is_file():
                self.publish(path, '/recipes/hello-coast.git/' + str(path.relative_to(bare)))
        files = {}
        for path in work.iterdir():
            if path.is_file():
                copied = self.public / 'recipe-files' / path.name
                copied.parent.mkdir(exist_ok=True)
                shutil.copy2(path, copied)
                self.publish(copied, '/recipes/hello-coast/files/' + path.name)
                files[path.name] = digest(path)
        recipe = dict(ID=1, PackageBaseID=1, Name='hello-coast', PackageBase='hello-coast', Version='1.1-1',
            Description='Reviewed native hello package', Maintainer='coast-qa', Depends=['musl'], MakeDepends=['gcc', 'make'],
            License=['0BSD'], NumVotes=0, Popularity=0.0, OutOfDate=None, FirstSubmitted=EPOCH, LastModified=EPOCH,
            Commit=commit, URLPath='/recipes/hello-coast.git', Files=files)
        catalog = self.public / 'index.json'
        catalog.write_text(json.dumps(dict(version=1, name='Brine', results=[recipe])) + '\n')
        self.sign(catalog)
        self.publish(catalog, '/recipes/index.json')
        self.publish(Path(str(catalog) + '.sig'), '/recipes/index.json.sig')
        if nested_iso:
            path = self.public / 'nested.iso'
            shutil.copy2(nested_iso, path)
            self.publish(path, '/distfiles/nested.iso')
        manifest = self.public / 'repository.json'
        manifest.write_text(json.dumps(dict(target_host_executed=False, signing_secret_included=False,
            public_signing_fingerprint=self.fingerprint, host_files=self.assets, recipes=[recipe],
            packages={binary_name: dict(version=version, description='Disposable wider integration fixture',
                provides=[], license='0BSD', files={filename: {}}, aliases={})}), indent=2) + '\n')
        return manifest
