#!/usr/bin/env python3
"""Build pinned, unmodified upstream GNU nano with static musl and ncurses.

Sources are vendored and verified, so rebuilding never requires a network.
ModernOS supplies the x86-64 syscall ABI. Linux virtual-console extensions are
disabled; curses uses its embedded VT100/xterm descriptions, not host files.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'userland/vendor'
BUILD = ROOT / 'build/vendor'
MUSL = ROOT / 'build/native-musl'
LIBS = ROOT / 'build/native-libs'
FLAGS = '-O2 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2'
PACKAGES = {
    'musl-1.2.5.tar.gz': 'a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4',
    'ncurses-6.5.tar.gz': '136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6',
    'nano-8.7.tar.xz': 'afd287aa672c48b8e1a93fdb6c6588453d527510d966822b687f2835f0d986e9',
}


def run(where, log, *command, env=None):
    with (BUILD / log).open('wb') as output:
        result = subprocess.run(command, cwd=where, env=env,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'{command[0]} failed; see {BUILD/log}')


BUILD.mkdir(parents=True, exist_ok=True)

for tool in ('gcc', 'make', 'ar', 'tic', 'infocmp'):
    if not shutil.which(tool):
        raise SystemExit(
            f'Native nano requires the host build tool {tool}. See userland/vendor/README.md.')

stamp = BUILD / 'modernos-nano.stamp'
key = hashlib.sha256(Path(__file__).read_bytes() + str(ROOT).encode() + ''.join(PACKAGES.values()).encode()).hexdigest()
binary = BUILD / 'nano-8.7/src/nano'

for filename, digest in PACKAGES.items():
    path = VENDOR / filename
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise SystemExit(f'Upstream source checksum mismatch: {path}')

if not stamp.exists() or stamp.read_text() != key or not binary.exists():
    for filename in PACKAGES:
        name = filename.split('.tar.')[0]
        target = BUILD / name
        if target.exists():
            shutil.rmtree(target)
        with tarfile.open(VENDOR / filename) as source:
            source.extractall(BUILD, filter='data')

    jobs = str(min(os.cpu_count() or 1, 8))
    env = os.environ.copy()
    for name in ('CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS', 'CONFIG_SITE'):
        env.pop(name, None)
    env['CFLAGS'] = FLAGS
    musl = BUILD / 'musl-1.2.5'
    run(musl, 'musl-configure.log', './configure', f'--prefix={MUSL}', '--disable-shared', env=env)
    run(musl, 'musl-build.log', 'make', '-j' + jobs, env=env)
    run(musl, 'musl-install.log', 'make', 'install', env=env)
    env['CC'] = str(MUSL / 'bin/musl-gcc')
    env['LDFLAGS'] = '-static -no-pie'
    curses = BUILD / 'ncurses-6.5'
    run(curses, 'ncurses-configure.log', './configure', f'--prefix={LIBS}',
        '--without-shared', '--without-debug', '--without-ada', '--without-cxx',
        '--without-cxx-binding', '--without-tests', '--without-progs', '--disable-db-install',
        '--disable-database', '--with-fallbacks=vt100,xterm', '--with-tic-path=' + shutil.which('tic'),
        '--with-infocmp-path=' + shutil.which('infocmp'), '--disable-home-terminfo', '--enable-widec', env=env)
    run(curses, 'ncurses-build.log', 'make', '-j' + jobs, env=env)
    run(curses, 'ncurses-install.log', 'make', 'install', env=env)
    env['CFLAGS'] = FLAGS + f' -U__linux__ -I{LIBS}/include -I{LIBS}/include/ncursesw'
    env['LDFLAGS'] = f'-static -no-pie -L{LIBS}/lib'
    nano = BUILD / 'nano-8.7'
    run(nano, 'nano-configure.log', './configure', '--prefix=/usr', '--disable-nls',
        '--disable-libmagic', '--disable-speller', '--disable-nanorc', '--enable-utf8', env=env)
    run(nano, 'nano-build.log', 'make', '-j' + jobs, env=env)
    stamp.write_text(key)

destination = ROOT / 'userland/bin/nano'
destination.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(binary, destination)
licenses = ROOT / 'userland/vendor/licenses'
licenses.mkdir(exist_ok=True)

for source, target in ((BUILD / 'musl-1.2.5/COPYRIGHT', 'musl-COPYRIGHT.txt'),
                       (BUILD / 'ncurses-6.5/COPYING', 'ncurses-COPYING.txt'),
                       (BUILD / 'nano-8.7/COPYING', 'nano-COPYING.txt')):
    shutil.copy2(source, licenses / target)

print('Built native GNU nano 8.7 (static musl 1.2.5 + ncurses 6.5).')
