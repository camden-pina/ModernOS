"""Describe the files actually assembled into a fresh desktop to libalpm.

The initial live image is assembled from source receipts, so its local database
records validation=none. Only later signed repository transactions claim PGP
validation. No host package database or earlier image supplies metadata.
"""
import gzip
import hashlib
from pathlib import PurePosixPath
import tarfile

from source_tree import EPOCH


def create(payload, records):
    packages, owners = {}, {}

    def package(name, version, description, depends=(), provides=()):
        packages[name] = dict(name=name, version=version, description=description,
                              depends=list(depends), provides=list(provides), files=[])

    package('coast-base', '1.0-3', 'Coast native base assembled from source',
            provides=['busybox', 'sudo', 'gzip'])
    package('musl', '1.2.5-1', 'Source-built musl C library and loader')
    package('coast-desktop', '1.0-1', 'Source-built Coast desktop, browser and Code OSS', ['coast-base'])
    package('bash', '5.3.20-1', 'GNU Bash with Readline and Coast terminal integration', ['musl'])
    package('binutils', '2.37-1', 'GNU assembler, linker and binary utilities', ['musl'])
    package('gcc', '14.2.0-1', 'GNU C and C++ compilers with POSIX threads', ['musl', 'binutils'],
            ['g++=14.2.0', 'cc', 'c++', 'cpp', 'gcc-libs=14.2.0'])
    for name, version, description in [
        ('pacman', '7.1.0', 'Pacman, libalpm and makepkg'), ('gnupg', '2.5.24', 'OpenPGP verification tools'),
        ('libarchive', '3.8.11', 'BSD archive tools with fakeroot support'), ('curl', '8.22.0', 'Certificate-verifying HTTP(S) client'),
        ('openssl', '3.5.9', 'OpenSSL LTS tools'), ('make', '4.4.1', 'GNU Make'), ('zstd', '1.5.7', 'Zstandard compression'),
        ('python', '3.14.10', 'Python interpreter and standard library'), ('jq', '1.8.2', 'JSON processor'),
        ('coreutils', '9.12', 'GNU core utilities'), ('findutils', '4.11.0', 'GNU find and xargs'),
        ('file', '5.48', 'File type identification'), ('fakeroot', '2.1.4', 'Package metadata simulation without kernel privilege'),
        ('gawk', '5.4.0', 'GNU Awk'), ('git', '2.56.0', 'Git with local and HTTP(S) transports'),
        ('sed', '4.10', 'GNU stream editor'), ('diffutils', '3.12', 'GNU file comparison tools'),
        ('qt6-demo', '6.8.3', 'Qt Core/Gui/Widgets with the native Coast platform plugin'),
        ('qemu', '10.2.4', 'QEMU x86-64 TCI emulator and source-built BIOS firmware'),
        ('qemu-devel', '10.2.4', 'QEMU static dependency SDK, fixtures and upstream sources'),
        ('novnc', '1.6.0', 'Source JavaScript VNC client'), ('coast-vm', '1.0', 'Local QEMU launcher and Chromium viewer'),
        ('coast-packages', '1.6', 'Signed package and reviewed source recipe frontend'),
        ('makepkg-environment', '1.0', 'Source recipes and support files for native package builds')]:
        package(name, version + '-1', description, ['musl'])
    from build_extra_programs import PACKAGES
    for name in ('ninja', 'pkgconf', 'm4', 'patch', 'xz'):
        package(name, PACKAGES[name]['version'] + '-1', 'Upstream ' + name + ' native build tool', ['musl'],
                ['pkg-config'] if name == 'pkgconf' else [])
    packages['coast-packages']['depends'] = ['python', 'pacman', 'base-devel']
    packages['coast-vm']['depends'] = ['python', 'qemu', 'novnc']
    packages['qemu-devel']['depends'] = ['qemu', 'gcc', 'pkgconf']
    packages['pacman']['depends'] = ['bash', 'gnupg', 'libarchive', 'coreutils', 'findutils', 'file']
    packages['fakeroot']['depends'] = ['musl', 'bash']
    package('base-devel', '1.0-3', 'Native makepkg build environment',
            ['gcc', 'binutils', 'make', 'gawk', 'git', 'bash', 'coreutils', 'findutils', 'file',
             'fakeroot', 'pacman', 'patch', 'pkgconf', 'm4', 'grep', 'sed', 'diffutils'])
    import json
    from pathlib import Path
    registry = json.loads((Path(__file__).resolve().parents[1] / 'userland/vendor/linux-ports-sources.json').read_text())
    grep = next(p for p in registry['packages'] if p['name'] == 'grep')
    package('grep', grep['version'] + '-1', 'GNU grep with the Coast pipe fallback', ['musl'])

    def members(record):
        return [*record.get('files', {}), *record.get('aliases', {})]

    def assign(record, default, chooser=None):
        for name in members(record):
            if name in payload.entries:
                owners[name] = chooser(name) if chooser else default

    binutils = {'addr2line', 'ar', 'as', 'c++filt', 'dwp', 'elfedit', 'gprof', 'ld', 'ld.bfd',
                'nm', 'objcopy', 'objdump', 'ranlib', 'readelf', 'size', 'strings', 'strip'}
    assign(records.get('cli_build', {}), 'bash')
    assign(records.get('compiler_build', {}), 'gcc', lambda name: 'binutils' if
           (name.startswith('usr/bin/') and PurePosixPath(name).name in binutils) or '/binutils/' in name else 'gcc')

    def package_cli(name):
        base = PurePosixPath(name).name
        if base in ('bsdtar', 'bsdcpio', 'bsdcat', 'bsdunzip') or '/libarchive/' in name:
            return 'libarchive'
        if base.startswith(('gpg', 'gpgtar', 'gnupg', 'dumpsexp', 'hmac256', 'kbxutil', 'watchgnupg')) or '/gnupg/' in name:
            return 'gnupg'
        if name.startswith('usr/bin/') and base in ('curl', 'openssl', 'make', 'zstd', 'unzstd', 'zstdcat', 'zstdmt'):
            return 'zstd' if 'zstd' in base else base
        for candidate in ('curl', 'openssl', 'make', 'zstd'):
            if '/' + candidate + '/' in name or base.startswith(candidate + '-'):
                return candidate
        return 'pacman'

    assign(records.get('packages_build', {}), 'pacman', package_cli)
    for key in ('gui_runtime', 'assets_build'):
        assign(records.get(key, {}), 'coast-desktop')
    for name in list(payload.entries):
        if name.startswith(('opt/', 'usr/share/coast-browser/', 'usr/share/coast/codeoss/')):
            owners[name] = 'coast-desktop'
    assign(records.get('program_build', {}), 'python', lambda name: 'jq' if
           name == 'usr/bin/jq' or '/jq/' in name or '/jq-' in name else 'python')

    tools = {'find': 'findutils', 'xargs': 'findutils', 'locate': 'findutils', 'updatedb': 'findutils',
             'file': 'file', 'fakeroot': 'fakeroot', 'faked': 'fakeroot', 'kill': 'bash',
             'gawk': 'gawk', 'awk': 'gawk', 'git': 'git', 'git-shell': 'git',
             'sed': 'sed', 'diff': 'diffutils', 'cmp': 'diffutils', 'diff3': 'diffutils', 'sdiff': 'diffutils',
             'grep': 'grep', 'egrep': 'grep', 'fgrep': 'grep', 'bsdtar': 'libarchive', 'bsdcat': 'libarchive',
             'bsdcpio': 'libarchive', 'bsdunzip': 'libarchive', 'ninja': 'ninja', 'pkgconf': 'pkgconf',
             'pkg-config': 'pkgconf', 'm4': 'm4', 'linux-patch': 'patch', 'patch': 'patch',
             'linux-xz': 'xz', 'linux-unxz': 'xz', 'linux-xzcat': 'xz', 'xz': 'xz', 'unxz': 'xz', 'xzcat': 'xz',
             'bash': 'bash'}

    def makepkg(name):
        base = PurePosixPath(name).name
        if name in ('usr/lib/libc.so', 'lib/ld-musl-x86_64.so.1') or '/shared-musl/' in name:
            return 'musl'
        if name == 'etc/makepkg.conf':
            return 'pacman'
        if name.startswith('usr/bin/'):
            return tools.get(base, 'coreutils')
        if name.startswith('usr/libexec/coreutils/'):
            return 'coreutils'
        if name.startswith(('usr/lib/libfakeroot/', 'usr/share/man/man1/fakeroot', 'usr/share/man/man1/faked')):
            return 'fakeroot'
        if name.startswith(('usr/libexec/git-core/', 'usr/share/git-core/')):
            return 'git'
        if name.startswith(('usr/share/misc/magic', 'usr/lib/libmagic', 'usr/include/magic')):
            return 'file'
        for candidate in ('coreutils', 'findutils', 'file', 'fakeroot', 'gawk', 'git', 'sed', 'diffutils',
                          'libarchive', 'grep', 'ninja', 'pkgconf', 'm4', 'patch', 'xz'):
            if '/' + candidate + '/' in name or base.startswith((candidate + '-', candidate + '_')):
                return candidate
        return 'makepkg-environment'

    assign(records.get('makepkg_build', {}), 'makepkg-environment', makepkg)
    assign(records.get('frontend_build', {}), 'coast-packages', lambda name: 'pacman' if name in ('etc/pacman.conf', 'etc/makepkg.conf') else 'coast-packages')
    assign(records.get('qt_build', {}), 'qt6-demo')
    assign(records.get('qemu_build', {}), 'qemu', lambda name: 'qemu-devel' if
           name.startswith(('usr/lib/coast/qemu-sdk/', 'usr/src/', 'usr/share/coast/qemu/')) else 'qemu')
    assign(records.get('viewer_build', {}), 'novnc', lambda name: 'coast-vm' if name.startswith(
           ('usr/bin/coast-vm', 'usr/share/coast/qemu/', 'usr/share/licenses/coast-vm/')) or name == 'usr/share/novnc/coast.html' else 'novnc')
    for name, entry in payload.entries.items():
        if entry['kind'] in (tarfile.REGTYPE, tarfile.SYMTYPE) and not name.startswith(('home/', 'root/', 'tmp/', 'var/lib/pacman/')):
            owner = owners.setdefault(name, 'coast-base')
            packages[owner]['files'].append(name)
    # An omitted application must never appear as an installed package.
    retained = {name for name, item in packages.items() if item['files']} | {'coast-base', 'musl'}
    if 'makepkg_build' in records:
        retained.add('base-devel')
    packages = {name: item for name, item in packages.items() if name in retained}
    provided = set(packages) | {alias.split('=')[0] for item in packages.values() for alias in item['provides']}
    for name, item in packages.items():
        if missing := set(item['depends']) - provided:
            raise ValueError('Installed package dependency missing for ' + name + ': ' + ', '.join(sorted(missing)))
    installed = {}

    def escaped(text):
        return ''.join('\\%03o' % ord(char) if char in ' \\#\t\n' else char for char in text)

    for name, package in sorted(packages.items()):
        names = sorted(package['files'])
        size = sum(len(payload.entries[path]['data']) for path in names)
        directories = sorted({str(parent) for path in names for parent in PurePosixPath(path).parents if str(parent) != '.'})
        mtree = ['#mtree', f'. type=dir uid=0 gid=0 mode=755 time={EPOCH}.0']
        for path in directories:
            mode = payload.entries.get(path, {}).get('mode', 0o755)
            mtree.append(f'./{escaped(path)} type=dir uid=0 gid=0 mode={mode:o} time={EPOCH}.0')
        for path in names:
            entry = payload.entries[path]
            line = f'./{escaped(path)} uid=0 gid=0 mode={entry["mode"]:o} time={EPOCH}.0'
            if entry['kind'] == tarfile.SYMTYPE:
                line += ' type=link link=' + escaped(entry['link'])
            else:
                line += ' type=file size=' + str(len(entry['data'])) + ' sha256digest=' + hashlib.sha256(entry['data']).hexdigest()
            mtree.append(line)
        fields = dict(NAME=[name], BASE=[name], VERSION=[package['version']], DESC=[package['description']],
            URL=['https://coast.local/'], ARCH=['x86_64'], BUILDDATE=[str(EPOCH)], INSTALLDATE=[str(EPOCH)],
            PACKAGER=['Coast source image assembler'], SIZE=[str(size)], REASON=['0'], VALIDATION=['none'],
            DEPENDS=package['depends'], PROVIDES=package['provides'])
        directory = 'var/lib/pacman/local/' + name + '-' + package['version']
        payload.add(directory + '/desc', ''.join('%' + key + '%\n' + '\n'.join(value) + '\n\n' for key, value in fields.items() if value).encode())
        files = '%FILES%\n' + '\n'.join(sorted([*(path + '/' for path in directories), *names])) + '\n\n'
        backups = [path for path in names if path.startswith('etc/') and payload.entries[path]['kind'] == tarfile.REGTYPE]
        if backups:
            files += '%BACKUP%\n' + ''.join(path + '\t' + hashlib.md5(payload.entries[path]['data']).hexdigest() + '\n' for path in backups) + '\n'
        payload.add(directory + '/files', files.encode())
        payload.add(directory + '/mtree', gzip.compress(('\n'.join(mtree) + '\n').encode(), mtime=EPOCH))
        installed[name] = dict(version=package['version'], files=len(names), bytes=size, depends=package['depends'], provides=package['provides'])
    return dict(packages=installed, owners=owners, validation='none; assembled from verified source receipts')
