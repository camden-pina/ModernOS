#!/usr/bin/env python3
"""Build standalone pacman/GnuPG/archive/HTTPS tools from pinned sources.

The binary-package CLI uses static musl programs. The higher-level Python Coast
client and fakeroot/makepkg source-build environment remain separate components.
No existing ISO, repository, signing identity or installed host package is read.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from fetch_sources import fetch, read_manifest
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-build', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--reconfigure', action='store_true', help='accept builder-only fixes with unchanged source pins and native base')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24:
        parser.error('--jobs must be 1..24')
    base = args.native_build.resolve(strict=True)
    native_receipt = base / 'BUILD.json'
    if json.loads(native_receipt.read_text()).get('inherited_iso') is not False:
        parser.error('Use a source-built native base')
    compiler = base / 'musl/bin/musl-gcc'
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    inputs = [Path(__file__), ROOT / 'tools/build_arch_ports.py', ROOT / 'tools/fetch_sources.py',
        ROOT / 'tools/source_archive.py', ROOT / 'userland/vendor/sources.json',
        ROOT / 'userland/vendor/arch-ports-sources.json', ROOT / 'userland/vendor/extra-program-sources.json',
        ROOT / 'userland/packages/config/pacman.conf']
    identity = dict(source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
        native_build_sha256=digest(native_receipt), compiler_sha256=digest(compiler))
    stamp = out / 'SOURCE.json'
    if stamp.exists() and json.loads(stamp.read_text()) != identity:
        previous = json.loads(stamp.read_text())
        builders = {'tools/build_desktop_packages.py', 'tools/build_arch_ports.py'}
        def stable(record):
            return {**record, 'source_sha256': {k: v for k, v in record['source_sha256'].items() if k not in builders}}
        if not args.reconfigure or stable(previous) != stable(identity):
            raise ValueError('Package build inputs changed; choose a fresh output directory or --reconfigure for builder-only fixes')
        history = out / 'configuration-history'
        history.mkdir(exist_ok=True)
        shutil.copyfile(stamp, history / (digest(stamp) + '.json'))
    stamp.write_text(json.dumps(identity, indent=2) + '\n')
    registry = read_manifest()
    pins = {name: registry[archive] for name, archive in [
        ('zlib', 'zlib-1.3.2.tar.xz'), ('openssl', 'openssl-3.5.9-source.tar.gz'),
        ('curl', 'curl-8.22.0.tar.xz'), ('make', 'make-4.4.1.tar.gz')]}
    pins['zstd'] = json.loads((ROOT / 'userland/vendor/extra-program-sources.json').read_text())['zstd']
    vendor, prefix = out / 'vendor', out / 'dependencies'
    vendor.mkdir(exist_ok=True)
    prefix.mkdir(exist_ok=True)
    sources = {}
    for name, pin in pins.items():
        fetch(pin, vendor)
        directory = pin.get('source') or {'zlib': 'zlib-1.3.2', 'openssl': 'openssl-openssl-3.5.9',
            'curl': 'curl-8.22.0', 'make': 'make-4.4.1'}[name]
        source = out / directory
        if not source.exists():
            unpack(vendor / pin['archive'], out)
        sources[name] = source
    flags = '-O2 -g0 -fPIC -fno-stack-protector -mno-avx -mno-avx2'
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        CC=str(compiler), AR='ar', RANLIB='ranlib', STRIP='strip', CFLAGS=flags,
        CPPFLAGS='-I' + str(prefix / 'include') + ' -idirafter ' + str(ROOT / 'ports/linux-uapi/include'),
        LDFLAGS='-static -no-pie -L' + str(prefix / 'lib'),
        PKG_CONFIG_LIBDIR=str(prefix / 'lib/pkgconfig'), CONFIG_SITE='/dev/null')
    confinement = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
        '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
        '--bind', str(out), str(out)]
    commands = []

    def run(name, command, cwd, extra=None):
        command = list(map(str, command))
        commands.append(dict(name=name, argv=command, cwd=str(cwd)))
        print('Desktop packages: ' + name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(cwd), '--', *command],
                env={**environment, **(extra or {})}, stdin=subprocess.DEVNULL,
                stdout=log, stderr=subprocess.STDOUT, check=True)

    def copy(source, target):
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(source.stat().st_mode & 0o777)

    # All dependency installations stay inside this output directory.
    if not (prefix / 'lib/libz.a').exists():
        source = sources['zlib']
        run('zlib-configure', ['./configure', '--static', '--prefix=' + str(prefix)], source)
        run('zlib-build', ['make', '-j' + str(args.jobs)], source)
        run('zlib-install', ['make', 'install'], source)
    if not (prefix / 'lib/libssl.a').exists():
        build = out / 'openssl-build'
        build.mkdir(exist_ok=True)
        extra = dict(LDFLAGS='-no-pie')
        run('openssl-configure', ['perl', sources['openssl'] / 'Configure', 'linux-x86_64',
            'threads', 'no-shared', 'no-module', 'no-tests', 'no-asm', 'no-engine',
            '--with-rand-seed=getrandom', '--prefix=' + str(prefix), '--openssldir=/etc/ssl', '--libdir=lib'], build, extra)
        # These are separate recursive Make goals with overlapping objects.
        # Running both in one parallel invocation races their dependency files.
        run('openssl-libraries', ['make', '-j' + str(args.jobs), 'build_libs'], build, extra)
        run('openssl-programs', ['make', '-j' + str(args.jobs), 'build_programs'], build, extra)
        run('openssl-install', ['make', 'install_sw'], build, extra)
    if not (prefix / 'lib/libcurl.a').exists():
        build = out / 'curl-build'
        build.mkdir(exist_ok=True)
        run('curl-configure', [sources['curl'] / 'configure', '--prefix=' + str(prefix),
            '--build=' + subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip(),
            '--host=x86_64-linux-musl', '--disable-shared', '--enable-static', '--with-openssl=' + str(prefix),
            '--with-zlib=' + str(prefix), '--with-ca-bundle=/etc/ssl/cert.pem', '--without-ca-path',
            '--without-ca-fallback', '--disable-threaded-resolver', '--disable-ipv6', '--without-libpsl',
            '--disable-unix-sockets', '--without-nghttp2', '--without-nghttp3', '--without-libidn2',
            '--without-brotli', '--without-zstd', '--without-libssh2', '--without-libssh',
            '--disable-ldap', '--disable-ldaps', '--disable-rtsp', '--disable-smtp', '--disable-pop3',
            '--disable-imap', '--disable-mqtt', '--disable-tftp', '--disable-telnet', '--disable-dict', '--disable-ftp'], build)
        run('curl-build', ['make', '-j' + str(args.jobs)], build)
        run('curl-install', ['make', 'install'], build)
    if not (prefix / 'lib/libzstd.a').exists():
        source = sources['zstd']
        run('zstd-library', ['make', '-C', source / 'lib', '-j' + str(args.jobs), 'libzstd.a'], source)
        run('zstd-program', ['make', '-C', source / 'programs', '-j' + str(args.jobs), 'zstd-release',
            'HAVE_ZLIB=0', 'HAVE_LZMA=0', 'HAVE_LZ4=0', 'LDFLAGS=-static -no-pie'], source)
        for name in ('zstd.h', 'zstd_errors.h', 'zdict.h'):
            copy(source / 'lib' / name, prefix / 'include' / name)
        copy(source / 'lib/libzstd.a', prefix / 'lib/libzstd.a')
        copy(source / 'programs/zstd', prefix / 'bin/zstd')
    if not (prefix / 'bin/make').exists():
        build = out / 'make-build'
        build.mkdir(exist_ok=True)
        # Make 4.4.1's gnulib uses pre-C23 function declarations. GCC 16's
        # newer default changes their meaning; select its supported C dialect.
        extra = dict(CFLAGS=flags + ' -std=gnu11')
        run('make-configure', [sources['make'] / 'configure', '--prefix=/usr',
            '--build=' + subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip(),
            '--host=x86_64-linux-musl', '--disable-nls', '--without-guile'], build, extra)
        run('make-clean', ['make', 'clean'], build, extra)
        run('make-build', ['make', '-j' + str(args.jobs)], build, extra)
        run('make-install', ['make', 'install', 'DESTDIR=' + str(out / 'make-install')], build, extra)
        copy(out / 'make-install/usr/bin/make', prefix / 'bin/make')
    dependencies = out / 'DEPENDENCIES.json'
    dependencies.write_text(json.dumps(dict(prefix=str(prefix), target_host_executed=False,
        sources=pins, **identity,
        artifacts={str(p): digest(p) for p in prefix.rglob('*') if p.is_file()}), indent=2) + '\n')
    arch = out / 'arch'
    arch_receipt = arch / 'COMPILATION.json'
    if not arch_receipt.exists():
        print('Desktop packages: pacman/GnuPG/libarchive source builds', flush=True)
        with (out / 'arch-build.log').open('w') as log:
            subprocess.run(list(map(str, [sys.executable, '-B', ROOT / 'tools/build_arch_ports.py',
                '--native-build', base, '--dependencies-build', dependencies, '--static-archive-tools',
                '--output-root', arch, '--jobs', args.jobs])), stdin=subprocess.DEVNULL,
                stdout=log, stderr=subprocess.STDOUT, check=True)
    built = json.loads(arch_receipt.read_text())
    for path, expected in built['artifacts'].items():
        if digest(Path(path)) != expected:
            raise ValueError('A completed package-tool artifact changed: ' + path)
    stage = out / 'runtime'
    stage.mkdir(exist_ok=True)
    for path in (arch / 'install/usr').rglob('*'):
        relative = str(path.relative_to(arch / 'install'))
        if path.is_file() and not path.is_symlink() and relative.startswith(('usr/bin/', 'usr/share/')):
            copy(path, stage / relative)
    for directory in (prefix / 'bin', arch / 'prefix/bin'):
        for path in directory.iterdir():
            if path.is_file() and not path.is_symlink() and path.read_bytes().startswith(b'\x7fELF'):
                copy(path, stage / 'usr/bin' / path.name)
    for name in ('pacman', 'pacman-conf', 'vercmp', 'gpg', 'gpgv', 'bsdtar', 'curl', 'openssl', 'zstd', 'make'):
        elf = subprocess.check_output(['readelf', '-h', '-lW', stage / 'usr/bin' / name], text=True)
        if 'Advanced Micro Devices X86-64' not in elf or 'INTERP' in elf or ' RWE ' in elf:
            raise ValueError('Package tools must be static non-WX x86-64 executables: ' + name)
    copy(ROOT / 'userland/packages/config/pacman.conf', stage / 'etc/pacman.conf')
    license_sources = [(sources['zlib'], 'LICENSE'), (sources['openssl'], 'LICENSE.txt'),
        (sources['curl'], 'COPYING'), (sources['zstd'], 'LICENSE'), (sources['make'], 'COPYING')]
    arch_pins = json.loads((ROOT / 'userland/vendor/arch-ports-sources.json').read_text())
    for name in ('libgpg-error', 'libgcrypt', 'libassuan', 'libksba', 'npth', 'gnupg', 'gpgme', 'libarchive', 'libseccomp', 'pacman'):
        source = arch / arch_pins[name]['source']
        for pattern in ('COPYING*', 'LICENSE*'):
            license_sources += [(source, path.name) for path in source.glob(pattern) if path.is_file()]
    for source, name in license_sources:
        copy(source / name, stage / 'usr/share/licenses/desktop-packages' / source.name / name)
    if any(digest(ROOT / name) != expected for name, expected in identity['source_sha256'].items()):
        raise ValueError('Package recipes changed during compilation')
    files = {str(p.relative_to(stage)): dict(path=str(p), sha256=digest(p),
        mode=0o755 if p.stat().st_mode & 0o111 else 0o644)
        for p in sorted(stage.rglob('*')) if p.is_file() and not p.is_symlink()}
    record = dict(**identity, files=files, aliases={'etc/ssl/cert.pem': 'certs/ca-certificates.crt'},
        directories=['etc/pacman.d/gnupg', 'etc/pacman.d/hooks', 'var/lib/pacman/local',
            'var/lib/pacman/sync', 'var/cache/pacman/pkg', 'var/log'], commands=commands,
        arch_build=dict(path=str(arch_receipt), sha256=digest(arch_receipt)),
        dependencies=dict(path=str(dependencies), sha256=digest(dependencies)),
        state='binary package CLI source compiled; Coast qualification pending',
        coast_python_frontend_included=False, makepkg_environment_complete=False,
        target_host_executed=False)
    (out / 'BUILD.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Source-built pacman/GnuPG/archive/HTTPS tools ready', flush=True)


if __name__ == '__main__':
    main()
