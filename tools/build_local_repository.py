#!/usr/bin/env python3
"""Produce a signed, offline pacman repository and local Git PKGBUILD catalog.

Only known host generators run here. Guest programs/PKGBUILDs never run on the
host. No host package database, account or system directory is modified.
"""
import argparse
import base64
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import time
from urllib.parse import urlsplit
from repository_base import extract as extract_base
from build_coast_base import rename

ROOT = Path(__file__).resolve().parents[1]
EPOCH = 1793836800


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify(record):
    assert record['target_host_executed'] is False
    if reuse := record.get('reuse'):
        assert sha(reuse['receipt']) == reuse['receipt_sha256']
        for old,proof in reuse['changes'].items():
            original=Path(proof['original_path'])
            assert sha(original)==proof['original_sha256']
            assert rename(original.read_text()).encode()==(ROOT/proof['renamed']).read_bytes()
            assert sha(ROOT/proof['renamed'])==proof['renamed_sha256']
    for name, digest in record['source_sha256'].items():
        assert sha(ROOT / name) == digest, name
    for name, asset in record.get('files', {}).items():
        assert not PurePosixPath(name).is_absolute() and '..' not in PurePosixPath(name).parts
        assert sha(asset['path']) == asset['sha256'], name
    for path, digest in record.get('artifacts', {}).items():
        assert sha(path) == digest, path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ports', type=Path, default=ROOT / 'build/program-ports-r2/BUILD.json')
    parser.add_argument('--arch-build', type=Path, default=ROOT / 'build/arch-ports-r1')
    parser.add_argument('--arch-receipt',type=Path)
    parser.add_argument('--support-receipt',type=Path)
    parser.add_argument('--gcc-build',type=Path,help='fresh native GCC platform-branding rebuild')
    parser.add_argument('--qt-build', type=Path, default=ROOT / 'build/qt6/port-r2/BUILD.json')
    parser.add_argument('--base-iso', type=Path, default=ROOT / 'build/Coast-base.iso',
                        help='qualified base containing the actual installed native toolchain')
    parser.add_argument('--repository-mode', choices=('host', 'embedded'), default='host',
                        help='host serves packages/Git/sources over HTTP; embedded is an optional offline image')
    parser.add_argument('--guest-url', default='http://10.0.2.2:8765', help='VM-visible address of the host repository')
    parser.add_argument('--community-url',default='http://10.0.2.2:8770',help='VM-visible AUR v5 community server origin')
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--signing-home', type=Path, required=True, help='private host-only GnuPG home')
    args = parser.parse_args()
    endpoint = urlsplit(args.guest_url)
    if endpoint.scheme not in ('http', 'https') or not endpoint.hostname or endpoint.username or endpoint.password or endpoint.query or endpoint.fragment or endpoint.path not in ('', '/'):
        parser.error('--guest-url must be a plain HTTP(S) server origin')
    guest_url = args.guest_url.rstrip('/') if args.repository_mode == 'host' else 'file:///usr/share/coast/packages/repository'
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=False)
    arch = args.arch_build.resolve()
    keys = args.signing_home.resolve()
    assert keys.is_dir() and keys.stat().st_mode & 0o077 == 0
    assert not keys.is_relative_to(out), 'Signing secrets must stay outside the image output'
    conf = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--ro-bind', '/', '/',
            '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out),
            '--bind', str(keys), str(keys)]
    env = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
               GNUPGHOME=str(keys), SOURCE_DATE_EPOCH=str(EPOCH),
               MAKEPKG_LIBRARY=str(arch / 'install/usr/share/makepkg'),
               GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null',
               GIT_AUTHOR_NAME='Coast local ports', GIT_AUTHOR_EMAIL='ports@coast.local',
               GIT_COMMITTER_NAME='Coast local ports', GIT_COMMITTER_EMAIL='ports@coast.local',
               GIT_AUTHOR_DATE=f'@{EPOCH} +0000', GIT_COMMITTER_DATE=f'@{EPOCH} +0000')
    commands = []

    def host(command, cwd=out, data=None):
        command = list(map(str, command))
        commands.append(command)
        result = subprocess.run([*conf, '--chdir', str(cwd), '--', *command], env=env,
                                input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode:
            raise RuntimeError(str(command[:2]) + ': ' + result.stderr.decode(errors='replace')[-2500:])
        return result.stdout

    sources = [Path(__file__), ROOT / 'tools/repository_base.py', ROOT/'tools/build_coast_base.py', ROOT/'tools/migrate_coast_receipt.py', ROOT / 'tools/serve_repository.py', ROOT/'tools/export_repository.py', ROOT / 'userland/packages/client.py', ROOT/'userland/packages/search.py', ROOT/'userland/packages/community.py',ROOT / 'userland/packages/launcher.c',
               ROOT/'Documentation/local-repositories.md',ROOT/'userland/runtime/usr/lib/coast/linux-ports/manifest.json',
               *sorted((ROOT/'deployment/repository').glob('*')),
               *sorted((ROOT / 'userland/packages/web').glob('*')),
               *sorted((ROOT / 'userland/packages/config').glob('*')),
               *sorted(p for p in (ROOT / 'userland/packages/recipes').rglob('*') if p.is_file())]
    inputs = {str(p.relative_to(ROOT)): sha(p) for p in sources}
    ports = json.loads(args.ports.read_text())
    verify(ports)
    for receipt in [args.arch_receipt or arch/'COMPILATION.json', args.support_receipt or arch/'SUPPORT-COMPILATION.json']:
        record=json.loads(receipt.read_text())
        assert record.get('artifacts'), 'Complete compiled-artifact receipt required: ' + str(receipt)
        verify(record)
    files, aliases, host_files = {}, {}, {}
    # Pacman's public trust store must be readable for unprivileged queries.
    # This is upstream pacman-key's 0755 directory / 0644 public-file layout.
    directories = {'etc/pacman.d/gnupg': 0o755, 'var/lib/pacman/local': 0o755,
                   'var/lib/pacman/sync': 0o755, 'var/cache/pacman/pkg': 0o755,
                   'etc/pacman.d/hooks': 0o755, 'var/log': 0o755}
    packages = {}
    owners = {}

    def package(name, version, description, depends=(), provides=(), license='GPL-3.0-or-later', reason=0):
        assert name not in packages, 'Duplicate package: ' + name
        packages[name] = dict(name=name, version=version, description=description, depends=list(depends),
                              provides=list(provides), license=license, reason=reason, files={}, aliases={}, directories=[], backup=[])
        return name

    def add(name, path, owner=None, mode=None):
        path = Path(path).resolve()
        assert not name.startswith('/') and '..' not in PurePosixPath(name).parts
        if name in files:
            assert files[name]['sha256'] == sha(path), name
            assert owner is None or owners.get(name) == owner, 'Conflicting file ownership: ' + name
            assert mode is None or files[name]['mode'] == mode, 'Conflicting file mode: ' + name
            return
        if mode is None:
            mode = 0o755 if name.startswith('usr/bin/') or path.stat().st_mode & 0o111 else 0o644
        assert mode in (0o600, 0o644, 0o755)
        files[name] = dict(path=str(path), sha256=sha(path), mode=mode)
        if owner:
            assert name not in owners
            owners[name] = owner
            packages[owner]['files'][name] = files[name]

    def alias(name, target, owner):
        assert name not in aliases and name not in files
        aliases[name] = target
        owners[name] = owner
        packages[owner]['aliases'][name] = target

    def generated(name, content, owner=None, mode=0o644):
        path = out / 'generated' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content.encode() if isinstance(content, str) else content)
        add(name, path, owner, mode)

    def publish(url, path):
        path = Path(path).resolve()
        assert path.is_relative_to(out) and path.is_file(), url
        assert url not in host_files and url.startswith(('/repo/', '/recipes/', '/distfiles/', '/keys/'))
        host_files[url] = dict(path=str(path), sha256=sha(path))
        if args.repository_mode == 'embedded':
            add('usr/share/coast/packages/repository' + url, path,
                None if url.startswith('/repo/') else 'coast-packages', 0o644)

    base = package('coast-base', '1.0-2', 'Coast native kernel, desktop and musl runtime',
                   provides=['musl=1.2.5', 'gcc-libs=14.2.0', 'curl', 'gzip', 'sudo', 'busybox'],
                   license='custom')
    add('usr/bin/curl', ROOT / 'userland/bin/curl', base, 0o755)
    generated('etc/os-release','NAME=Coast\nID=coast\nPRETTY_NAME="Coast (Live)"\nVERSION_ID=0.2\n',base)
    generated('etc/hostname','coast\n',base)
    generated('etc/issue','Coast live system\n',base)
    for name, version, description, depends, provides, license in [
        ('binutils', '2.37', 'GNU assembler, linker and binary utilities', ['musl'], [], 'GPL-3.0-or-later'),
        ('gcc', '14.2.0', 'GNU C and C++ compilers: gcc, g++, cc, c++ and cpp', ['musl', 'gcc-libs', 'binutils'],
         ['g++=14.2.0', 'cc', 'c++', 'cpp'], 'GPL-3.0-or-later WITH GCC-exception-3.1'),
        ('make', '4.4.1', 'GNU Make build automation', ['musl'], [], 'GPL-3.0-or-later'),
        ('gawk', '5.4.0', 'GNU Awk pattern scanning and processing language', ['musl'], ['awk'], 'GPL-3.0-or-later'),
        ('git', '2.56.0', 'Git distributed version control system', ['musl'], [], 'GPL-2.0-only'),
        ('nano', '8.7', 'GNU nano terminal text editor', ['musl'], [], 'GPL-3.0-or-later'),
        ('vim', '9.2.0000', 'Vim Vi IMproved terminal text editor', ['musl'], ['vi'], 'Vim'),
    ]:
        package(name, version + '-1', description, depends, provides, license)
    base_files, base_aliases, base_receipt = extract_base(args.base_iso, out / 'base-files', host)
    if args.gcc_build:
        gcc=json.loads(args.gcc_build.read_text())
        assert gcc['target_host_executed'] is False
        for command,asset in gcc['files'].items():
            assert sha(asset['path'])==asset['sha256']
            names=['usr/bin/'+command]
            if command in ('gcc','g++'):names += [name for name in ['usr/bin/'+('cc' if command=='gcc' else 'c++')] if name in base_files]
            if command in ('cc1','cc1plus'):names=['usr/libexec/gcc/x86_64-linux-musl/14.2.0/'+command]
            for name in names:
                assert name in base_files, name
                base_files[name]['path']=asset['path']
    for name, asset in base_files.items():
        add(name, asset['path'], asset['owner'], asset['mode'])
    for name, asset in base_aliases.items():
        alias(name, asset['target'], asset['owner'])
    # These tools used to live only in /bin. Keep the exact qualified bytes
    # there and supply conventional /usr/bin paths owned by the same package.
    for command, owner in [('make', 'make'), ('gawk', 'gawk'), ('nano', 'nano'), ('vim', 'vim')]:
        if 'usr/bin/' + command not in files:
            add('usr/bin/' + command, base_files['bin/' + command]['path'], owner, 0o755)
    for command, target, owner in [('awk', 'gawk', 'gawk'), ('vi', 'vim', 'vim'), ('view', 'vim', 'vim'), ('vimdiff', 'vim', 'vim')]:
        if 'usr/bin/' + command not in files and 'usr/bin/' + command not in aliases:
            alias('usr/bin/' + command, target, owner)
    if 'etc/nanorc' in packages['nano']['files']:
        packages['nano']['backup'].append('etc/nanorc')
    names = {}
    descriptions = {'python':'Python programming language and standard library', 'jq':'Command line JSON processor',
                    'ninja':'Small fast build system', 'pkgconf':'Package configuration dependency resolver',
                    'm4':'GNU macro processor', 'patch':'GNU patch utility', 'gperf':'Perfect hash generator',
                    'bc':'GNU arbitrary precision calculator and dc', 'zstd':'Zstandard compression tools',
                    'lz4':'LZ4 compression tools', 'xz':'XZ compression tools', 'rsync':'Incremental file transfer utility'}
    licenses={'python':'custom:Python','jq':'MIT','ninja':'Apache-2.0','pkgconf':'ISC',
              'zstd':'BSD-3-Clause OR GPL-2.0-or-later','lz4':'GPL-2.0-or-later','xz':'0BSD'}
    for name, record in ports['packages'].items():
        names[name] = package(name, record['version'] + '-1', descriptions[name], ['musl'],
                              provides=['patch'] if name == 'patch' else ['xz'] if name == 'xz' else ['pkg-config'] if name=='pkgconf' else [],
                              license=licenses.get(name,'GPL-3.0-or-later'))
    for name, asset in ports['files'].items():
        owner = next((n for n in names if f'/licenses/{n}/' in name or f'/licenses/extra-programs/{n}/' in name or Path(name).name == ports['packages'][n]['archive']), base)
        if name.startswith('usr/bin/'):
            command = Path(name).name
            owner = {'python3.14': 'python', 'linux-patch': 'patch', 'linux-xz': 'xz', 'dc': 'bc'}.get(command, command)
        elif name.startswith('usr/lib/python'):
            owner = 'python'
        add(name, asset['path'], owner)
    for name, target in ports['aliases'].items():
        alias(name, target, owners['usr/bin/' + target])

    pacman = package('pacman', '7.1.0-2', 'Upstream pacman with Landlock and seccomp downloads, libalpm and makepkg',
                     ['bash', 'gnupg', 'libarchive', 'coreutils', 'findutils', 'file'],license='GPL-2.0-or-later')
    gnupg = package('gnupg', '2.5.24-1', 'GnuPG OpenPGP verification and agent', ['musl'])
    pacman_bins = {'pacman', 'pacman-conf', 'vercmp', 'makepkg', 'makepkg-template', 'pacman-db-upgrade',
                   'pacman-key', 'repo-add', 'repo-remove'}
    for path in sorted((arch / 'install/usr').rglob('*')):
        relative = str(path.relative_to(arch / 'install'))
        if path.is_symlink():
            if relative.startswith('usr/bin/'):
                alias(relative, os.readlink(path), pacman if path.name in pacman_bins else gnupg)
            continue
        if not path.is_file() or not (relative.startswith(('usr/bin/', 'usr/share/makepkg/', 'usr/share/pacman/', 'usr/share/gnupg/'))):
            continue
        owner = pacman if path.name in pacman_bins or relative.startswith(('usr/share/makepkg/', 'usr/share/pacman/')) else gnupg
        add(relative, path, owner)
    libarchive = package('libarchive', '3.8.11-2', 'BSD tar/cpio with OpenSSL package-file checksums', ['musl'],license='BSD-2-Clause')
    for name in ['bsdtar', 'bsdcpio', 'bsdcat', 'bsdunzip']:
        add('usr/bin/' + name, arch / 'prefix/bin' / name, libarchive)
    package('libseccomp','2.6.1-1','Upstream Linux seccomp filter library and development headers',['musl'],license='LGPL-2.1-only')
    add('usr/include/seccomp.h',arch/'prefix/include/seccomp.h','libseccomp',0o644)
    add('usr/include/seccomp-syscalls.h',arch/'prefix/include/seccomp-syscalls.h','libseccomp',0o644)
    add('usr/lib/libseccomp.a',arch/'prefix/lib/libseccomp.a','libseccomp',0o644)
    generated('usr/lib/pkgconfig/libseccomp.pc','prefix=/usr\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: libseccomp\nDescription: Seccomp filter library\nVersion: 2.6.1\nLibs: -L${libdir} -lseccomp\nCflags: -I${includedir}\n','libseccomp')
    support = arch / 'support-install'
    for name, version, bins in [
            ('coreutils', '9.12', None), ('findutils', '4.11.0', {'find', 'xargs'}),
            ('file', '5.48', {'file'}), ('bash', '5.3.20', {'bash', 'bashbug'}),
            ('fakeroot', '2.1.4', {'fakeroot', 'faked'})]:
        package(name, version + '-1', 'Upstream ' + name + ' for native package builds', ['musl'], reason=1,
                license='BSD-2-Clause' if name=='file' else 'GPL-3.0-or-later')
        for path in sorted((support / 'usr/bin').iterdir()):
            if (bins is None and path.name in {'find', 'xargs', 'file', 'bash', 'bashbug', 'fakeroot', 'faked'}) or (bins is not None and path.name not in bins):
                continue
            relative = 'usr/bin/' + path.name
            if path.is_symlink():
                alias(relative, os.readlink(path), name)
            elif path.name == 'bashbug':
                generated(relative, rename(path.read_text()), name, 0o755)
            else:
                add(relative, path, name)
    add('usr/share/misc/magic.mgc', support / 'usr/share/misc/magic.mgc', 'file')
    for path in sorted((support / 'usr/lib/libfakeroot').iterdir()):
        if path.suffix == '.la':
            continue
        relative = 'usr/lib/libfakeroot/' + path.name
        if path.is_symlink():
            alias(relative, os.readlink(path), 'fakeroot')
        else:
            add(relative, path, 'fakeroot', 0o755)
    for path in (support / 'usr/libexec/coreutils').glob('*'):
        if path.is_file():
            add('usr/libexec/coreutils/' + path.name, path, 'coreutils', 0o755)
    old_ports=json.loads((ROOT/'userland/runtime/usr/lib/coast/linux-ports/manifest.json').read_text())
    for record in old_ports['packages']:
        name=record['name']
        # The selected desktop carries a later interactive Bash port. Keep
        # that base component; this new Bash package supplies /usr/bin/bash
        # for makepkg, without replacing the qualified terminal shell.
        if name=='bash':continue
        if name!='bash':
            package(name,record['version']+'-1','Upstream '+name+' native tools',['musl'],
                    license='MIT' if name=='lua' else 'custom:PublicDomain' if name=='sqlite' else 'GPL-3.0-or-later')
        for command in record['programs']:
            path=ROOT/'userland/runtime'/command['path']
            assert sha(path)==command['sha256']
            add(command['path'],path,name,0o755)
            alias(command['alias'],'/'+command['path'],name)
            if name!='bash':alias('usr/bin/'+command['program'],'/'+command['path'],name)
        license_root=ROOT/'userland/runtime/usr/share/licenses/linux-ports'/name
        for path in sorted(license_root.rglob('*')):
            if path.is_file():add(str(path.relative_to(ROOT/'userland/runtime')),path,name,0o644)
        source=ROOT/'userland/vendor/linux-ports'/record['source']['archive']
        assert sha(source)==record['source']['sha256']
        add('usr/src/coast/package-tools/'+source.name,source,name,0o644)
    # This BusyBox ash configuration has no kill builtin/applet. Use Bash's
    # actual kill implementation so upstream daemon cleanup works normally.
    generated('usr/bin/kill','#!/usr/bin/bash\nkill "$@"\n','bash',0o755)
    packages['fakeroot']['depends'].append('bash')
    # Convenient names select the already validated native GNU ports.
    for command, target in [('patch', 'linux-patch'), ('xz', 'linux-xz'), ('unxz', 'linux-xz'), ('xzcat', 'linux-xz')]:
        alias('usr/bin/' + command, target, owners['usr/bin/' + target])

    qt = json.loads(args.qt_build.read_text())
    assert qt['target_host_executed'] is False and sha(qt['binary']) == qt['binary_sha256']
    for name, digest in qt['source_sha256'].items():
        assert sha(ROOT / name) == digest
    package('qt6-demo', qt['version'] + '-1', 'Native Qt 6 widgets demonstration', ['musl'], license='LGPL-3.0-only')
    add('usr/bin/qt6-demo', qt['binary'], 'qt6-demo')
    add('usr/src/qt6/qtbase-' + qt['version'] + '.tar.xz',
        ROOT / ('build/qt6/vendor/qtbase-everywhere-src-' + qt['version'] + '.tar.xz'), 'qt6-demo', 0o644)
    for path in sorted((ROOT / 'userland/qt6').iterdir()):
        if path.is_file():
            add('usr/src/coast/qt6/' + path.name, path, 'qt6-demo', 0o644)
    for path in sorted((Path(qt['source_directory']) / 'LICENSES').iterdir()):
        if path.is_file():
            add('usr/share/licenses/qt6/' + path.name, path, 'qt6-demo', 0o644)

    package('base-devel', '1.0-2', 'Native tools required for local PKGBUILD builds',
            ['coast-base', 'gcc', 'binutils', 'make', 'gawk', 'git', 'bash', 'coreutils', 'findutils', 'file', 'fakeroot', 'pacman', 'patch', 'pkgconf', 'm4','grep','sed','diffutils'], reason=1)
    package('coast-packages', '1.5-1', 'Coast Packages: signed packages and reviewed community recipes', ['python', 'pacman', 'base-devel'], license='0BSD')
    host([ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-gcc', '-O2', '-static', '-no-pie',
          '-Wl,-z,relro,-z,now,-z,noexecstack', ROOT / 'userland/packages/launcher.c', '-o', out / 'coast'])
    add('usr/bin/coast', out / 'coast', 'coast-packages', 0o755)
    add('usr/lib/coast/packages/client.py', ROOT / 'userland/packages/client.py', 'coast-packages', 0o644)
    add('usr/lib/coast/packages/search.py', ROOT / 'userland/packages/search.py', 'coast-packages', 0o644)
    add('usr/lib/coast/packages/community.py',ROOT/'userland/packages/community.py','coast-packages',0o644)
    add('usr/share/doc/coast/local-repositories.md',ROOT/'Documentation/local-repositories.md','coast-packages',0o644)
    for name in ['pacman.conf', 'makepkg.conf']:
        content = (ROOT / 'userland/packages/config' / name).read_text()
        if name == 'pacman.conf':
            content = content.replace('Server = file:///var/lib/coast/repository/coast/os/$arch', 'Server = ' + guest_url + '/repo/$arch')
            content = content.replace('--proto =file', '--noproxy "*" --connect-timeout 5 --max-time 180 --proto =' + ('file' if args.repository_mode == 'embedded' else 'http,https'))
        else:
            content += '\n# Native source recipes use the same local server as binary packages.\nexport COAST_REPOSITORY_URL=' + repr(guest_url) + '\n'
        generated('etc/' + name, content, pacman, 0o644)
        packages[pacman]['backup'].append('etc/' + name)

    fingerprint = next(line.split(':')[9] for line in host(['/usr/bin/gpg', '--batch', '--with-colons', '--list-keys']).decode().splitlines() if line.startswith('fpr:'))
    public = host(['/usr/bin/gpg', '--batch', '--armor', '--export', fingerprint])
    generated('usr/share/pacman/keyrings/coast-local.asc', public, pacman)
    keyring = out / 'public-keyring'
    keyring.mkdir(mode=0o700)
    public_path = out / 'repo-public.asc'
    public_path.write_bytes(public)
    publish('/keys/coast-local.asc', public_path)
    generated('etc/coast/packages.json', json.dumps(dict(version=1, url=guest_url,
              signing_fingerprint=fingerprint,community_url=args.community_url or None), indent=2) + '\n', 'coast-packages')
    packages['coast-packages']['backup'].append('etc/coast/packages.json')
    host(['/usr/bin/gpg', '--batch', '--no-autostart', '--homedir', keyring, '--import', public_path])
    host(['/usr/bin/gpg', '--batch', '--homedir', keyring, '--import-ownertrust'], data=(fingerprint + ':6:\n').encode())
    # Resolve the trust graph now, while writable, so read-only user queries
    # never need to create a trustdb lock or trigger a first-run rebuild.
    host(['/usr/bin/gpg','--batch','--homedir',keyring,'--check-trustdb'])
    generated('etc/pacman.d/gnupg/pubring.gpg',host(['/usr/bin/gpg','--batch','--homedir',keyring,'--export']),mode=0o644)
    add('etc/pacman.d/gnupg/trustdb.gpg',keyring/'trustdb.gpg',mode=0o644)
    generated('etc/pacman.d/gnupg/gpg.conf','no-greeting\nno-permission-warning\nno-auto-key-retrieve\n')
    assert not any((keyring / 'private-keys-v1.d').glob('*'))
    passwd = ['root:!:0:0:root:/root:/bin/sh', 'alpm:!:990:990:Package downloader:/:/bin/false',
              'user:!:1000:1000:Desktop user:/home/user:/bin/sh']
    groups = ['root:!:0:', 'alpm:!:990:', 'user:!:1000:']
    for i in range(1, 32):
        passwd.append(f'profile-{i}:!:{1000+i}:{1000+i}:Profile {i}:/home/profile-{i}:/bin/sh')
        groups.append(f'profile-{i}:!:{1000+i}:')
    generated('etc/passwd', '\n'.join(passwd) + '\n', base)
    generated('etc/group', '\n'.join(groups) + '\n', base)

    results = []
    recipe_metadata = {
        'hello-coast': ('1.1-1', 'Native example built from a reviewed PKGBUILD', ['musl'], ['gcc', 'make'], ['0BSD']),
        'bc': ('1.08.2-3', 'GNU arbitrary precision calculator and dc', ['musl'], ['gcc', 'make'], ['GPL-3.0-or-later']),
        'ninja': ('1.13.2-4', 'Ninja build system', ['musl', 'gcc-libs'], ['gcc', 'python', 'patch'], ['Apache-2.0']),
    }
    for name, (version, description, depends, makedepends, licenses) in recipe_metadata.items():
        work = out / 'recipe-work' / name
        shutil.copytree(ROOT / 'userland/packages/recipes' / name, work)
        srcinfo = f'pkgbase = {name}\n\tpkgdesc = {description}\n\tpkgver = {version.rsplit("-",1)[0]}\n\tpkgrel = {version.rsplit("-",1)[1]}\n\tarch = x86_64\n'
        for key, values in [('license', licenses), ('depends', depends), ('makedepends', makedepends)]:
            srcinfo += ''.join(f'\t{key} = {value}\n' for value in values)
        if name == 'hello-coast':
            srcinfo += '\tsource = hello.c\n\tsha256sums = ' + sha(work / 'hello.c') + '\n'
        else:
            source = ports['packages'][name]
            srcinfo += '\turl = ' + ('https://www.gnu.org/software/bc/' if name == 'bc' else 'https://ninja-build.org/') + '\n'
            srcinfo += '\tsource = ' + guest_url + '/distfiles/' + source['archive'] + '\n\tsha256sums = ' + source['sha256'] + '\n'
            if name == 'ninja':
                for source in (ROOT / 'ports/qemu/ninja-session.patch', ROOT / 'ports/qemu/ninja_check.py'):
                    srcinfo += '\tsource = ' + guest_url + '/distfiles/' + source.name + '\n\tsha256sums = ' + sha(source) + '\n'
        # makepkg --printsrcinfo may regenerate authoritative metadata after
        # checkout; catalog creation never evaluates a recipe on the host.
        srcinfo += f'\npkgname = {name}\n'
        (work / '.SRCINFO').write_text(srcinfo)
        host(['/usr/bin/git', 'init', '--initial-branch=master', work])
        host(['/usr/bin/git', '-C', work, 'add', '.'])
        host(['/usr/bin/git', '-C', work, 'commit', '-m', 'Initial local PKGBUILD'])
        commit = host(['/usr/bin/git', '-C', work, 'rev-parse', 'HEAD']).decode().strip()
        bare = out / 'catalog' / (name + '.git')
        bare.parent.mkdir(exist_ok=True)
        host(['/usr/bin/git', 'clone', '--bare', '--no-local', work, bare])
        host(['/usr/bin/git','--git-dir='+str(bare),'remote','remove','origin'])
        host(['/usr/bin/git','--git-dir='+str(bare),'update-server-info'])
        for path in sorted(bare.rglob('*')):
            suffix = str(path.relative_to(bare))
            if path.is_dir() and args.repository_mode == 'embedded':
                relative=f'usr/share/coast/packages/repository/recipes/{name}.git/'+suffix
                directories[relative]=0o755
                packages['coast-packages']['directories'].append(relative)
            if path.is_file() and (suffix == 'HEAD' or suffix == 'info/refs' or suffix.startswith('objects/')):
                publish(f'/recipes/{name}.git/' + suffix, path)
            elif path.is_file() and args.repository_mode == 'embedded' and suffix in ('config', 'packed-refs'):
                add(f'usr/share/coast/packages/repository/recipes/{name}.git/' + suffix, path, 'coast-packages', 0o644)
        recipe_files = {}
        for path in sorted(work.iterdir()):
            if path.is_file():
                publish(f'/recipes/{name}/files/' + path.name, path)
                recipe_files[path.name] = sha(path)
        results.append(dict(ID=len(results)+1, PackageBaseID=len(results)+1, Name=name, PackageBase=name,
                            Version=version, Description=description, Maintainer='coast-local', Depends=depends,
                            MakeDepends=makedepends, License=licenses, NumVotes=0, Popularity=0.0, OutOfDate=None,
                            FirstSubmitted=EPOCH, LastModified=EPOCH, Commit=commit,
                            URLPath=f'/recipes/{name}.git', Files=recipe_files))
    catalog = out / 'catalog/index.json'
    catalog.write_text(json.dumps(dict(version=1, name='Coast Packages', results=results), indent=2) + '\n')
    host(['/usr/bin/gpg', '--batch', '--yes', '--pinentry-mode', 'loopback', '--passphrase', '',
          '--local-user', fingerprint, '--detach-sign', catalog])
    publish('/recipes/index.json', catalog)
    publish('/recipes/index.json.sig', str(catalog) + '.sig')
    add('usr/share/coast/packages/catalog.json', catalog, 'coast-packages', 0o644)
    add('usr/share/coast/packages/catalog.json.sig', str(catalog) + '.sig', 'coast-packages', 0o644)
    for name in ['bc', 'ninja']:
        record = ports['packages'][name]
        path = ROOT / 'build/program-ports-r2/vendor' / record['archive']
        assert sha(path) == record['sha256']
        published = out / 'distfiles' / record['archive']
        published.parent.mkdir(exist_ok=True)
        shutil.copyfile(path, published)
        publish('/distfiles/' + record['archive'], published)
    for path in (ROOT / 'ports/qemu/ninja-session.patch', ROOT / 'ports/qemu/ninja_check.py'):
        published = out / 'distfiles' / path.name
        shutil.copyfile(path, published)
        publish('/distfiles/' + path.name, published)
    for owner, source in [('pacman', arch / 'pacman-v7.1.0'), ('gnupg', arch / 'gnupg-2.5.24'),
                          ('libarchive', arch / 'libarchive-3.8.11'), ('coreutils', arch / 'coreutils-9.12'),
                          ('findutils', arch / 'findutils-4.11.0'), ('file', arch / 'file-5.48'),
                          ('bash', arch / 'bash-5.3-dynamic'), ('fakeroot', arch / 'fakeroot-unix-2.1.4')]:
        for name in ['COPYING', 'COPYING.LIB', 'COPYINGv3', 'COPYINGv2', 'LICENSE', 'COPYRIGHT']:
            path = source / name
            if path.is_file():
                add('usr/share/licenses/' + owner + '/' + name, path, owner, 0o644)

    def escape(value):
        return ''.join('\\%03o' % byte if byte <= 32 or byte in (35, 92, 127) else chr(byte) for byte in value.encode())

    def archive(package):
        entries = dict(package['files'])
        links = package['aliases']
        dirs = set(package['directories'])
        for name in [*entries, *links]:
            dirs.update(str(p) for p in PurePosixPath(name).parents if str(p) != '.')
        size = sum(Path(a['path']).stat().st_size for a in entries.values())
        info = f"pkgname = {package['name']}\npkgbase = {package['name']}\npkgver = {package['version']}\npkgdesc = {package['description']}\nurl = https://coast.local/\nbuilddate = {EPOCH}\npackager = Coast local ports\nsize = {size}\narch = x86_64\nlicense = {package['license']}\n"
        for key, values in [('depend', package['depends']), ('provides', package['provides']), ('backup', package['backup'])]:
            info += ''.join(f'{key} = {value}\n' for value in values)
        # Canonical mtree root is '.', not './'. libalpm strips './' from
        # file names, so the latter becomes an empty file-list entry and
        # falsely conflicts with every other package in a transaction.
        mtree = ['#mtree', f'. type=dir uid=0 gid=0 mode=755 time={EPOCH}.0']
        for name in sorted(dirs):
            mtree.append(f'./{escape(name)} type=dir uid=0 gid=0 mode=755 time={EPOCH}.0')
        for name, asset in sorted(entries.items()):
            mtree.append(f'./{escape(name)} type=file uid=0 gid=0 mode={asset["mode"]:o} size={Path(asset["path"]).stat().st_size} time={EPOCH}.0 sha256digest={asset["sha256"]}')
        for name, target in sorted(links.items()):
            mtree.append(f'./{escape(name)} type=link uid=0 gid=0 mode=777 time={EPOCH}.0 link={escape(target)}')
        path = out / 'repo' / f"{package['name']}-{package['version']}-x86_64.pkg.tar.gz"
        path.parent.mkdir(exist_ok=True)
        with path.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', mtime=EPOCH, filename='') as compressed, tarfile.open(fileobj=compressed, mode='w|', format=tarfile.PAX_FORMAT) as tar:
            def put(name, data=None, mode=0o644, kind=tarfile.REGTYPE, target=''):
                entry = tarfile.TarInfo(name)
                entry.uid = entry.gid = 0
                entry.mtime, entry.mode, entry.type, entry.linkname = EPOCH, mode, kind, target
                entry.size = len(data) if data else 0
                tar.addfile(entry, io.BytesIO(data) if data else None)
            put('.PKGINFO', info.encode())
            mtree_data = gzip.compress(('\n'.join(mtree) + '\n').encode(), mtime=EPOCH)
            put('.MTREE', mtree_data)
            for name in sorted(dirs):
                put(name + '/', mode=0o755, kind=tarfile.DIRTYPE)
            for name, asset in sorted(entries.items()):
                put(name, Path(asset['path']).read_bytes(), asset['mode'])
            for name, target in sorted(links.items()):
                put(name, kind=tarfile.SYMTYPE, mode=0o777, target=target)
        host(['/usr/bin/gpg', '--batch', '--yes', '--pinentry-mode', 'loopback', '--passphrase', '',
              '--local-user', fingerprint, '--detach-sign', path])
        host(['/usr/bin/gpg', '--batch', '--verify', str(path) + '.sig', path])
        desc = {'NAME': [package['name']], 'VERSION': [package['version']], 'BASE': [package['name']],
                'DESC': [package['description']], 'URL': ['https://coast.local/'], 'ARCH': ['x86_64'],
                'BUILDDATE': [str(EPOCH)], 'INSTALLDATE': [str(EPOCH)], 'PACKAGER': ['Coast local ports'],
                'SIZE': [str(size)], 'REASON': [str(package['reason'])], 'LICENSE': [package['license']],
                'VALIDATION': ['pgp'], 'DEPENDS': package['depends'], 'PROVIDES': package['provides']}
        directory = f"var/lib/pacman/local/{package['name']}-{package['version']}"
        generated(directory + '/desc', ''.join('%' + k + '%\n' + '\n'.join(v) + '\n\n' for k, v in desc.items() if v))
        filelist = sorted([*(d + '/' for d in dirs), *entries, *links])
        dbfiles = '%FILES%\n' + '\n'.join(filelist) + '\n\n'
        if package['backup']:
            dbfiles += '%BACKUP%\n' + ''.join(n + '\t' + hashlib.md5(Path(entries[n]['path']).read_bytes()).hexdigest() + '\n' for n in package['backup']) + '\n'
        generated(directory + '/files', dbfiles)
        generated(directory + '/mtree', mtree_data)
        package['archive'], package['sha256'], package['size'] = str(path), sha(path), size
        return path

    archives = [archive(p) for p in packages.values()]
    generated('var/lib/pacman/local/ALPM_DB_VERSION', '9\n')
    # Upstream repo-add writes the real database including detached package
    # signatures. It runs with host tools, in the confined output directory.
    host(['/usr/bin/bash', arch / 'install/usr/bin/repo-add', '--sign', '--key', fingerprint,
          out / 'repo/coast.db.tar.gz', *archives])
    for path in sorted((out / 'repo').iterdir()):
        if path.name.endswith(('.old', '.old.sig')):
            continue
        publish('/repo/x86_64/' + path.name, path)
        if path.name in ['coast.db', 'coast.db.sig', 'coast.files', 'coast.files.sig']:
            add('var/lib/pacman/sync/' + path.name, path, mode=0o644)
    # Ship upstream sources and the exact port recipes for license compliance
    # and local auditing, without distributing any repository signing secret.
    registries = [ROOT / 'userland/vendor/arch-ports-sources.json', ROOT / 'userland/vendor/package-support-sources.json']
    for registry in registries:
        for record in json.loads(registry.read_text()).values():
            if record.get('source') == 'meson-host':
                continue
            path = arch / 'vendor' / record['archive']
            assert sha(path) == record['sha256']
            add('usr/src/coast/package-tools/' + path.name, path, mode=0o644)
    bash=next(p for p in json.loads((ROOT/'userland/vendor/linux-ports-sources.json').read_text())['packages'] if p['name']=='bash')
    for name,digest in [(bash['archive'],bash['sha256']),*((p['file'],p['sha256']) for p in bash['patches'])]:
        path=ROOT/'userland/vendor/linux-ports'/name
        assert sha(path)==digest
        add('usr/src/coast/package-tools/'+name,path,mode=0o644)
    for path in [ROOT / 'tools/build_arch_ports.py', ROOT / 'tools/build_package_support.py', ROOT / 'tools/patch_fakeroot_unix.py', *sources]:
        add('usr/src/coast/package-tools/port/' + str(path.relative_to(ROOT)), path, mode=0o644)
    assert {str(p.relative_to(ROOT)): sha(p) for p in sources} == inputs
    manifest = dict(state='assembled; guest validation pending', target_host_executed=False,
                    files=files, aliases=aliases, directories=directories, mtime=EPOCH, source_sha256={**ports['source_sha256'], **inputs},
                    packages=packages, recipes=results, host_files=host_files, repository_mode=args.repository_mode,
                    guest_url=guest_url, public_signing_fingerprint=fingerprint,
                    signing_secret_included=False, commands=commands, confinement=conf,
                    sources={'ports': str(args.ports.resolve()), 'arch': str(arch), 'qt': str(args.qt_build.resolve()), 'base': base_receipt})
    (out / 'BUILD.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Built {len(packages)} signed packages and {len(results)} local Git recipes', flush=True)


if __name__ == '__main__':
    main()
