#!/usr/bin/env python3
"""Build pinned NASM for Coast and optionally a separate Linux-host copy."""
from source_archive import unpack
from build_state import project_lock
from build_state import atomic_write
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

from fetch_sources import fetch, read_manifest

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'build/nasm')
    parser.add_argument('--host', action='store_true', help='build a host tool for compiling the bootloader')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    out = args.output_root.resolve()
    out.mkdir(parents=True, exist_ok=True)
    entry = read_manifest()['nasm-2.16.03.tar.xz']
    fetch(entry)
    source = out / 'nasm-2.16.03'
    if not source.exists():
        unpack(ROOT / 'userland/vendor' / entry['archive'], out, filter='data')
    build = out / ('host-build' if args.host else 'coast-build')
    build.mkdir(exist_ok=True)
    # NASM's makefile supports VPATH but does not create object subdirectories.
    for directory in source.rglob('*'):
        if directory.is_dir():
            (build / directory.relative_to(source)).mkdir(parents=True, exist_ok=True)
    cross = ROOT / 'build/compiler/cross/bin/x86_64-linux-musl-'
    if not args.host and not Path(str(cross) + 'gcc').is_file():
        parser.error('Build the Coast musl compiler first: make -C userland runtime/usr/bin/gcc')
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        CC='gcc' if args.host else str(cross) + 'gcc',
        AR='ar' if args.host else str(cross) + 'ar',
        RANLIB='ranlib' if args.host else str(cross) + 'ranlib',
        CFLAGS='-O2 -g0 -fno-pie -fno-stack-protector -mno-avx -mno-avx2',
        LDFLAGS='-static -no-pie' if not args.host else '-no-pie')
    commands = [[str(source / 'configure'), '--prefix=/usr']]
    if not args.host:
        commands[0] += ['--host=x86_64-linux-musl', '--build=' + subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip()]
    commands += [['make', '-j' + str(args.jobs), 'nasm', 'ndisasm']]
    conf = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--ro-bind', '/', '/',
            '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out), '--chdir', str(build)]
    for index, command in enumerate(commands):
        with (build / f'{index}-build.log').open('w') as log:
            subprocess.run([*conf, '--', *command], env=environment, stdin=subprocess.DEVNULL,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
    runtime = out / ('host' if args.host else 'runtime')
    files = {}
    for name in ['nasm', 'ndisasm']:
        binary = build / name
        if not args.host:
            headers = subprocess.check_output(['readelf', '-l', str(binary)], text=True)
            if 'INTERP' in headers:
                raise ValueError('The Coast assembler must be static')
        destination = runtime / 'usr/bin' / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(binary, destination)
        files['usr/bin/' + name] = dict(path=str(destination), sha256=sha(destination), mode=0o755)
    for name, path in [('LICENSE', source / 'LICENSE'), ('source.tar.xz', ROOT / 'userland/vendor' / entry['archive'])]:
        relative = ('usr/share/licenses/nasm/LICENSE' if name == 'LICENSE'
                    else 'usr/src/coast/nasm/' + entry['archive'])
        destination = runtime / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
        files[relative] = dict(path=str(destination), sha256=sha(destination), mode=0o644)
    record = dict(version='2.16.03', source=entry, files=files, commands=commands,
                  target='linux-host' if args.host else 'coast-x86_64', target_host_executed=False)
    atomic_write(out / ('HOST.json' if args.host else 'BUILD.json'), json.dumps(record, indent=2) + '\n')
    print('NASM built:', runtime)


if __name__ == '__main__':
    with project_lock():
        main()
