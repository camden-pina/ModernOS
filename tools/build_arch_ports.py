#!/usr/bin/env python3
"""Cross-build the upstream Arch package tools and their OpenPGP dependencies."""
from source_archive import unpack, unpack_zip
from build_state import project_lock
from build_state import atomic_write
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_FILE = ROOT / 'userland/vendor/arch-ports-sources.json'
SOURCES = json.loads(SOURCE_FILE.read_text())
ORDER = ['libgpg-error', 'libgcrypt', 'libassuan', 'libksba', 'npth', 'gnupg', 'gpgme', 'libarchive','libseccomp', 'pacman']


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/arch-ports-r1')
    parser.add_argument('--package', choices=ORDER, action='append')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--reuse-build',type=Path,help='import verified dependency artifacts into a fresh output')
    parser.add_argument('--reuse-receipt',type=Path,help='receipt pinning the imported dependency artifacts')
    parser.add_argument('--openssl-build',type=Path,default=ROOT/'build/coast-openssl-pic-r1/BUILD.json',help='verified PIC OpenSSL dependency receipt')
    parser.add_argument('--native-build', type=Path, help='source-built native base providing the musl compiler')
    parser.add_argument('--dependencies-build', type=Path, help='fresh static curl/OpenSSL/zlib/zstd dependency receipt')
    parser.add_argument('--static-archive-tools', action='store_true',
                        help='build standalone static archive tools for binary package management')
    args = parser.parse_args()
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    reused=None
    if args.reuse_build or args.reuse_receipt:
        assert args.reuse_build and args.reuse_receipt
        previous=args.reuse_build.resolve();receipt=args.reuse_receipt.resolve()
        record=json.loads(receipt.read_text());assert record['target_host_executed'] is False
        assert previous!=out and not (out/'prefix').exists() and not (out/'install').exists()
        copied={}
        for name,digest in record['artifacts'].items():
            path=Path(name);assert sha(path)==digest,name
            if path.is_relative_to(previous/'prefix') or path.is_relative_to(previous/'install'):
                copied[str(path.relative_to(previous))]=digest
        for folder in ('prefix','install'):
            shutil.copytree(previous/folder,out/folder,symlinks=True)
        for path in (out/'prefix').rglob('*.pc'):
            path.write_text(path.read_text().replace(str(previous/'prefix'),str(out/'prefix')))
        reused=dict(receipt=str(receipt),receipt_sha256=sha(receipt),build=str(previous),artifacts=copied)
    vendor, prefix, stage = out / 'vendor', out / 'prefix', out / 'runtime'
    for path in [vendor, prefix / 'lib/pkgconfig', prefix / 'include', stage]:
        path.mkdir(parents=True, exist_ok=True)
    cross = ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-'
    headers = ROOT / 'ports/linux-uapi/include' if args.native_build else ROOT / 'build/qt6/port-r1/linux-uapi'
    dependency_receipt = (args.dependencies_build or args.openssl_build).resolve(strict=True)
    ssl_receipt=json.loads(dependency_receipt.read_text());assert ssl_receipt['target_host_executed'] is False
    for path,digest in ssl_receipt['artifacts'].items():assert sha(path)==digest,path
    if not args.dependencies_build:
        assert sha(ssl_receipt['source_archive'])==ssl_receipt['source_archive_sha256']
    tls=Path(ssl_receipt['prefix'])
    web = tls if args.dependencies_build else ROOT / 'build/web/deps'
    zstd = tls / 'lib/libzstd.a' if args.dependencies_build else ROOT / 'build/program-ports-r2/zstd-build/lib/libzstd.a'
    flags = f'-O2 -g0 -std=gnu11 -fPIC -fno-stack-protector -mno-avx -mno-avx2 -idirafter {headers}'
    env = dict(PATH=f'{prefix}/bin:/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
               CC=str(cross) + 'gcc', CXX=str(cross) + 'g++', AR=str(cross) + 'ar', RANLIB=str(cross) + 'ranlib',
               STRIP=str(cross) + 'strip', CC_FOR_BUILD='/usr/bin/cc', CFLAGS_FOR_BUILD='-O2 -std=gnu11',
               CFLAGS=flags, CXXFLAGS=flags, CPPFLAGS=f'-I{prefix}/include -idirafter {headers}',
               LDFLAGS=f'-static -no-pie -L{prefix}/lib', PKG_CONFIG='/usr/bin/pkg-config',
               PKG_CONFIG_LIBDIR=str(prefix / 'lib/pkgconfig'), CONFIG_SITE='/dev/null',
               PYTHONPATH=str(out / 'meson-host'), PYTHONDONTWRITEBYTECODE='1')
    if args.native_build:
        compiler = args.native_build.resolve(strict=True) / 'musl/bin/musl-gcc'
        if not compiler.is_file():
            parser.error('The native base must include its compiled musl SDK')
        env.update(CC=str(compiler), CXX='false', AR='ar', RANLIB='ranlib', STRIP='strip')
    confinement = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--ro-bind', '/', '/',
                   '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []
    inputs = [Path(__file__), SOURCE_FILE]
    original = {str(p.relative_to(ROOT)): sha(p) for p in inputs}

    def run(name, command, cwd, source, extra=None):
        command = list(map(str, command))
        commands.append(dict(name=name, command=command, cwd=str(cwd)))
        print(name, flush=True)
        with (out / (name + '.log')).open('wb') as log:
            subprocess.run([*confinement, '--ro-bind', str(source), str(source), '--chdir', str(cwd), '--', *command],
                           env={**env, **(extra or {})}, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)

    def source_for(name):
        package = SOURCES[name]
        archive = vendor / package['archive']
        from fetch_sources import fetch
        fetch(package, vendor)
        assert sha(archive) == package['sha256'], name
        source = out / package['source']
        if not source.exists():
            if name == 'meson':
                unpack_zip(archive, source)
            else:
                unpack(archive, out, filter='data')
        return source

    # Public dependency headers only. Never expose the browser's libc headers.
    for source in [web / 'include/zlib.h', web / 'include/zconf.h']:
        shutil.copy2(source, prefix / 'include' / source.name)
    for directory in [tls / 'include/openssl', web / 'include/curl']:
        shutil.copytree(directory, prefix / 'include' / directory.name, dirs_exist_ok=True)
    for source in [tls / 'lib/libssl.a', tls / 'lib/libcrypto.a', web / 'lib/libcurl.a', web / 'lib/libz.a', zstd]:
        shutil.copy2(source, prefix / 'lib' / source.name)
    for filename in ['zstd.h', 'zstd_errors.h', 'zdict.h']:
        source = tls / 'include' / filename if args.dependencies_build else ROOT / 'build/program-ports-r2/zstd-1.5.7/lib' / filename
        shutil.copy2(source, prefix / 'include' / filename)
    for name, version, libraries, private in [
            ('libcrypto', '3.5.9', '-lcrypto', '-pthread'), ('libssl', '3.5.9', '-lssl', '-lcrypto -pthread'),
            ('libcurl', '8.22.0', '-lcurl', '-lssl -lcrypto -lz -pthread'),
            ('zlib', '1.3.1', '-lz', ''), ('libzstd', '1.5.7', '-lzstd', '-pthread')]:
        (prefix / 'lib/pkgconfig' / (name + '.pc')).write_text(
            f'prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\nName: {name}\n'
            f'Description: Verified Coast static dependency\nVersion: {version}\nLibs: -L${{libdir}} {libraries}\n'
            f'Libs.private: {private}\nCflags: -I${{includedir}}\n')
    source_for('meson')
    for name in ORDER:
        if args.package and name not in args.package:
            continue
        source = source_for(name)
        if name == 'gpgme':
            # getdents64 has fixed-width Linux fields; musl no longer exports
            # glibc's legacy ino64_t/off64_t typedef names.
            path = source / 'src/posix-io.c'
            data = path.read_text()
            if 'ino64_t d_ino' in data:
                data = data.replace('ino64_t d_ino', 'uint64_t d_ino').replace('off64_t d_off', 'int64_t d_off')
                data = '#include <stdint.h>\n' + data
                path.write_text(data)
        snapshot = {str(p.relative_to(source)): sha(p) for p in source.rglob('*') if p.is_file() and not p.is_symlink()}
        build = out / (name + '-build')
        build.mkdir(exist_ok=True)
        extra = {}
        if name == 'pacman':
            cross_file = out / 'cross.ini'
            cross_file.write_text(f"[binaries]\nc = '{env['CC']}'\nar = '{env['AR']}'\nstrip = '{env['STRIP']}'\npkg-config = '/usr/bin/pkg-config'\n"
                                 "[host_machine]\nsystem = 'linux'\ncpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n"
                                 "[properties]\nneeds_exe_wrapper = true\n"
                                 f"[built-in options]\nc_args = {repr((flags + ' ' + env['CPPFLAGS']).split())}\nc_link_args = {repr(env['LDFLAGS'].split())}\n")
            options = ['/usr/bin/python3', '-m', 'mesonbuild.mesonmain', 'setup', build, source,
                       '--cross-file', cross_file, '--prefix=/usr', '--sysconfdir=/etc', '--localstatedir=/var',
                       '--libdir=lib', '--default-library=static', '-Dbuildstatic=true', '-Di18n=false',
                       '-Ddoc=disabled', '-Dgpgme=enabled', '-Dcurl=enabled', '-Dcrypto=openssl',
                       '-Dfile-seccomp=disabled', '-Dpkg-ext=.pkg.tar.zst', '-Dsrc-ext=.src.tar.gz']
        else:
            destination = '/usr' if name == 'gnupg' else str(prefix)
            options = [source / 'configure', '--build=x86_64-pc-linux-gnu', '--host=x86_64-linux-musl',
                       '--prefix=' + destination, '--disable-shared', '--enable-static', '--disable-nls', '--disable-dependency-tracking']
            if name == 'libgcrypt':
                options += ['--disable-asm', '--disable-doc', '--with-libgpg-error-prefix=' + str(prefix)]
            if name in ('libassuan', 'libksba'):
                options += ['--with-libgpg-error-prefix=' + str(prefix)]
            if name == 'gnupg':
                options += ['--sysconfdir=/etc', '--libexecdir=/usr/bin', '--disable-doc', '--disable-all-tests',
                            '--disable-scdaemon', '--disable-gpgsm', '--disable-dirmngr', '--disable-g13',
                            '--disable-sqlite', '--disable-ldap', '--disable-ntbtls', '--disable-gnutls',
                            '--disable-bzip2', '--without-readline', '--without-capabilities',
                            '--with-libgpg-error-prefix=' + str(prefix), '--with-libgcrypt-prefix=' + str(prefix),
                            '--with-libassuan-prefix=' + str(prefix), '--with-ksba-prefix=' + str(prefix),
                            '--with-npth-prefix=' + str(prefix)]
            if name == 'gpgme':
                options += ['--enable-languages=no', '--disable-gpgconf-test', '--disable-gpg-test',
                            '--disable-gpgsm-test', '--disable-g13-test', '--disable-doc',
                            '--with-libgpg-error-prefix=' + str(prefix), '--with-libassuan-prefix=' + str(prefix)]
            if name=='libseccomp':
                options+=['--disable-tools','--disable-tests']
            if name == 'libarchive':
                # makepkg invokes bsdtar under fakeroot. Its preload wrappers
                # require a dynamic executable; libarchive itself stays static.
                if not args.static_archive_tools:
                    extra = dict(CFLAGS=flags + f' -B{web}/lib -specs={web}/lib/coast-dynamic.specs',
                                 LDFLAGS=f'-L{prefix}/lib -L{web}/lib -Wl,-z,relro,-z,now,-z,noexecstack')
                options += ['--without-bz2lib', '--without-lzma', '--without-lz4', '--without-lzo2',
                            '--without-nettle', '--with-openssl', '--without-libb2', '--without-xml2', '--without-expat',
                            '--disable-xattr', '--disable-acl', '--without-iconv', '--with-zlib', '--with-zstd']
        key = hashlib.sha256(json.dumps([list(map(str, options)), env, extra,ssl_receipt['artifacts']]).encode()).hexdigest()
        stamp = build / '.configured'
        if not stamp.exists() or stamp.read_text() != key or (name=='libgpg-error' and not (build/'src/lock-obj-pub.native.h').exists()):
            # libgpg-error generates its cross ABI lock header at configure
            # time; cleaning afterward would delete that required header.
            if name != 'pacman' and (build/'Makefile').exists():
                run(name + '-clean', ['make', 'clean'], build, source, extra)
            run(name + '-configure', options, build, source, extra)
            stamp.write_text(key)
        if name == 'pacman':
            run(name + '-build', ['ninja', '-j' + str(args.jobs)], build, source)
            run(name + '-install', ['/usr/bin/python3', '-m', 'mesonbuild.mesonmain', 'install', '-C', build, '--no-rebuild', '--destdir', out / 'install'], build, source)
        else:
            run(name + '-build', ['make', '-j' + str(args.jobs), 'all'], build, source, extra)
            run(name + '-install', ['make', 'install', *(['DESTDIR=' + str(out / 'install')] if name == 'gnupg' else [])], build, source, extra)
        if name=='libarchive':
            assert '#define ARCHIVE_CRYPTO_SHA256_OPENSSL 1' in (build/'config.h').read_text(),'package-file hashes require a real crypto provider'
        assert {str(p.relative_to(source)): sha(p) for p in source.rglob('*') if p.is_file() and not p.is_symlink()} == snapshot
        atomic_write(out / (name + '-SOURCES.json'), json.dumps(snapshot, indent=2) + '\n')
    assert {str(p.relative_to(ROOT)): sha(p) for p in inputs} == original, 'Recipe changed during compilation'
    artifacts = {str(p):sha(p) for folder in [out/'install',prefix/'bin',prefix/'lib'] for p in folder.rglob('*')
                 if p.is_file() and not p.is_symlink()}
    atomic_write(out / 'COMPILATION.json', json.dumps(dict(state='compiled; guest validation pending', sources=SOURCES,
        source_sha256=original, commands=commands, confinement=confinement, target_host_executed=False,
        dependency_receipt=dict(path=str(dependency_receipt),sha256=sha(dependency_receipt)),
        native_build_sha256=sha(args.native_build/'BUILD.json') if args.native_build else None,
        reused_dependencies=reused), indent=2) + '\n')
    receipt=json.loads((out/'COMPILATION.json').read_text())
    receipt['artifacts']=artifacts
    atomic_write(out/'COMPILATION.json', json.dumps(receipt,indent=2)+'\n')


if __name__ == '__main__':
    with project_lock():
        main()
