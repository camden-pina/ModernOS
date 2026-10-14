#!/usr/bin/env python3
"""Offline static musl libraries/assets shared by NetSurf and Git HTTPS."""
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/web'
SOURCE = BUILD / 'deps-src'
PREFIX = BUILD / 'deps'
MUSL = ROOT / 'build/native-musl/bin/musl-gcc'
VENDOR = ROOT / 'userland/vendor'
FLAGS = '-O2 -g0 -fno-pie -fno-stack-protector -mno-avx -mno-avx2'
PACKAGES = {
    'zlib-1.3.2.tar.xz': ('zlib-1.3.2', 'd7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3'),
    'openssl-3.5.9-source.tar.gz': ('openssl-openssl-3.5.9', '796831ec533bf41998eaaee9d632d921b8a69f57394fa4086ff01a941113d26d'),
    'curl-8.22.0.tar.xz': ('curl-8.22.0', 'f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7'),
    'libpng-1.6.59.tar.xz': ('libpng-1.6.59', 'd80dd2a38a37f803cb9b6ac7b14bd6e74ddc3b654780a8380bdf93523fdb4389'),
    'jpegsrc.v10.tar.gz': ('jpeg-10', '8b9eaa13242690ebd03e1728ab1edf97a81a78ed6e83624d493655f31ac95ab5'),
    'freetype-2.14.3.tar.xz': ('freetype-2.14.3', '36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f'),
    'expat-2.9.0-source.tar.gz': ('libexpat-R_2_9_0/expat', '6e8ce7b52ebbaa423c59becc7cf1e6875c42e49228c062ac8123c10e700cec27'),
}
CA_HASH = 'a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505'


@contextmanager
def stage_lock():
    # Parallel Make targets share this library prefix and SDK destination.
    # Lock the whole install, including symlinks, rather than individual files.
    BUILD.mkdir(parents=True, exist_ok=True)
    with (BUILD / '.stage.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def copy_stage(source, destination):
    # copytree overwrites regular files, but refuses existing symlink targets.
    for path in source.rglob('*'):
        if path.is_symlink():
            target = destination / path.relative_to(source)
            if target.is_symlink() or target.is_file():
                target.unlink()
    shutil.copytree(source, destination, dirs_exist_ok=True, symlinks=True)


def run(where, name, *args, env=None):
    log = BUILD / (name + '.log')
    with log.open('wb') as output:
        result = subprocess.run(args, cwd=where, env=env, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'{args[0]} failed; see {log}')


def package(archive, configure, env, install='install'):
    directory, digest = PACKAGES[archive]
    if hashlib.sha256((VENDOR / archive).read_bytes()).hexdigest() != digest:
        raise SystemExit('Upstream source checksum mismatch: ' + archive)
    source = SOURCE / directory
    stamp = BUILD / (directory.replace('/', '-') + '.completed')
    key = hashlib.sha256(json.dumps(
        [digest, configure, install, FLAGS, str(MUSL), str(PREFIX)]).encode()).hexdigest()
    if stamp.exists() and stamp.read_text() == key and source.exists():
        return source
    if source.exists():
        shutil.rmtree(source)
    with tarfile.open(VENDOR / archive) as contents:
        contents.extractall(SOURCE, filter='data')
    name = directory.replace('/', '-')
    if archive.startswith('expat-'):
        run(source, name + '-autogen', './buildconf.sh', env=env)
    run(source, name + '-configure', *configure, env=env)
    run(source, name + '-build', 'make', '-j8', env=env)
    run(source, name + '-install', 'make', install, env=env)
    stamp.write_text(key)
    return source


def build():
    BUILD.mkdir(parents=True, exist_ok=True)
    SOURCE.mkdir(exist_ok=True)
    env = os.environ.copy()
    for name in ('CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS', 'CONFIG_SITE'):
        env.pop(name, None)
    env.update(CC=str(MUSL), CFLAGS=FLAGS, LDFLAGS='-static -no-pie',
               CPPFLAGS='-I' + str(PREFIX / 'include'),
               PKG_CONFIG_LIBDIR=str(PREFIX / 'lib/pkgconfig'), PKG_CONFIG_PATH='')
    common = ['./configure', '--prefix=' + str(PREFIX), '--host=x86_64-linux-musl',
              '--disable-shared', '--enable-static']

    zlib = package('zlib-1.3.2.tar.xz', ['./configure', '--static', '--prefix=' + str(PREFIX)], env)

    ssl = package('openssl-3.5.9-source.tar.gz',
                  ['perl', './Configure', 'linux-x86_64', '--prefix=' + str(PREFIX),
                   '--openssldir=/etc/ssl', '--libdir=lib', 'no-shared', 'no-threads',
                   'no-dso', 'no-module', 'no-asm', 'no-tests', 'no-async', 'no-engine', 'no-afalgeng',
                   'no-secure-memory', '--with-rand-seed=getrandom'], env, 'install_sw')

    png_env = env | {'LDFLAGS': '-static -no-pie -L' + str(PREFIX / 'lib')}
    png = package('libpng-1.6.59.tar.xz', common, png_env)
    jpeg = package('jpegsrc.v10.tar.gz', common, png_env)

    freetype = package('freetype-2.14.3.tar.xz', common + [
        '--without-harfbuzz', '--without-bzip2', '--without-brotli', '--without-librsvg',
        '--with-zlib=yes', '--with-png=yes'], png_env)

    expat = package('expat-2.9.0-source.tar.gz', common + [
        '--without-xmlwf', '--without-examples', '--without-tests', '--without-docbook'], png_env)

    curl = package('curl-8.22.0.tar.xz', common + [
        '--with-openssl=' + str(PREFIX), '--with-zlib=' + str(PREFIX),
        '--with-ca-bundle=/etc/ssl/cert.pem', '--without-ca-path', '--without-ca-fallback',
        '--disable-threaded-resolver', '--disable-ipv6', '--without-libpsl',
        '--disable-unix-sockets',
        '--without-nghttp2', '--without-nghttp3', '--without-libidn2',
        '--without-brotli', '--without-zstd', '--without-libssh2', '--without-libssh',
        '--disable-ldap', '--disable-ldaps', '--disable-rtsp', '--disable-smtp',
        '--disable-pop3', '--disable-imap', '--disable-mqtt', '--disable-tftp',
        '--disable-telnet', '--disable-dict', '--disable-ftp'], png_env)

    runtime = ROOT / 'userland/runtime'
    for directory in ('include', 'lib'):
        copy_stage(PREFIX / directory, runtime / 'usr' / directory)
    for executable in ('curl', 'openssl'):
        shutil.copy2(PREFIX / 'bin' / executable, ROOT / 'userland/bin' / executable)

    ca = VENDOR / 'mozilla-ca-bundle.pem'
    if hashlib.sha256(ca.read_bytes()).hexdigest() != CA_HASH:
        raise SystemExit('CA bundle checksum mismatch')
    (runtime / 'etc/ssl').mkdir(parents=True, exist_ok=True)
    shutil.copy2(ca, runtime / 'etc/ssl/cert.pem')

    licenses = VENDOR / 'licenses'
    for source, name in [(zlib / 'LICENSE', 'zlib-LICENSE.txt'),
                         (ssl / 'LICENSE.txt', 'openssl-LICENSE.txt'),
                         (curl / 'COPYING', 'curl-COPYING.txt'),
                         (png / 'LICENSE', 'libpng-LICENSE.txt'),
                         (jpeg / 'README', 'jpeg-README.txt'),
                         (freetype / 'docs/FTL.TXT', 'freetype-FTL.txt'),
                         (freetype / 'docs/GPLv2.TXT', 'freetype-GPLv2.txt')]:
        shutil.copy2(source, licenses / name)

    shutil.copy2(expat / 'COPYING', licenses / 'expat-COPYING.txt')

    print('Built static zlib, OpenSSL LTS, libpng and curl with verified HTTPS defaults.')


def main():
    with stage_lock():
        build()


if __name__ == '__main__':
    main()
