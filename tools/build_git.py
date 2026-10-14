#!/usr/bin/env python3
"""Build genuine upstream Git for ModernOS's static, single-threaded ABI.

All inputs are pinned local archives. Optional network transports are added by
the web dependency build; the local repository implementation is never mocked.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
from build_web_deps import main as build_dependencies, copy_stage, stage_lock, PREFIX as WEB_PREFIX

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/git'
PREFIX = BUILD / 'deps'
MUSL = ROOT / 'build/native-musl/bin/musl-gcc'
VENDOR = ROOT / 'userland/vendor'
PACKAGES = {
    'zlib-1.3.2.tar.xz': 'd7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3',
    'git-2.56.0.tar.xz': '26c56c296b38c0695b26fa95f475f1d01704d2d38e73465ca30b0b2f5dc789d3',
}
FLAGS = '-O2 -g0 -fno-pie -fno-stack-protector -mno-avx -mno-avx2'


def run(where, name, *args, env=None):
    log = BUILD / (name + '.log')
    with log.open('wb') as output:
        result = subprocess.run(args, cwd=where, env=env, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'{args[0]} failed; see {log}')


def build():
    BUILD.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    for name in ('CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS', 'CONFIG_SITE'):
        env.pop(name, None)
    env.update(CC=str(MUSL), CFLAGS=FLAGS, LDFLAGS='-static -no-pie')
    digest = hashlib.sha256(Path(__file__).read_bytes() + str(ROOT).encode())
    for name in ('curl', 'ssl', 'crypto', 'z'):
        digest.update((WEB_PREFIX / 'lib' / ('lib' + name + '.a')).read_bytes())
    key = digest.hexdigest()
    stamp = BUILD / '.completed'
    jobs = '-j' + str(min(os.cpu_count() or 1, 8))

    for name, digest in PACKAGES.items():
        if hashlib.sha256((VENDOR / name).read_bytes()).hexdigest() != digest:
            raise SystemExit(f'Upstream source checksum mismatch: {name}')
    if not MUSL.is_file():
        raise SystemExit('Build the musl SDK first: make -C userland bin/nano')

    stage = BUILD / 'install'
    if not stamp.exists() or stamp.read_text() != key:
        for name in PACKAGES:
            directory = BUILD / name.split('.tar.')[0]
            if directory.exists():
                shutil.rmtree(directory)
            with tarfile.open(VENDOR / name) as archive:
                archive.extractall(BUILD, filter='data')

        zlib = BUILD / 'zlib-1.3.2'
        run(zlib, 'zlib-configure', './configure', '--static', '--prefix=' + str(PREFIX), env=env)
        run(zlib, 'zlib-build', 'make', jobs, env=env)
        run(zlib, 'zlib-install', 'make', 'install', env=env)
        git = BUILD / 'git-2.56.0'
        options = [
            'prefix=/usr', 'uname_S=ModernOS', 'uname_M=x86_64',
            'SHELL_PATH=/bin/sh', 'PERL_PATH=/usr/bin/perl', 'PYTHON_PATH=/usr/bin/python3',
            'CC=' + str(MUSL), 'CFLAGS=' + FLAGS, 'LDFLAGS=-static -no-pie',
            'ZLIB_PATH=' + str(PREFIX), 'NO_PTHREADS=YesPlease', 'NO_MMAP=YesPlease',
            'CURLDIR=' + str(WEB_PREFIX), 'CURL_CONFIG=' + str(WEB_PREFIX / 'bin/curl-config'),
            'CURL_LDFLAGS=' + subprocess.check_output([str(WEB_PREFIX / 'bin/curl-config'), '--static-libs'], text=True).strip(),
            'NO_GETTEXT=YesPlease', 'NO_PERL=YesPlease', 'NO_PYTHON=YesPlease',
            'NO_TCLTK=YesPlease', 'NO_RUST=YesPlease',
            'NO_OPENSSL=YesPlease', 'NO_EXPAT=YesPlease', 'NO_UNIX_SOCKETS=YesPlease',
            'NO_REGEX=NeedsStartEnd',
            'HAVE_ALLOCA_H=YesPlease', 'HAVE_GETDELIM=YesPlease',
            'HAVE_CLOCK_GETTIME=YesPlease', 'HAVE_CLOCK_MONOTONIC=YesPlease',
            'HAVE_DEV_TTY=YesPlease', 'CSPRNG_METHOD=getrandom',
            'INSTALL_SYMLINKS=YesPlease', 'NO_INSTALL_HARDLINKS=YesPlease',
        ]
        run(git, 'git-build', 'make', jobs, *options, 'all', env=env)
        if stage.exists():
            shutil.rmtree(stage)
        run(git, 'git-install', 'make', *options, 'DESTDIR=' + str(stage), 'install', env=env)
        stamp.write_text(key)

    runtime = ROOT / 'userland/runtime'
    copy_stage(stage / 'usr', runtime / 'usr')

    # One real executable plus aliases; avoid copying 130 identical binaries.
    shutil.copy2(stage / 'usr/bin/git', ROOT / 'userland/bin/git')
    licenses = VENDOR / 'licenses'
    licenses.mkdir(exist_ok=True)
    shutil.copy2(BUILD / 'git-2.56.0/COPYING', licenses / 'git-COPYING.txt')
    shutil.copy2(BUILD / 'zlib-1.3.2/LICENSE', licenses / 'zlib-LICENSE.txt')

    print('Built native Git 2.56.0 with local repositories and certificate-verifying HTTP/HTTPS transports.')


def main():
    build_dependencies()
    with stage_lock():
        build()


if __name__ == '__main__':
    main()
