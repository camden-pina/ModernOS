#!/usr/bin/env python3
"""Build checksum-pinned GNU awk for native GCC's option generators.

BusyBox awk removes a replacement backslash needed by opt-functions.awk,
silently dropping the C++ language flags. Use the upstream GNU implementation
for both awk and gawk; no generated compiler tables are patched or substituted.
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
PACKAGE = 'gawk-5.4.0.tar.xz'
DIGEST = '3dd430f0cd3b4428c6c3f6afc021b9cd3c1f8c93f7a688dc268ca428a90b4ac1'


def main():
    source = VENDOR / PACKAGE
    if hashlib.sha256(source.read_bytes()).hexdigest() != DIGEST:
        raise SystemExit(f'Upstream source checksum mismatch: {source}')
    if not (MUSL / 'bin/musl-gcc').exists():
        raise SystemExit('Build bin/nano first for the pinned static musl toolchain.')

    BUILD.mkdir(parents=True, exist_ok=True)
    directory = BUILD / 'gawk-5.4.0'
    stamp = BUILD / 'modernos-gawk.stamp'
    key = hashlib.sha256(Path(__file__).read_bytes() + str(ROOT).encode() +
                         DIGEST.encode()).hexdigest()
    if not stamp.exists() or stamp.read_text() != key or not (directory / 'gawk').exists():
        if directory.exists():
            shutil.rmtree(directory)
        with tarfile.open(source) as archive:
            archive.extractall(BUILD, filter='data')
        env = os.environ.copy()
        for name in ('CC', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS', 'CONFIG_SITE'):
            env.pop(name, None)
        env.update(CC=str(MUSL / 'bin/musl-gcc'),
                   CFLAGS='-O2 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2',
                   LDFLAGS='-static -no-pie')
        commands = [
            ('gawk-configure.log', ['./configure', '--prefix=/usr', '--disable-nls',
                                    '--disable-extensions', '--disable-pma', '--disable-mpfr',
                                    '--without-readline', '--without-libiconv-prefix',
                                    '--without-libintl-prefix']),
            ('gawk-build.log', ['make', '-j' + str(min(os.cpu_count() or 1, 8))]),
        ]
        for name, command in commands:
            with (BUILD / name).open('wb') as output:
                result = subprocess.run(command, cwd=directory, env=env,
                                        stdout=output, stderr=subprocess.STDOUT)
            if result.returncode:
                raise SystemExit(f'{command[0]} failed; see {BUILD / name}')
        stamp.write_text(key)

    for name in ('gawk', 'awk'):
        destination = ROOT / 'userland/bin' / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(directory / 'gawk', destination)

    licenses = VENDOR / 'licenses'
    licenses.mkdir(exist_ok=True)
    shutil.copy2(directory / 'COPYING', licenses / 'gawk-COPYING.txt')

    print('Built native GNU awk 5.4.0 (static musl, no dynamic extensions/PMA/MPFR).')


if __name__ == '__main__':
    main()
