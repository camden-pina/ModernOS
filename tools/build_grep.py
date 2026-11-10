#!/usr/bin/env python3
"""Build GNU grep with the real read fallback for Coast's absent splice syscall."""
from build_state import project_lock
from build_state import atomic_write
import argparse
import fcntl
import json
from pathlib import Path
import platform
import shutil
import subprocess

from fetch_sources import fetch
from linux_uapi import prepare as prepare_uapi
from source_archive import unpack
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'userland/vendor/linux-ports-sources.json'


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
    out = (args.output_root or ROOT / ('build/grep-native' if args.native else 'build/grep')).resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    record = next(p for p in json.loads(SOURCES.read_text())['packages'] if p['name'] == 'grep')
    vendor = out / 'vendor'
    vendor.mkdir(exist_ok=True)
    cached = ROOT / 'userland/vendor/linux-ports' / record['archive']
    if not (vendor / record['archive']).exists() and cached.is_file():
        if digest(cached) != record['sha256']:
            raise ValueError('Grep source cache checksum mismatch')
        shutil.copyfile(cached, vendor / record['archive'])
    fetch(record, vendor)
    source = out / ('grep-' + record['version'])
    if not source.exists():
        unpack(vendor / record['archive'], out)
    patch = ROOT / 'ports/gnu-runtime/grep-splice-fallback.patch'
    stamp = source / '.coast-splice'
    if not stamp.exists():
        subprocess.run(['patch', '-p1', '-i', str(patch)], cwd=source, check=True)
        stamp.write_text(digest(patch))
    if stamp.read_text() != digest(patch):
        raise ValueError('Grep patch changed; select a fresh output directory')
    headers = prepare_uapi(out / 'linux-uapi')
    prefix = '/usr/bin/' if args.native else str(args.compiler_prefix.resolve())
    flags = '-O2 -g0 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2 -idirafter ' + str(headers)
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        CC=prefix + 'gcc', AR=prefix + 'ar', RANLIB=prefix + 'ranlib',
        CFLAGS=flags, LDFLAGS='-static -no-pie', MAKEINFO='true', HELP2MAN='true')
    build, runtime = out / 'objects', out / 'runtime'
    build.mkdir(exist_ok=True)
    confinement = [] if args.native else ['bwrap', '--unshare-all', '--die-with-parent', '--new-session',
        '--ro-bind', '/', '/', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--bind', str(out), str(out)]
    commands = []

    def run(name, command):
        command = list(map(str, command))
        commands.append(command)
        print('Grep ' + name, flush=True)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run([*confinement, '--chdir', str(build), '--', *command] if confinement else command,
                cwd=build, env=environment, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, check=True)

    host = 'x86_64-linux-musl' if args.native else subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip()
    run('configure', [source / 'configure', '--prefix=/usr', '--build=' + host, '--host=x86_64-linux-musl',
        '--disable-nls', '--disable-perl-regexp', '--disable-rpath', '--enable-relocatable'])
    run('build', ['make', '-j' + str(args.jobs)])
    run('install', ['make', 'DESTDIR=' + str(runtime), 'install'])
    binary = runtime / 'usr/bin/grep'
    header = subprocess.check_output(['readelf', '-h', '-l', str(binary)], text=True)
    if 'Advanced Micro Devices X86-64' not in header or 'INTERP' in header or ' RWE ' in header:
        raise ValueError('Grep must be a static non-WX x86-64 program')
    notice = runtime / 'usr/share/licenses/grep/COPYING'
    notice.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / 'COPYING', notice)
    inputs = [Path(__file__), SOURCES, patch, ROOT / 'tools/fetch_sources.py',
        ROOT / 'tools/source_archive.py', ROOT / 'tools/linux_uapi.py']
    atomic_write(out / 'BUILD.json', json.dumps(dict(version=record['version'], source=record,
        files={str(p.relative_to(runtime)): dict(path=str(p), sha256=digest(p),
            mode=0o755 if p.stat().st_mode & 0o111 else 0o644)
            for p in sorted(runtime.rglob('*')) if p.is_file()}, commands=commands,
        source_sha256={str(p.relative_to(ROOT)): digest(p) for p in inputs},
        build_mode='native-coast' if args.native else 'linux-cross', target_host_executed=args.native,
        state='compiled; guest qualification pending'), indent=2) + '\n')
    print('Grep compiled; qualify pipe draining and Autoconf inside Coast', flush=True)


if __name__ == '__main__':
    with project_lock():
        main()
