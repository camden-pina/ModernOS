#!/usr/bin/env python3
"""Build GNU Bison for Coast from the checksum-pinned release sources."""
from build_state import project_lock
from build_state import atomic_write
import argparse
import fcntl
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

from fetch_sources import fetch
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'ports/gnu-runtime/SOURCES.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--compiler-prefix', type=Path,
        default=ROOT / 'build/compiler-threaded/cross/bin/x86_64-linux-musl-')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 24 or args.native and platform.system() != 'Coast':
        parser.error('Use --jobs 1..24; --native requires Coast')
    out = (args.output_root or ROOT / ('build/bison-native' if args.native else 'build/bison')).resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    record = json.loads(SOURCES.read_text())['bison']
    vendor = out / 'vendor'
    vendor.mkdir(exist_ok=True)
    if not (vendor / record['archive']).exists():
        shared = ROOT / 'build/gnu-runtime/vendor'
        shared.mkdir(parents=True, exist_ok=True)
        fetch(record, shared)
        shutil.copyfile(shared / record['archive'], vendor / record['archive'])
    fetch(record, vendor)
    source = out / record['source']
    if not source.exists():
        unpack(vendor / record['archive'], out)
    prefix = '/usr/bin/' if args.native else str(args.compiler_prefix.resolve())
    flags = '-O2 -g0 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2'
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        CC=prefix + 'gcc', AR=prefix + 'ar', RANLIB=prefix + 'ranlib',
        CFLAGS=flags, LDFLAGS='-static -no-pie', M4='/usr/bin/m4', MAKEINFO='true', HELP2MAN='true')
    build, runtime = out / 'objects', out / 'runtime'
    build.mkdir(exist_ok=True)
    confinement = [] if args.native else ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
        '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []

    def run(name, command):
        command = list(map(str, command))
        commands.append(command)
        print('Bison ' + name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(build), '--', *command] if confinement else command,
                cwd=build, env=environment, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)

    target = 'x86_64-linux-musl'
    host = target if args.native else subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip()
    run('configure', [source / 'configure', '--prefix=/usr', '--build=' + host, '--host=' + target,
        '--disable-nls', '--disable-rpath', '--enable-relocatable', '--without-libtextstyle-prefix'])
    run('build', ['make', '-j' + str(args.jobs)])
    run('install', ['make', 'DESTDIR=' + str(runtime), 'install'])
    binary = runtime / 'usr/bin/bison'
    header = subprocess.check_output(['readelf', '-h', '-l', str(binary)], text=True)
    if 'Advanced Micro Devices X86-64' not in header or 'INTERP' in header or ' RWE ' in header:
        raise ValueError('Bison must be a static non-WX x86-64 program')
    notice = runtime / 'usr/share/licenses/bison/COPYING'
    notice.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / 'COPYING', notice)
    inputs = [Path(__file__), SOURCES, ROOT / 'tools/fetch_sources.py', ROOT / 'tools/source_archive.py']
    atomic_write(out / 'BUILD.json', json.dumps(dict(version=record['version'], source=record,
        files={str(p.relative_to(runtime)): dict(path=str(p), sha256=digest(p),
            mode=0o755 if p.stat().st_mode & 0o111 else 0o644)
            for p in sorted(runtime.rglob('*')) if p.is_file()}, commands=commands,
        source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
        build_mode='native-coast' if args.native else 'linux-cross', target_host_executed=args.native,
        state='compiled; guest qualification pending'), indent=2) + '\n')
    print('Bison compiled; qualify parser generation inside Coast', flush=True)


if __name__ == '__main__':
    with project_lock():
        main()
