#!/usr/bin/env python3
"""Extend an immutable Coast repository with signed QEMU and SDK packages."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import zipfile

from export_repository import export
from prepare_browser_host import PACKAGES as HOST_TOOLS
from serve_repository import load_repository

ROOT = Path(__file__).resolve().parents[1]
EPOCH = 1793923200
SDK = 'usr/lib/coast/qemu-sdk/'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, default=ROOT / 'build/brine-repository-server/repository.json')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/qemu/BUILD.json')
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--signing-home', type=Path, default=ROOT / 'build/coast-signing-keys')
    parser.add_argument('--repo-add', type=Path,
                        default=ROOT / 'build/coast-security-ports-r3/install/usr/bin/repo-add')
    args = parser.parse_args()
    build = json.loads(args.build.read_text())
    if build['target_host_executed'] is not False:
        parser.error('A cross-build receipt is required')
    for relative, digest in build['source_sha256'].items():
        if sha(ROOT / relative) != digest:
            raise ValueError('Build input changed: ' + relative)
    for relative, asset in build['files'].items():
        if sha(asset['path']) != asset['sha256']:
            raise ValueError('Build output changed: ' + relative)
    output = args.output_root.resolve()
    keys = args.signing_home.resolve()
    if not keys.is_dir() or keys.stat().st_mode & 0o077 or keys.is_relative_to(output):
        parser.error('A private existing signing directory outside the public output is required')
    export(args.repository, output)
    manifest = output / 'repository.json'
    snapshot = json.loads(manifest.read_text())
    fingerprint = snapshot['public_signing_fingerprint']
    env = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
               GNUPGHOME=str(keys), SOURCE_DATE_EPOCH=str(EPOCH),
               MAKEPKG_LIBRARY=str(args.repo_add.resolve().parents[1] / 'share/makepkg'))
    conf = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
            '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
            '--bind', str(output), str(output), '--bind', str(keys), str(keys)]

    def host(command):
        result = subprocess.run([*conf, '--chdir', str(output), '--', *map(str, command)],
                                env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr[-2500:] + '\n' + result.stdout[-1500:])
        return result.stdout

    def sign(path):
        host(['gpg', '--batch', '--yes', '--pinentry-mode', 'loopback', '--passphrase', '',
              '--local-user', fingerprint, '--detach-sign', path])
        host(['gpg', '--batch', '--verify', str(path) + '.sig', path])

    packages = {}

    def package(name, version, description, depends, license, provides=()):
        if name in snapshot['packages']:
            raise ValueError('Use the prior snapshot without QEMU packages: ' + name)
        packages[name] = dict(name=name, version=version + '-1', description=description,
                              depends=depends, license=license, provides=list(provides),
                              files={}, aliases={})

    package('qemu', '10.2.4', 'QEMU x86-64 system emulator (TCI) and disk image tools',
            ['musl', 'qemu-common=10.2.4-1'], 'GPL-2.0-only', ['qemu-system-x86_64', 'qemu-img', 'qemu-io'])
    package('qemu-common', '10.2.4', 'Matching x86 BIOS, licenses and QEMU integration examples', [], 'GPL-2.0-only')
    names = {'glib': 'glib2', 'pcre2': 'pcre2', 'libffi': 'libffi', 'pixman': 'pixman',
             'libslirp': 'libslirp', 'zlib': 'zlib', 'libucontext': 'libucontext'}
    for source, name in names.items():
        metadata = build['packages'][source]
        dependencies = ['musl']
        if source == 'glib':
            dependencies += ['libffi', 'pcre2', 'zlib']
        if source == 'libslirp':
            dependencies += ['glib2']
        package(name, metadata['version'], name + ' headers, PIC static libraries and pkg-config metadata for Coast',
                dependencies, metadata['license'])
    wheel_record = next(p for p in HOST_TOOLS if p['kind'] == 'wheel')
    wheel = ROOT / 'build/modern-browser/downloads' / wheel_record['archive']
    if sha(wheel) != wheel_record['sha256']:
        raise ValueError('Invalid Meson wheel')
    package('meson', '1.10.2', 'Meson build system for native Coast Python', ['python', 'ninja'], 'Apache-2.0')
    package('qemu-devel', '10.2.4', 'Complete static QEMU dependency SDK and native build instructions',
            [*names.values(), 'meson', 'ninja', 'pkgconf', 'gcc', 'make', 'python'], '0BSD')
    package('qemu-source', '10.2.4', 'Exact QEMU/dependency sources and Coast build recipes', [], 'GPL-2.0-only')
    sdk_owners = {SDK + relative: names[source] for source, paths in build['sdk'].items()
                  for relative in paths if relative.startswith(('lib/', 'include/')) and not relative.endswith('.la')}
    for relative, asset in build['files'].items():
        if relative.startswith('usr/bin/'):
            owner = 'qemu'
        elif relative.startswith('usr/src/'):
            owner = 'qemu-source'
        elif relative.startswith(SDK):
            owner = sdk_owners[relative]
        elif relative.startswith('usr/share/licenses/qemu-port/'):
            source = PurePosixPath(relative).parts[4]
            owner = 'qemu-common'
            if source in names:
                packages[names[source]]['files']['usr/share/licenses/' + names[source] + '/' + Path(relative).name] = asset
        else:
            owner = 'qemu-common'
        packages[owner]['files'][relative] = asset
    generated = output / 'generated'
    generated.mkdir()

    def add(owner, relative, data, mode=0o644):
        path = generated / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.encode() if isinstance(data, str) else data)
        path.chmod(mode)
        packages[owner]['files'][relative] = dict(path=str(path), sha256=sha(path), mode=mode)

    with zipfile.ZipFile(wheel) as bundle:
        for item in bundle.infolist():
            path = PurePosixPath(item.filename)
            if path.is_absolute() or '..' in path.parts:
                raise ValueError('Unsafe wheel member')
            if item.is_dir() or not (item.filename.startswith('mesonbuild/') or '.dist-info/' in item.filename):
                continue
            add('meson', 'usr/lib/python3.14/site-packages/' + item.filename, bundle.read(item))
    add('meson', 'usr/bin/meson', '#!/usr/bin/python3\nfrom mesonbuild.mesonmain import main\nraise SystemExit(main())\n', 0o755)
    add('qemu-devel', 'usr/share/doc/qemu/sdk.txt',
        'Static dependency SDK: /usr/lib/coast/qemu-sdk\n'
        'For native configure, export PKG_CONFIG_PATH=/usr/lib/coast/qemu-sdk/lib/pkgconfig\n'
        'and use pkg-config --static. Sources: brine install qemu-source.\n'
        'The host cross-build recipe and exact configure options accompany the sources.\n'
        'A complete native QEMU source rebuild is not a qualified workflow yet.\n')
    add('qemu-common', 'usr/share/doc/qemu/coast.md', (ROOT / 'Documentation/qemu.md').read_bytes())
    add('qemu-source', 'usr/src/coast/qemu/vendor/' + wheel.name, wheel.read_bytes())
    add('qemu-source', 'usr/src/coast/qemu/tools/add_qemu_repository.py', Path(__file__).read_bytes())

    def publish(path, route):
        snapshot['host_files'][route] = dict(path=str(path.relative_to(output)), sha256=sha(path))

    directory = output / 'public/repo/x86_64'
    directory.mkdir(parents=True, exist_ok=True)
    archives = []
    owned = {path: name for name, p in snapshot['packages'].items() for path in [*p['files'], *p['aliases']]}
    for name, package in packages.items():
        entries = package['files']
        for path in entries:
            if path in owned:
                raise ValueError(f'Package file conflict: {path} ({owned[path]}, {name})')
            owned[path] = name
        dirs = sorted({str(parent) for path in entries for parent in PurePosixPath(path).parents
                       if str(parent) != '.'})
        size = sum(Path(asset['path']).stat().st_size for asset in entries.values())
        info = (f'pkgname = {name}\npkgbase = {name}\npkgver = {package["version"]}\n'
                f'pkgdesc = {package["description"]}\nurl = https://www.qemu.org/\n'
                f'builddate = {EPOCH}\npackager = Coast local ports\nsize = {size}\n'
                f'arch = x86_64\nlicense = {package["license"]}\n')
        info += ''.join('depend = ' + dep + '\n' for dep in package['depends'])
        info += ''.join('provides = ' + dep + '\n' for dep in package['provides'])
        mtree = ['#mtree', f'. type=dir uid=0 gid=0 mode=755 time={EPOCH}.0']
        for path in dirs:
            mtree.append(f'./{path} type=dir uid=0 gid=0 mode=755 time={EPOCH}.0')
        for path, asset in sorted(entries.items()):
            if any(c.isspace() for c in path):
                raise ValueError('Unsupported whitespace in package file name')
            mtree.append(f'./{path} type=file uid=0 gid=0 mode={asset["mode"]:o} '
                         f'size={Path(asset["path"]).stat().st_size} time={EPOCH}.0 sha256digest={asset["sha256"]}')
        archive = directory / f'{name}-{package["version"]}-x86_64.pkg.tar.gz'
        print('Packaging ' + name, flush=True)
        with archive.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', mtime=EPOCH, filename='') as compressed, \
                tarfile.open(fileobj=compressed, mode='w|', format=tarfile.PAX_FORMAT) as tar:
            def put(path, data=b'', mode=0o644, kind=tarfile.REGTYPE):
                item = tarfile.TarInfo(path)
                item.uid = item.gid = 0
                item.mtime, item.mode, item.type, item.size = EPOCH, mode, kind, len(data)
                tar.addfile(item, io.BytesIO(data) if data else None)
            put('.PKGINFO', info.encode())
            put('.MTREE', gzip.compress(('\n'.join(mtree) + '\n').encode(), mtime=EPOCH))
            for path in dirs:
                put(path + '/', mode=0o755, kind=tarfile.DIRTYPE)
            for path, asset in sorted(entries.items()):
                put(path, Path(asset['path']).read_bytes(), asset['mode'])
        sign(archive)
        archives.append(archive)
        publish(archive, '/repo/x86_64/' + archive.name)
        publish(Path(str(archive) + '.sig'), '/repo/x86_64/' + archive.name + '.sig')
        snapshot['packages'][name] = {**{k: v for k, v in package.items() if k not in ('files', 'aliases')},
                                      'files': sorted(entries), 'aliases': []}
    host(['/usr/bin/bash', args.repo_add.resolve(), '--sign', '--key', fingerprint,
          directory / 'coast.db.tar.gz', *archives])
    for path in directory.iterdir():
        if path.name.endswith(('.old', '.old.sig')):
            path.unlink()
            continue
        publish(path, '/repo/x86_64/' + path.name)
    # Every exact upstream archive is independently downloadable as well.
    for relative, asset in packages['qemu-source']['files'].items():
        if '/vendor/' not in relative:
            continue
        target = output / 'public/distfiles' / Path(relative).name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(asset['path'], target)
        publish(target, '/distfiles/' + target.name)
    snapshot['qemu_build_sha256'] = sha(args.build)
    manifest.write_text(json.dumps(snapshot, indent=2) + '\n')
    # Generated staging is private to packaging; public paths must all resolve
    # within this snapshot and carry a hash before any service selects it.
    load_repository(manifest)
    shutil.rmtree(generated)
    print(json.dumps(dict(repository=str(manifest), added=list(packages),
                          total_packages=len(snapshot['packages']), signing_secret_included=False), indent=2))


if __name__ == '__main__':
    main()
